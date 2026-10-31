#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <time.h>

#define MAX_TABS 9
#define MAX_LINES 1000
#define LINE_SIZE 1024
#define INPUT_SIZE 1024
#define MAX_ARGS 128
#define MAX_HISTORY 10000
#define MAX_WATCH_COMMANDS 16

#define WINDOW_WIDTH 900
#define WINDOW_HEIGHT 600
#define HEADER_HEIGHT 30
#define LEFT_MARGIN 12
#define LINE_HEIGHT 20
#define PROMPT "user@terminator> "

typedef struct {
    char lines[MAX_LINES][LINE_SIZE];
    int count;
} TerminalBuffer;

typedef struct {
    pid_t pid;
    int output_fd;
    int active;
    int pipe_eof;
    int child_reaped;
    int stopped;
    int background;
    int exit_status;
    char partial_line[LINE_SIZE];
    size_t partial_length;
    char command_text[INPUT_SIZE];
} ActiveCommand;

typedef struct {
    char text[INPUT_SIZE];
    pid_t pid;
    int output_fd;
    int active;
    int pipe_eof;
    int child_reaped;
    int exit_status;
    char partial_line[LINE_SIZE];
    size_t partial_length;
    long long next_start_ms;
} WatchCommand;

typedef struct {
    int active;
    int count;
    WatchCommand commands[MAX_WATCH_COMMANDS];
} MultiWatch;

typedef struct {
    TerminalBuffer buffer;
    char input[INPUT_SIZE];
    size_t input_length;
    size_t input_cursor;
    size_t history_position;
    char saved_input[INPUT_SIZE];
    /* Accumulates multiline commands while quotes/substitutions continue. */
    char pending_command[INPUT_SIZE];
    size_t pending_length;
    int continuation_mode;
    int suppress_next_echo;
    int previous_backslash_continuation;
    char cwd[PATH_MAX];
    ActiveCommand command;
    ActiveCommand background_command;
    MultiWatch watch;
    int scroll_offset;
    int last_status;
} TerminalTab;

typedef struct {
    char commands[MAX_HISTORY][INPUT_SIZE];
    size_t count;
    size_t next;
} CommandHistory;

static Display *display;
static Window window;
static GC gc;
static XFontStruct *font;
static Atom wm_delete;
static Atom clipboard_atom;
static Atom utf8_atom;
static Atom targets_atom;
static Atom string_atom;
static char clipboard_text[INPUT_SIZE];
static int clipboard_owned;

/* Mouse text selection: line == buffer.count represents the editable input. */
typedef struct {
    int active;
    int dragging;
    int start_line, start_col;
    int end_line, end_col;
} TextSelection;
static TextSelection text_selection;

/* Default bindings can be overridden in ~/.terminatorrc. */
typedef struct { KeySym key; unsigned int modifiers; } KeyBinding;
static KeyBinding binding_ctrl_a, binding_ctrl_e, binding_ctrl_u, binding_ctrl_k;
static KeyBinding binding_ctrl_w, binding_ctrl_l, binding_ctrl_d;
static KeyBinding binding_copy, binding_paste;

static TerminalTab tabs[MAX_TABS];
static int tab_count = 1;
static int active_tab = 0;
static int running = 1;

static int window_width = WINDOW_WIDTH;
static int window_height = WINDOW_HEIGHT;

static CommandHistory command_history;

static void stop_multiwatch(TerminalTab *tab);
static void service_tab(TerminalTab *tab);

/* ---------- Task 7: configurable hotkey bindings ---------- */
static int binding_equal(KeyBinding a, KeyBinding b)
{
    return a.key == b.key && a.modifiers == b.modifiers;
}

static int parse_binding(const char *value, KeyBinding *out)
{
    char copy[80];
    if (!value || strlen(value) >= sizeof(copy)) return 0;
    strcpy(copy, value);
    char *save = NULL;
    char *token = strtok_r(copy, "+", &save);
    unsigned int mods = 0;
    KeySym key = NoSymbol;
    while (token) {
        if (strcasecmp(token, "Ctrl") == 0 || strcasecmp(token, "Control") == 0)
            mods |= ControlMask;
        else if (strcasecmp(token, "Shift") == 0)
            mods |= ShiftMask;
        else if (strcasecmp(token, "Alt") == 0)
            mods |= Mod1Mask;
        else {
            KeySym candidate = XStringToKeysym(token);
            if (candidate == NoSymbol && strlen(token) == 1)
                candidate = (KeySym)(unsigned char)token[0];
            if (candidate == NoSymbol || key != NoSymbol) return 0;
            key = candidate;
        }
        token = strtok_r(NULL, "+", &save);
    }
    if (key == NoSymbol) return 0;
    out->key = key; out->modifiers = mods;
    return 1;
}

static void load_key_bindings(void)
{
    binding_ctrl_a = (KeyBinding){XK_a, ControlMask};
    binding_ctrl_e = (KeyBinding){XK_e, ControlMask};
    binding_ctrl_u = (KeyBinding){XK_u, ControlMask};
    binding_ctrl_k = (KeyBinding){XK_k, ControlMask};
    binding_ctrl_w = (KeyBinding){XK_w, ControlMask};
    binding_ctrl_l = (KeyBinding){XK_l, ControlMask};
    binding_ctrl_d = (KeyBinding){XK_d, ControlMask};
    binding_copy = (KeyBinding){XK_c, ControlMask | ShiftMask};
    binding_paste = (KeyBinding){XK_v, ControlMask | ShiftMask};

    const char *home = getenv("HOME");
    if (!home) return;
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/.terminatorrc", home) >= (int)sizeof(path)) return;
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[160];
    KeyBinding parsed;
    while (fgets(line, sizeof(line), fp)) {
        char *nl = strpbrk(line, "\r\n"); if (nl) *nl = '\0';
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq++ = '\0';
        char *name = line;
        while (*name == ' ' || *name == '\t') name++;
        while (*eq == ' ' || *eq == '\t') eq++;
        KeyBinding *target = NULL;
        if (!strcmp(name,"ctrl_a")) target=&binding_ctrl_a;
        else if (!strcmp(name,"ctrl_e")) target=&binding_ctrl_e;
        else if (!strcmp(name,"ctrl_u")) target=&binding_ctrl_u;
        else if (!strcmp(name,"ctrl_k")) target=&binding_ctrl_k;
        else if (!strcmp(name,"ctrl_w")) target=&binding_ctrl_w;
        else if (!strcmp(name,"ctrl_l")) target=&binding_ctrl_l;
        else if (!strcmp(name,"ctrl_d")) target=&binding_ctrl_d;
        else if (!strcmp(name,"copy")) target=&binding_copy;
        else if (!strcmp(name,"paste")) target=&binding_paste;
        if (!target) continue;
        if (!parse_binding(eq, &parsed)) {
            fprintf(stderr,"terminator: invalid binding for %s; using default\n",name);
            continue;
        }
        int conflict = 0;
        KeyBinding *all[] = {&binding_ctrl_a,&binding_ctrl_e,&binding_ctrl_u,&binding_ctrl_k,
            &binding_ctrl_w,&binding_ctrl_l,&binding_ctrl_d,&binding_copy,&binding_paste};
        for (size_t i=0;i<sizeof(all)/sizeof(all[0]);i++)
            if (all[i] != target && binding_equal(parsed,*all[i])) conflict=1;
        if (conflict) {
            fprintf(stderr,"terminator: conflicting binding for %s; using default\n",name);
            continue;
        }
        *target=parsed;
    }
    fclose(fp);
}

static int binding_pressed(XKeyEvent *event, KeySym key, KeyBinding binding)
{
    unsigned int state = event->state & (ControlMask | ShiftMask | Mod1Mask);
    KeySym actual = XLookupKeysym(event, 0);
    return actual == binding.key && state == binding.modifiers &&
           (key == binding.key || actual == binding.key);
}

static void paste_clipboard(void)
{
    /* Prefer UTF-8, which is supported by modern X11 clipboard managers. */
    XConvertSelection(display, clipboard_atom, utf8_atom, clipboard_atom, window, CurrentTime);
    XFlush(display);
}

static int selection_compare(int line_a, int col_a, int line_b, int col_b)
{
    if (line_a != line_b) return line_a < line_b ? -1 : 1;
    if (col_a != col_b) return col_a < col_b ? -1 : 1;
    return 0;
}

static void selection_bounds(int *sl, int *sc, int *el, int *ec)
{
    *sl = text_selection.start_line;
    *sc = text_selection.start_col;
    *el = text_selection.end_line;
    *ec = text_selection.end_col;
    if (selection_compare(*sl, *sc, *el, *ec) > 0) {
        int t;
        t = *sl; *sl = *el; *el = t;
        t = *sc; *sc = *ec; *ec = t;
    }
}

static int text_column_at_x(const char *text, int x)
{
    int col = 0;
    int width = 0;
    int len = (int)strlen(text);
    while (col < len) {
        int next = XTextWidth(font, text + col, 1);
        if (x < width + next / 2) break;
        width += next;
        ++col;
    }
    return col;
}

/* Convert a mouse coordinate into a visible buffer line and character column. */
static int selection_position_at(int x, int y, int *line, int *col)
{
    TerminalTab *tab = &tabs[active_tab];
    int visible_rows = (window_height - HEADER_HEIGHT - 40) / LINE_HEIGHT;
    if (visible_rows < 1) visible_rows = 1;
    int max_start = tab->buffer.count > visible_rows
                    ? tab->buffer.count - visible_rows : 0;
    int start = tab->scroll_offset == 0 ? max_start : tab->scroll_offset - 1;
    if (start < 0) start = 0;
    if (start > max_start) start = max_start;

    int row = (y - (HEADER_HEIGHT + LINE_HEIGHT) + LINE_HEIGHT / 2) / LINE_HEIGHT;
    if (row < 0) return 0;
    int buffer_line = start + row;
    if (buffer_line >= start && buffer_line < tab->buffer.count &&
        row < visible_rows) {
        *line = buffer_line;
        *col = text_column_at_x(tab->buffer.lines[buffer_line], x - LEFT_MARGIN);
        return 1;
    }

    /* Also allow selecting the editable command line. */
    int input_y = HEADER_HEIGHT + LINE_HEIGHT;
    for (int i = start; i < tab->buffer.count && input_y < window_height - LINE_HEIGHT;
         ++i, input_y += LINE_HEIGHT)
    if (y >= input_y - LINE_HEIGHT / 2 && y <= input_y + LINE_HEIGHT / 2 &&
        !(tab->command.active && !tab->command.stopped && !tab->command.background) &&
        !tab->watch.active) {
        const char *prompt = tab->continuation_mode ? "> " : PROMPT;
        int prompt_width = XTextWidth(font, prompt, (int)strlen(prompt));
        *line = tab->buffer.count;
        *col = text_column_at_x(tab->input, x - LEFT_MARGIN - prompt_width);
        return 1;
    }
    return 0;
}

static const char *selection_line_text(TerminalTab *tab, int line)
{
    return line == tab->buffer.count ? tab->input : tab->buffer.lines[line];
}

static void copy_text_selection(void)
{
    if (!text_selection.active ||
        selection_compare(text_selection.start_line, text_selection.start_col,
                          text_selection.end_line, text_selection.end_col) == 0)
        return;

    TerminalTab *tab = &tabs[active_tab];
    int sl, sc, el, ec;
    selection_bounds(&sl, &sc, &el, &ec);
    size_t used = 0;
    clipboard_text[0] = '\0';

    for (int line = sl; line <= el && used + 1 < sizeof(clipboard_text); ++line) {
        const char *text = selection_line_text(tab, line);
        size_t len = strlen(text);
        size_t begin = line == sl ? (size_t)sc : 0;
        size_t end = line == el ? (size_t)ec : len;
        if (begin > len) begin = len;
        if (end > len) end = len;
        if (end > begin) {
            size_t amount = end - begin;
            if (amount > sizeof(clipboard_text) - used - 1)
                amount = sizeof(clipboard_text) - used - 1;
            memcpy(clipboard_text + used, text + begin, amount);
            used += amount;
        }
        if (line < el && used + 1 < sizeof(clipboard_text))
            clipboard_text[used++] = '\n';
    }
    clipboard_text[used] = '\0';
    if (used == 0) return;

    XSetSelectionOwner(display, clipboard_atom, window, CurrentTime);
    clipboard_owned = XGetSelectionOwner(display, clipboard_atom) == window;
    XSetSelectionOwner(display, XA_PRIMARY, window, CurrentTime);
    XFlush(display);
}

static void draw_selection_segment(int y, const char *text, int from, int to)
{
    int len = (int)strlen(text);
    if (from < 0) from = 0;
    if (to > len) to = len;
    if (to <= from) return;
    int x1 = LEFT_MARGIN + XTextWidth(font, text, from);
    int x2 = LEFT_MARGIN + XTextWidth(font, text, to);
    XSetForeground(display, gc, BlackPixel(display, DefaultScreen(display)));
    XFillRectangle(display, window, gc, x1, y - 15,
                   (unsigned int)(x2 - x1), (unsigned int)LINE_HEIGHT);
    XSetForeground(display, gc, WhitePixel(display, DefaultScreen(display)));
    XDrawString(display, window, gc, x1, y, text + from, to - from);
    XSetForeground(display, gc, BlackPixel(display, DefaultScreen(display)));
}

static void draw_text_selection(TerminalTab *tab, int start, int y_first)
{
    if (!text_selection.active) return;
    int sl, sc, el, ec;
    selection_bounds(&sl, &sc, &el, &ec);
    int input_line = tab->buffer.count;
    for (int line = sl; line <= el; ++line) {
        int y = y_first + (line - start) * LINE_HEIGHT;
        if (line == input_line) {
            int visible_rows = (window_height - HEADER_HEIGHT - 40) / LINE_HEIGHT;
            if (visible_rows < 1) visible_rows = 1;
            int max_start = tab->buffer.count > visible_rows ?
                            tab->buffer.count - visible_rows : 0;
            int actual_start = tab->scroll_offset == 0 ? max_start : tab->scroll_offset - 1;
            int input_y = HEADER_HEIGHT + LINE_HEIGHT;
            for (int i = actual_start; i < tab->buffer.count &&
                 input_y < window_height - LINE_HEIGHT; ++i)
                input_y += LINE_HEIGHT;
            y = input_y;
            const char *prompt = tab->continuation_mode ? "> " : PROMPT;
            int prompt_width = XTextWidth(font, prompt, (int)strlen(prompt));
            int from = line == sl ? sc : 0;
            int to = line == el ? ec : (int)strlen(tab->input);
            int len = (int)strlen(tab->input);
            if (from < len && to > from) {
                int x1 = LEFT_MARGIN + prompt_width + XTextWidth(font, tab->input, from);
                int x2 = LEFT_MARGIN + prompt_width + XTextWidth(font, tab->input, to);
                XSetForeground(display, gc, BlackPixel(display, DefaultScreen(display)));
                XFillRectangle(display, window, gc, x1, y - 15,
                               (unsigned int)(x2 - x1), (unsigned int)LINE_HEIGHT);
                XSetForeground(display, gc, WhitePixel(display, DefaultScreen(display)));
                XDrawString(display, window, gc, x1, y, tab->input + from, to - from);
                XSetForeground(display, gc, BlackPixel(display, DefaultScreen(display)));
            }
            continue;
        }
        if (line < start || line >= tab->buffer.count) continue;
        const char *text = tab->buffer.lines[line];
        int from = line == sl ? sc : 0;
        int to = line == el ? ec : (int)strlen(text);
        draw_selection_segment(y, text, from, to);
    }
}

/* ---------- Terminal display buffer ---------- */

static void add_line(TerminalBuffer *buffer, const char *text)
{
    if (buffer->count >= MAX_LINES) {
        memmove(buffer->lines, buffer->lines + 1,
                (MAX_LINES - 1) * sizeof(buffer->lines[0]));
        buffer->count = MAX_LINES - 1;
    }

    snprintf(buffer->lines[buffer->count], LINE_SIZE, "%s",
             text ? text : "");
    buffer->count++;
}

static void add_format(TerminalBuffer *buffer, const char *format,
                       const char *argument)
{
    char message[LINE_SIZE];
    snprintf(message, sizeof(message), format, argument);
    add_line(buffer, message);
}

static void consume_output(TerminalTab *tab, ActiveCommand *cmd,
                           const char *data, size_t length)
{

    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)data[i];

        if (ch == '\r')
            continue;

        if (ch == '\n') {
            cmd->partial_line[cmd->partial_length] = '\0';
            add_line(&tab->buffer, cmd->partial_line);
            cmd->partial_length = 0;
            cmd->partial_line[0] = '\0';
        } else if (ch == '\t') {
            for (int j = 0; j < 4; j++) {
                if (cmd->partial_length < LINE_SIZE - 1)
                    cmd->partial_line[cmd->partial_length++] = ' ';
            }
        } else if (ch >= 32 || ch == '\b') {
            if (cmd->partial_length < LINE_SIZE - 1)
                cmd->partial_line[cmd->partial_length++] = (char)ch;
        }
    }
}

static void flush_partial_output(TerminalTab *tab, ActiveCommand *cmd)
{

    if (cmd->partial_length == 0)
        return;

    cmd->partial_line[cmd->partial_length] = '\0';
    add_line(&tab->buffer, cmd->partial_line);
    cmd->partial_length = 0;
    cmd->partial_line[0] = '\0';
}

/* ---------- Shared command history for this single-process version ---------- */

static void history_add(const char *command)
{
    if (command == NULL || command[0] == '\0')
        return;

    snprintf(command_history.commands[command_history.next],
             INPUT_SIZE, "%s", command);

    command_history.next =
        (command_history.next + 1) % MAX_HISTORY;

    if (command_history.count < MAX_HISTORY)
        command_history.count++;
}

static void record_command_line(TerminalTab *tab, const char *command)
{
    if (tab->suppress_next_echo) {
        tab->suppress_next_echo = 0;
    } else {
        char submitted[LINE_SIZE];
        snprintf(submitted, sizeof(submitted), "%s%.1000s", PROMPT, command);
        add_line(&tab->buffer, submitted);
    }
    history_add(command);
}

static void show_history(TerminalTab *tab)
{
    size_t shown = command_history.count;

    if (shown > 1000)
        shown = 1000;

    size_t first = (command_history.next + MAX_HISTORY
                    - shown) % MAX_HISTORY;

    char line[LINE_SIZE];

    for (size_t i = 0; i < shown; i++) {
        size_t index = (first + i) % MAX_HISTORY;

        snprintf(line, sizeof(line), "%zu  %.1000s",
                 i + 1, command_history.commands[index]);

        add_line(&tab->buffer, line);
    }

    tab->last_status = 0;
}

/* ---------- Basic command parser ---------- */

/*
 * Handles whitespace, single/double quotes and basic backslash escaping.
 * It is not a full Bash parser: pipes, redirection and substitutions
 * are not parsed here.
 */
static int parse_command(char *command, char *argv[], int max_args)
{
    char *read_pos = command;
    char *write_pos = command;
    int argc = 0;

    while (*read_pos != '\0') {
        while (*read_pos == ' ' || *read_pos == '\t' ||
               *read_pos == '\n') {
            read_pos++;
        }

        if (*read_pos == '\0')
            break;

        if (argc >= max_args - 1)
            break;

        argv[argc++] = write_pos;
        char quote = '\0';

        while (*read_pos != '\0') {
            char ch = *read_pos++;

            if (quote == '\0' &&
                (ch == ' ' || ch == '\t' || ch == '\n')) {
                break;
            }

            if (quote == '\0' && (ch == '\'' || ch == '"')) {
                quote = ch;
                continue;
            }

            if (quote != '\0' && ch == quote) {
                quote = '\0';
                continue;
            }

            /*
             * Preserve printf escapes such as \\n and \\t inside
             * double quotes. In double quotes, Bash removes the
             * backslash only before $, `, ", \\ and newline.
             */
            if (ch == '\\' && quote != '\'') {
                if (*read_pos == '\0') {
                    *write_pos++ = '\\';
                    break;
                }

                if (quote == '"') {
                    if (*read_pos == '$' || *read_pos == '`' ||
                        *read_pos == '"' || *read_pos == '\\') {
                        *write_pos++ = *read_pos++;
                    } else if (*read_pos == '\n') {
                        read_pos++;
                    } else {
                        *write_pos++ = '\\';
                        *write_pos++ = *read_pos++;
                    }
                } else {
                    *write_pos++ = *read_pos++;
                }
                continue;
            }

            *write_pos++ = ch;
        }

        if (quote != '\0')
            return -1;

        *write_pos++ = '\0';
    }

    argv[argc] = NULL;
    return argc;
}

/* Replace occurrences of $? with the tab's previous command status. */
static void expand_status(char *destination, size_t capacity,
                          const char *source, int status)
{
    char status_text[32];
    snprintf(status_text, sizeof(status_text), "%d", status);

    size_t out = 0;

    for (size_t i = 0; source[i] != '\0' && out + 1 < capacity; ) {
        if (source[i] == '$' && source[i + 1] == '?') {
            for (size_t j = 0;
                 status_text[j] != '\0' && out + 1 < capacity; j++)
                destination[out++] = status_text[j];

            i += 2;
        } else {
            destination[out++] = source[i++];
        }
    }

    destination[out] = '\0';
}

/* ---------- Per-tab working directory ---------- */

/*
 * chdir() in the GUI process would affect every tab. Validate the
 * requested directory in a temporary child and store its resolved path.
 */
static int change_directory(TerminalTab *tab, const char *path)
{
    char target[PATH_MAX];

    if (path == NULL || path[0] == '\0' || strcmp(path, "~") == 0) {
        const char *home = getenv("HOME");

        if (home == NULL || home[0] == '\0') {
            add_line(&tab->buffer, "cd: HOME is not set");
            tab->last_status = 1;
            return -1;
        }

        if (snprintf(target, sizeof(target), "%s", home)
            >= (int)sizeof(target)) {
            add_line(&tab->buffer, "cd: path is too long");
            tab->last_status = 1;
            return -1;
        }
    } else if (path[0] == '~' && path[1] == '/') {
        const char *home = getenv("HOME");

        if (home == NULL)
            home = "/";

        if (snprintf(target, sizeof(target), "%s%s",
                     home, path + 1) >= (int)sizeof(target)) {
            add_line(&tab->buffer, "cd: path is too long");
            tab->last_status = 1;
            return -1;
        }
    } else if (path[0] == '/') {
        if (snprintf(target, sizeof(target), "%s", path)
            >= (int)sizeof(target)) {
            add_line(&tab->buffer, "cd: path is too long");
            tab->last_status = 1;
            return -1;
        }
    } else {
        if (snprintf(target, sizeof(target), "%s/%s",
                     tab->cwd, path) >= (int)sizeof(target)) {
            add_line(&tab->buffer, "cd: path is too long");
            tab->last_status = 1;
            return -1;
        }
    }

    int pipefd[2];

    if (pipe(pipefd) == -1) {
        add_format(&tab->buffer, "cd: pipe: %s", strerror(errno));
        tab->last_status = 1;
        return -1;
    }

    pid_t pid = fork();

    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        add_format(&tab->buffer, "cd: fork: %s", strerror(errno));
        tab->last_status = 1;
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);

        if (chdir(target) == -1) {
            dprintf(pipefd[1], "ERROR:%s", strerror(errno));
        } else {
            char resolved[PATH_MAX];

            if (getcwd(resolved, sizeof(resolved)) != NULL)
                dprintf(pipefd[1], "%s", resolved);
            else
                dprintf(pipefd[1], "ERROR:%s", strerror(errno));
        }

        close(pipefd[1]);
        _exit(0);
    }

    close(pipefd[1]);

    char result[PATH_MAX + 64];
    size_t used = 0;

    while (used < sizeof(result) - 1) {
        ssize_t n = read(pipefd[0], result + used,
                         sizeof(result) - 1 - used);

        if (n > 0)
            used += (size_t)n;
        else if (n == -1 && errno == EINTR)
            continue;
        else
            break;
    }

    close(pipefd[0]);

    while (waitpid(pid, NULL, 0) == -1 && errno == EINTR)
        ;

    result[used] = '\0';

    if (used == 0) {
        add_line(&tab->buffer, "cd: could not resolve directory");
        tab->last_status = 1;
        return -1;
    }

    if (strncmp(result, "ERROR:", 6) == 0) {
        char message[LINE_SIZE];
        snprintf(message, sizeof(message), "cd: %.1000s", result + 6);
        add_line(&tab->buffer, message);
        tab->last_status = 1;
        return -1;
    }

    if (strlen(result) >= sizeof(tab->cwd)) {
        add_line(&tab->buffer, "cd: resolved path is too long");
        tab->last_status = 1;
        return -1;
    }

    memcpy(tab->cwd, result, strlen(result) + 1);
    tab->last_status = 0;
    return 0;
}

/* ---------- Tab initialization and cleanup ---------- */

static void init_tab(TerminalTab *tab)
{
    memset(tab, 0, sizeof(*tab));

    tab->command.pid = -1;
    tab->command.output_fd = -1;
    tab->background_command.pid = -1;
    tab->background_command.output_fd = -1;
    tab->last_status = 0;

    if (getcwd(tab->cwd, sizeof(tab->cwd)) == NULL)
        snprintf(tab->cwd, sizeof(tab->cwd), "/");

    add_line(&tab->buffer, "Terminator - Custom Terminal");
    add_line(&tab->buffer,
             "Ctrl+T new tab | Ctrl+W close tab | Ctrl+1..9 switch");
}

static void stop_one_command(ActiveCommand *cmd)
{
    if (cmd->active && cmd->pid > 0) {
        (void)kill(-cmd->pid, SIGTERM); (void)kill(cmd->pid, SIGTERM);
        while (waitpid(cmd->pid, NULL, 0) == -1 && errno == EINTR) ;
    }
    if (cmd->output_fd >= 0) close(cmd->output_fd);
    memset(cmd, 0, sizeof(*cmd)); cmd->pid = -1; cmd->output_fd = -1;
}
static void stop_tab_command(TerminalTab *tab)
{
    stop_multiwatch(tab);
    stop_one_command(&tab->command);
    stop_one_command(&tab->background_command);
}

static void close_tab(int index)
{
    if (index < 0 || index >= tab_count)
        return;

    if (tab_count == 1) {
        add_line(&tabs[0].buffer,
                 "Cannot close the last tab. Use exit or Escape.");
        return;
    }

    stop_tab_command(&tabs[index]);

    for (int i = index; i < tab_count - 1; i++)
        tabs[i] = tabs[i + 1];

    tab_count--;

    if (active_tab >= tab_count)
        active_tab = tab_count - 1;
    else if (active_tab > index)
        active_tab--;
}

/* Add spaces around shell redirection operators, except inside quotes. */
static void normalize_redirections(char *dst, size_t cap, const char *src)
{
    size_t out = 0;
    char quote = '\0';
    for (size_t i = 0; src[i] && out + 4 < cap; ++i) {
        char c = src[i];
        if (c == '\\' && quote != '\'') {
            dst[out++] = c;
            if (src[i + 1] && out + 2 < cap) dst[out++] = src[++i];
            continue;
        }
        if ((c == '\'' || c == '"') && (quote == '\0' || quote == c)) {
            quote = quote == '\0' ? c : '\0';
            dst[out++] = c;
            continue;
        }
        if (quote == '\0' && (c == '>' || c == '<')) {
            if (out && dst[out - 1] != ' ' && dst[out - 1] != '\t') dst[out++] = ' ';
            dst[out++] = c;
            if (c == '>' && src[i + 1] == '>') dst[out++] = src[++i];
            if (src[i + 1] && src[i + 1] != ' ' && src[i + 1] != '\t') dst[out++] = ' ';
        } else dst[out++] = c;
    }
    dst[out] = '\0';
}


#define MAX_PIPE_STAGES 32

/* Find a real pipe operator, ignoring quoted or escaped '|'. */
static int has_pipeline(const char *s)
{
    char quote = '\0';
    for (size_t i = 0; s[i]; ++i) {
        if (s[i] == '\\' && quote != '\'') {
            if (s[i + 1]) ++i;
            continue;
        }
        if ((s[i] == '\'' || s[i] == '"') &&
            (quote == '\0' || quote == s[i])) {
            quote = quote == '\0' ? s[i] : '\0';
            continue;
        }
        if (s[i] == '|' && quote == '\0') return 1;
    }
    return 0;
}

static int pipeline_stage_parse(char *text, char *argv[],
                                char **input_path, char **output_path,
                                int *append_output)
{
    char normalized[INPUT_SIZE];
    char copy[INPUT_SIZE];
    normalize_redirections(normalized, sizeof(normalized), text);
    snprintf(copy, sizeof(copy), "%s", normalized);
    int argc = parse_command(copy, argv, MAX_ARGS);
    if (argc <= 0) return argc;

    *input_path = NULL;
    *output_path = NULL;
    *append_output = 0;
    int clean_argc = 0;
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "<") == 0 || strcmp(argv[i], ">") == 0 ||
            strcmp(argv[i], ">>") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') return -1;
            if (strcmp(argv[i], "<") == 0) {
                if (*input_path) return -1;
                *input_path = argv[++i];
            } else {
                if (*output_path) return -1;
                *append_output = strcmp(argv[i], ">>") == 0;
                *output_path = argv[++i];
            }
        } else {
            argv[clean_argc++] = argv[i];
        }
    }
    argv[clean_argc] = NULL;
    return clean_argc;
}

/* Runs inside a supervisor child so the GUI can track one PID asynchronously. */
static void run_pipeline_child(const char *cwd, const char *command)
{
    char stages[MAX_PIPE_STAGES][INPUT_SIZE];
    int count = 0;
    char quote = '\0';
    size_t start = 0, len = strlen(command);

    for (size_t i = 0; i <= len; ++i) {
        char c = command[i];
        if (c == '\\' && quote != '\'' && i < len) { ++i; continue; }
        if ((c == '\'' || c == '"') &&
            (quote == '\0' || quote == c)) {
            quote = quote == '\0' ? c : '\0';
            continue;
        }
        if ((c == '|' && quote == '\0') || c == '\0') {
            size_t n = i - start;
            while (n && (command[start] == ' ' || command[start] == '\t')) { ++start; --n; }
            while (n && (command[start+n-1] == ' ' || command[start+n-1] == '\t')) --n;
            if (n == 0 || count >= MAX_PIPE_STAGES) {
                dprintf(STDERR_FILENO, "terminator: invalid or too many pipeline stages\n");
                _exit(2);
            }
            if (n >= INPUT_SIZE) n = INPUT_SIZE - 1;
            memcpy(stages[count], command + start, n);
            stages[count][n] = '\0';
            ++count;
            start = i + 1;
        }
    }
    if (quote != '\0') {
        dprintf(STDERR_FILENO, "Syntax error: unmatched quote.\n");
        _exit(2);
    }
    if (chdir(cwd) == -1) { dprintf(STDERR_FILENO, "chdir: %s\n", strerror(errno)); _exit(126); }

    pid_t pids[MAX_PIPE_STAGES];
    int previous_read = -1;
    pid_t pgid = 0;
    int final_status = 1;

    for (int i = 0; i < count; ++i) {
        int next_pipe[2] = {-1, -1};
        if (i < count - 1 && pipe(next_pipe) == -1) {
            dprintf(STDERR_FILENO, "pipe: %s\n", strerror(errno));
            if (previous_read >= 0) close(previous_read);
            _exit(1);
        }

        char *argv[MAX_ARGS];
        char *input_path = NULL, *output_path = NULL;
        int append_output = 0;
        int argc = pipeline_stage_parse(stages[i], argv, &input_path,
                                        &output_path, &append_output);
        if (argc <= 0) {
            dprintf(STDERR_FILENO, "Syntax error in pipeline stage %d\n", i + 1);
            if (previous_read >= 0) close(previous_read);
            if (next_pipe[0] >= 0) close(next_pipe[0]);
            if (next_pipe[1] >= 0) close(next_pipe[1]);
            _exit(2);
        }

        pid_t pid = fork();
        if (pid == -1) {
            dprintf(STDERR_FILENO, "fork: %s\n", strerror(errno));
            _exit(1);
        }
        if (pid == 0) {
            if (pgid > 0) (void)setpgid(0, pgid);
            if (previous_read >= 0 && dup2(previous_read, STDIN_FILENO) == -1) _exit(126);
            if (next_pipe[1] >= 0 && dup2(next_pipe[1], STDOUT_FILENO) == -1) _exit(126);
            if (previous_read >= 0) close(previous_read);
            if (next_pipe[0] >= 0) close(next_pipe[0]);
            if (next_pipe[1] >= 0) close(next_pipe[1]);

            if (input_path) {
                int fd = open(input_path, O_RDONLY);
                if (fd == -1) { dprintf(STDERR_FILENO, "%s: %s\n", input_path, strerror(errno)); _exit(1); }
                if (dup2(fd, STDIN_FILENO) == -1) _exit(126);
                close(fd);
            }
            if (output_path) {
                int flags = O_WRONLY | O_CREAT | (append_output ? O_APPEND : O_TRUNC);
                int fd = open(output_path, flags, 0666);
                if (fd == -1) { dprintf(STDERR_FILENO, "%s: %s\n", output_path, strerror(errno)); _exit(1); }
                if (dup2(fd, STDOUT_FILENO) == -1) _exit(126);
                close(fd);
            }
            execvp(argv[0], argv);
            int exec_error = errno;
            if (exec_error == ENOENT)
                dprintf(STDERR_FILENO, "terminator: %s: command not found\n", argv[0]);
            else
                dprintf(STDERR_FILENO, "terminator: %s: %s\n",
                        argv[0], strerror(exec_error));
            _exit(exec_error == ENOENT ? 127 : 126);
        }
        if (pgid == 0) pgid = pid;
        (void)setpgid(pid, pgid);
        pids[i] = pid;
        if (previous_read >= 0) close(previous_read);
        if (next_pipe[1] >= 0) close(next_pipe[1]);
        previous_read = next_pipe[0];
    }
    if (previous_read >= 0) close(previous_read);

    /*
     * Use pipefail-style reporting: preserve the status of the rightmost
     * failing stage instead of reporting success merely because the final
     * stage (for example, sort) succeeded.
     */
    final_status = 0;
    for (int i = 0; i < count; ++i) {
        int status = 0;
        while (waitpid(pids[i], &status, 0) == -1) {
            if (errno == EINTR)
                continue;
            status = 1 << 8;
            break;
        }

        int stage_status;
        if (WIFEXITED(status))
            stage_status = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            stage_status = 128 + WTERMSIG(status);
        else
            stage_status = 1;

        if (stage_status != 0)
            final_status = stage_status;
    }
    _exit(final_status);
}

static void start_pipeline(TerminalTab *tab, const char *original,
                           const char *expanded)
{
    record_command_line(tab, original);

    int output_pipe[2];
    if (pipe(output_pipe) == -1) {
        add_format(&tab->buffer, "pipe: %s", strerror(errno));
        tab->last_status = 1;
        return;
    }
    pid_t pid = fork();
    if (pid == -1) {
        close(output_pipe[0]); close(output_pipe[1]);
        add_format(&tab->buffer, "fork: %s", strerror(errno));
        tab->last_status = 1;
        return;
    }
    if (pid == 0) {
        (void)setpgid(0, 0);
        close(output_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) == -1 ||
            dup2(output_pipe[1], STDERR_FILENO) == -1) _exit(126);
        close(output_pipe[1]);
        run_pipeline_child(tab->cwd, expanded);
    }
    (void)setpgid(pid, pid);
    close(output_pipe[1]);
    int flags = fcntl(output_pipe[0], F_GETFL, 0);
    if (flags == -1 || fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK) == -1) {
        close(output_pipe[0]); (void)kill(-pid, SIGTERM); (void)waitpid(pid, NULL, 0);
        add_line(&tab->buffer, "Error configuring pipeline output pipe");
        tab->last_status = 1;
        return;
    }
    tab->command.pid = pid;
    tab->command.output_fd = output_pipe[0];
    tab->command.active = 1;
    tab->command.pipe_eof = 0;
    tab->command.child_reaped = 0;
    tab->command.stopped = 0;
    tab->command.background = 0;
    tab->command.partial_length = 0;
    tab->command.partial_line[0] = '\0';
    tab->command.exit_status = 0;
    snprintf(tab->command.command_text, sizeof(tab->command.command_text), "%s", original);
}

/* ---------- multiWatch support ---------- */

#define WATCH_SEPARATOR "----------------------------------------"

static long long monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
        return 0;
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void watch_flush_partial(TerminalTab *tab, WatchCommand *wc)
{
    if (wc->partial_length == 0)
        return;
    wc->partial_line[wc->partial_length] = '\0';
    add_line(&tab->buffer, wc->partial_line);
    wc->partial_length = 0;
    wc->partial_line[0] = '\0';
}

static void watch_consume_output(TerminalTab *tab, WatchCommand *wc,
                                 const char *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)data[i];
        if (ch == '\r')
            continue;
        if (ch == '\n') {
            wc->partial_line[wc->partial_length] = '\0';
            add_line(&tab->buffer, wc->partial_line);
            wc->partial_length = 0;
            wc->partial_line[0] = '\0';
        } else if (ch >= 32 || ch == '\t') {
            if (wc->partial_length < sizeof(wc->partial_line) - 1)
                wc->partial_line[wc->partial_length++] =
                    ch == '\t' ? ' ' : (char)ch;
        }
    }
}

static int launch_watch_command(TerminalTab *tab, WatchCommand *wc)
{
    int pfd[2];
    if (pipe(pfd) == -1) {
        char msg[LINE_SIZE];
        snprintf(msg, sizeof(msg), "multiWatch: pipe: %s", strerror(errno));
        add_line(&tab->buffer, msg);
        wc->next_start_ms = monotonic_ms() + 2000;
        return -1;
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(pfd[0]);
        close(pfd[1]);
        char msg[LINE_SIZE];
        snprintf(msg, sizeof(msg), "multiWatch: fork: %s", strerror(errno));
        add_line(&tab->buffer, msg);
        wc->next_start_ms = monotonic_ms() + 2000;
        return -1;
    }

    if (pid == 0) {
        (void)setpgid(0, 0);
        close(pfd[0]);
        if (dup2(pfd[1], STDOUT_FILENO) == -1 ||
            dup2(pfd[1], STDERR_FILENO) == -1)
            _exit(126);
        close(pfd[1]);
        if (chdir(tab->cwd) == -1) {
            dprintf(STDERR_FILENO, "chdir: %s\n", strerror(errno));
            _exit(126);
        }
        execl("/bin/sh", "sh", "-c", wc->text, (char *)NULL);
        dprintf(STDERR_FILENO, "multiWatch: cannot start shell: %s\n",
                strerror(errno));
        _exit(127);
    }

    (void)setpgid(pid, pid);
    close(pfd[1]);
    int flags = fcntl(pfd[0], F_GETFL, 0);
    if (flags == -1 || fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK) == -1) {
        close(pfd[0]);
        (void)kill(-pid, SIGTERM);
        while (waitpid(pid, NULL, 0) == -1 && errno == EINTR)
            ;
        add_line(&tab->buffer, "multiWatch: could not configure output pipe");
        wc->next_start_ms = monotonic_ms() + 2000;
        return -1;
    }

    wc->pid = pid;
    wc->output_fd = pfd[0];
    wc->active = 1;
    wc->pipe_eof = 0;
    wc->child_reaped = 0;
    wc->exit_status = 0;
    wc->partial_length = 0;
    wc->partial_line[0] = '\0';

    time_t now = time(NULL);
    struct tm tm_now;
    char timestamp[64] = "unknown-time";
    if (localtime_r(&now, &tm_now) != NULL)
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_now);
    char header[LINE_SIZE];
    snprintf(header, sizeof(header), "\"%.700s\" | Unix timestamp: %lld (%s)",
             wc->text, (long long)now, timestamp);
    add_line(&tab->buffer, header);
    add_line(&tab->buffer, WATCH_SEPARATOR);
    return 0;
}

static void stop_multiwatch(TerminalTab *tab)
{
    MultiWatch *mw = &tab->watch;
    if (!mw->active)
        return;
    for (int i = 0; i < mw->count; ++i) {
        WatchCommand *wc = &mw->commands[i];
        if (wc->active && wc->pid > 0) {
            (void)kill(-wc->pid, SIGTERM);
            (void)kill(wc->pid, SIGTERM);
        }
    }
    /* Force termination after requesting a graceful stop, avoiding a stuck GUI. */
    for (int i = 0; i < mw->count; ++i) {
        WatchCommand *wc = &mw->commands[i];
        if (wc->active && wc->pid > 0) {
            (void)kill(-wc->pid, SIGKILL);
            (void)kill(wc->pid, SIGKILL);
        }
    }
    for (int i = 0; i < mw->count; ++i) {
        WatchCommand *wc = &mw->commands[i];
        if (wc->active && wc->pid > 0) {
            while (waitpid(wc->pid, NULL, 0) == -1 && errno == EINTR)
                ;
        }
        if (wc->output_fd >= 0)
            close(wc->output_fd);
        wc->output_fd = -1;
        wc->pid = -1;
        wc->active = 0;
    }
    mw->active = 0;
    mw->count = 0;
    add_line(&tab->buffer, "multiWatch stopped (Ctrl+C).");
    tab->last_status = 130;
}

/* Parse multiWatch ["cmd one", "cmd two"] without splitting quoted commands. */
static int parse_multiwatch_list(const char *input, MultiWatch *mw)
{
    const char *p = input;
    while (*p == ' ' || *p == '\t') ++p;
    if (strncmp(p, "multiWatch", 10) != 0 ||
        (p[10] != ' ' && p[10] != '\t' && p[10] != '['))
        return 0;
    p += 10;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p++ != '[')
        return -1;

    memset(mw, 0, sizeof(*mw));
    while (1) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',') ++p;
        if (*p == ']') {
            ++p;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p != '\0') return -1;
            break;
        }
        if (*p != '\'' && *p != '"') return -1;
        char quote = *p++;
        if (mw->count >= MAX_WATCH_COMMANDS) return -1;
        WatchCommand *wc = &mw->commands[mw->count];
        memset(wc, 0, sizeof(*wc));
        wc->pid = -1;
        wc->output_fd = -1;
        size_t n = 0;
        while (*p && *p != quote) {
            if (*p == '\\' && p[1] != '\0') {
                ++p;
                if (*p == 'n') { if (n + 1 >= sizeof(wc->text)) return -1; wc->text[n++] = '\n'; ++p; continue; }
                if (*p == 't') { if (n + 1 >= sizeof(wc->text)) return -1; wc->text[n++] = '\t'; ++p; continue; }
            }
            if (n + 1 >= sizeof(wc->text)) return -1;
            wc->text[n++] = *p++;
        }
        if (*p != quote) return -1;
        ++p;
        wc->text[n] = '\0';
        if (n == 0) return -1;
        ++mw->count;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == ',') { ++p; continue; }
        if (*p == ']') continue;
        return -1;
    }
    return mw->count > 0 ? 1 : -1;
}

static void start_multiwatch(TerminalTab *tab, const char *original)
{
    record_command_line(tab, original);
    MultiWatch parsed;
    int parsed_result = parse_multiwatch_list(original, &parsed);
    if (parsed_result < 0) {
        add_line(&tab->buffer,
                 "multiWatch syntax: multiWatch [\"date\", \"uptime\"]");
        tab->last_status = 2;
        return;
    }
    if (tab->command.active || tab->background_command.active || tab->watch.active) {
        add_line(&tab->buffer, "A command or multiWatch is already running in this tab.");
        tab->last_status = 1;
        return;
    }
    tab->watch = parsed;
    tab->watch.active = 1;
    add_line(&tab->buffer, "multiWatch started. Press Ctrl+C to stop watched commands.");
    for (int i = 0; i < tab->watch.count; ++i)
        (void)launch_watch_command(tab, &tab->watch.commands[i]);
    tab->last_status = 0;
}

/* Read watched output, reap children, and relaunch completed commands after 2s. */
static void service_multiwatch(TerminalTab *tab)
{
    MultiWatch *mw = &tab->watch;
    if (!mw->active)
        return;
    long long now_ms = monotonic_ms();
    for (int i = 0; i < mw->count; ++i) {
        WatchCommand *wc = &mw->commands[i];
        if (wc->active && !wc->pipe_eof && wc->output_fd >= 0) {
            char data[2048];
            for (;;) {
                ssize_t n = read(wc->output_fd, data, sizeof(data));
                if (n > 0) {
                    watch_consume_output(tab, wc, data, (size_t)n);
                } else if (n == 0) {
                    wc->pipe_eof = 1;
                    close(wc->output_fd);
                    wc->output_fd = -1;
                    break;
                } else if (errno == EINTR) {
                    continue;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else {
                    wc->pipe_eof = 1;
                    close(wc->output_fd);
                    wc->output_fd = -1;
                    break;
                }
            }
        }
        if (wc->active && !wc->child_reaped) {
            int status = 0;
            pid_t r = waitpid(wc->pid, &status, WNOHANG);
            if (r == wc->pid) {
                wc->child_reaped = 1;
                if (WIFEXITED(status)) wc->exit_status = WEXITSTATUS(status);
                else if (WIFSIGNALED(status)) wc->exit_status = 128 + WTERMSIG(status);
                else wc->exit_status = 1;
            } else if (r == -1 && errno != EINTR) {
                wc->child_reaped = 1;
                wc->exit_status = 1;
            }
        }
        if (wc->active && wc->pipe_eof && wc->child_reaped) {
            watch_flush_partial(tab, wc);
            add_line(&tab->buffer, WATCH_SEPARATOR);
            char status_line[LINE_SIZE];
            snprintf(status_line, sizeof(status_line), "[%s completed; exit status %d]",
                     wc->text, wc->exit_status);
            add_line(&tab->buffer, status_line);
            add_line(&tab->buffer, WATCH_SEPARATOR);
            wc->active = 0;
            wc->pid = -1;
            wc->next_start_ms = now_ms + 2000;
        }
        if (!wc->active && now_ms >= wc->next_start_ms)
            (void)launch_watch_command(tab, wc);
    }
}

/* ---------- External command execution ---------- */

static int command_needs_bash(const char *s);
static void start_bash_command(TerminalTab *tab, const char *original,
                               const char *command);

/* Returns true while a quote, backtick, or $(...) remains unfinished. */
static int command_is_incomplete(const char *s)
{
    char quote = '\0';
    int backtick = 0, depth = 0, escaped = 0;
    for (size_t i = 0; s[i]; ++i) {
        char c = s[i];
        if (escaped) { escaped = 0; continue; }
        if (c == '\\' && quote != '\'') { escaped = 1; continue; }
        if (backtick) { if (c == '`') backtick = 0; continue; }
        if (quote == '\'') { if (c == '\'') quote = '\0'; continue; }
        if (c == '`') { backtick = 1; continue; }
        if (c == '"' && (quote == '\0' || quote == '"')) {
            quote = quote == '"' ? '\0' : '"';
            continue;
        }
        if (c == '\'' && quote == '\0') { quote = '\''; continue; }
        if (c == '$' && s[i + 1] == '(') { ++depth; ++i; continue; }
        if (depth > 0 && c == '(') ++depth;
        else if (depth > 0 && c == ')') --depth;
    }
    return quote != '\0' || backtick || depth > 0 || escaped;
}

/* Respect single quotes when deciding whether Bash expansion is required. */
static int command_needs_bash(const char *s)
{
    char quote = '\0';
    int escaped = 0;
    for (size_t i = 0; s[i]; ++i) {
        char c = s[i];
        if (escaped) { escaped = 0; continue; }
        if (c == '\\' && quote != '\'') { escaped = 1; continue; }
        if (quote == '\'') { if (c == '\'') quote = '\0'; continue; }
        if (c == '\'' && quote == '\0') { quote = '\''; continue; }
        if (c == '"' && (quote == '\0' || quote == '"')) {
            quote = quote == '"' ? '\0' : '"';
            continue;
        }
        /*
         * Delegate unquoted pathname patterns to Bash. This is important
         * for both standalone commands and pipeline stages: execvp() does
         * not expand patterns such as *.txt by itself.
         */
        if (quote == '\0' && (c == '*' || c == '?' || c == '['))
            return 1;

        if (c == '`' || (c == '$' &&
            (s[i + 1] == '(' || s[i + 1] == '{' ||
             (s[i + 1] >= 'A' && s[i + 1] <= 'Z') ||
             (s[i + 1] >= 'a' && s[i + 1] <= 'z') || s[i + 1] == '_')))
            return 1;
    }
    return 0;
}

static void start_bash_command(TerminalTab *tab, const char *original,
                               const char *command)
{
    record_command_line(tab, original);
    int pfd[2];
    if (pipe(pfd) == -1) {
        add_format(&tab->buffer, "pipe: %s", strerror(errno));
        tab->last_status = 1;
        return;
    }
    pid_t pid = fork();
    if (pid == -1) {
        close(pfd[0]); close(pfd[1]);
        add_format(&tab->buffer, "fork: %s", strerror(errno));
        tab->last_status = 1;
        return;
    }
    if (pid == 0) {
        (void)setpgid(0, 0);
        close(pfd[0]);
        if (dup2(pfd[1], STDOUT_FILENO) == -1 ||
            dup2(pfd[1], STDERR_FILENO) == -1) _exit(126);
        close(pfd[1]);
        if (chdir(tab->cwd) == -1) {
            dprintf(STDERR_FILENO, "chdir: %s\n", strerror(errno));
            _exit(126);
        }
        execl("/bin/bash", "bash", "--noprofile", "--norc", "-c",
              command, (char *)NULL);
        dprintf(STDERR_FILENO, "terminator: cannot start bash: %s\n",
                strerror(errno));
        _exit(127);
    }
    (void)setpgid(pid, pid);
    close(pfd[1]);
    int flags = fcntl(pfd[0], F_GETFL, 0);
    if (flags == -1 || fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK) == -1) {
        close(pfd[0]); (void)kill(-pid, SIGTERM);
        while (waitpid(pid, NULL, 0) == -1 && errno == EINTR) {}
        add_line(&tab->buffer, "Error configuring command output pipe");
        tab->last_status = 1;
        return;
    }
    tab->command.pid = pid;
    tab->command.output_fd = pfd[0];
    tab->command.active = 1;
    tab->command.pipe_eof = 0;
    tab->command.child_reaped = 0;
    tab->command.stopped = 0;
    tab->command.background = 0;
    tab->command.partial_length = 0;
    tab->command.partial_line[0] = '\0';
    tab->command.exit_status = 0;
    snprintf(tab->command.command_text, sizeof(tab->command.command_text),
             "%s", original);
}

/* Detect a trailing unquoted '&' and remove it from the command text. */
static int extract_background_operator(char *command)
{
    size_t n = strlen(command);
    while (n > 0 && (command[n - 1] == ' ' || command[n - 1] == '\t'))
        command[--n] = '\0';
    if (n == 0 || command[n - 1] != '&')
        return 0;

    /* Do not treat a quoted or escaped ampersand as a job operator. */
    char quote = '\0';
    int escaped = 0;
    for (size_t i = 0; i < n - 1; ++i) {
        char c = command[i];
        if (escaped) { escaped = 0; continue; }
        if (c == '\\' && quote != '\'') { escaped = 1; continue; }
        if ((c == '\'' || c == '"') && (quote == '\0' || quote == c)) {
            quote = quote == '\0' ? c : '\0';
            continue;
        }
    }
    if (quote != '\0' || escaped)
        return 0;

    /* A preceding '&' is not supported as a separate shell operator. */
    command[n - 1] = '\0';
    while (n > 1 && (command[n - 2] == ' ' || command[n - 2] == '\t'))
        command[--n - 1] = '\0';
    return 1;
}

static void promote_background_command(TerminalTab *tab)
{
    if (!tab->command.active) return;
    if (tab->background_command.active) {
        add_line(&tab->buffer, "A background job is already active in this tab.");
        (void)kill(-tab->command.pid, SIGTERM); (void)kill(tab->command.pid, SIGTERM);
        return;
    }
    tab->command.background = 1;
    tab->background_command = tab->command;
    memset(&tab->command, 0, sizeof(tab->command));
    tab->command.pid = -1; tab->command.output_fd = -1;
}

static void start_command(TerminalTab *tab, const char *original_command)
{
    if (tab->command.active && !tab->command.stopped) {
        add_line(&tab->buffer, "A foreground command is already running.");
        tab->last_status = 1; return;
    }

    if (strncmp(original_command, "multiWatch", 10) == 0 &&
        (original_command[10] == ' ' || original_command[10] == '\t' ||
         original_command[10] == '[')) {
        start_multiwatch(tab, original_command);
        return;
    }

    char expanded[INPUT_SIZE];
    expand_status(expanded, sizeof(expanded),
                  original_command, tab->last_status);
    int run_in_background = extract_background_operator(expanded);
    if (expanded[0] == '\0') {
        add_line(&tab->buffer, "Syntax error: '&' requires a command.");
        tab->last_status = 2;
        return;
    }

    if (command_needs_bash(expanded)) {
        start_bash_command(tab, original_command, expanded);
        if (run_in_background && tab->command.active) {
            char message[LINE_SIZE];
            snprintf(message, sizeof(message), "[1] %ld started in background",
                     (long)tab->command.pid);
            add_line(&tab->buffer, message);
            promote_background_command(tab);
        }
        return;
    }

    if (has_pipeline(expanded)) {
        start_pipeline(tab, original_command, expanded);
        if (run_in_background && tab->command.active) {
            char message[LINE_SIZE];
            snprintf(message, sizeof(message), "[1] %ld started in background",
                     (long)tab->command.pid);
            add_line(&tab->buffer, message);
            promote_background_command(tab);
        }
        return;
    }

    char normalized[INPUT_SIZE];
    normalize_redirections(normalized, sizeof(normalized), expanded);
    char copy[INPUT_SIZE];
    snprintf(copy, sizeof(copy), "%s", normalized);

    char *argv[MAX_ARGS];
    int argc = parse_command(copy, argv, MAX_ARGS);

    if (argc < 0) {
        add_line(&tab->buffer, "Syntax error: unmatched quote.");
        tab->last_status = 2;
        return;
    }

    if (argc == 0)
        return;

    char *input_path = NULL;
    char *output_path = NULL;
    int append_output = 0;
    int clean_argc = 0;
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "<") == 0 || strcmp(argv[i], ">") == 0 ||
            strcmp(argv[i], ">>") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                add_line(&tab->buffer, "Syntax error: redirection requires a filename.");
                tab->last_status = 2;
                return;
            }
            if (strcmp(argv[i], "<") == 0) {
                if (input_path) {
                    add_line(&tab->buffer, "Syntax error: duplicate input redirection.");
                    tab->last_status = 2; return;
                }
                input_path = argv[++i];
            } else {
                if (output_path) {
                    add_line(&tab->buffer, "Syntax error: duplicate output redirection.");
                    tab->last_status = 2; return;
                }
                append_output = strcmp(argv[i], ">>") == 0;
                output_path = argv[++i];
            }
        } else {
            argv[clean_argc++] = argv[i];
        }
    }
    argv[clean_argc] = NULL;
    argc = clean_argc;
    if (argc == 0) {
        add_line(&tab->buffer, "Syntax error: redirection without a command.");
        tab->last_status = 2; return;
    }

    record_command_line(tab, original_command);

    /* Built-in: cd */
    if (strcmp(argv[0], "cd") == 0) {
        if (argc > 2) {
            add_line(&tab->buffer, "cd: too many arguments");
            tab->last_status = 1;
        } else {
            const char *path = argc == 1 ? NULL : argv[1];
            (void)change_directory(tab, path);
        }
        return;
    }

    /* Built-in: history */
    if (strcmp(argv[0], "history") == 0) {
        show_history(tab);
        return;
    }

    /* Built-in: exit */
    if (strcmp(argv[0], "exit") == 0) {
        if (tab_count > 1) {
            close_tab(active_tab);
        } else {
            running = 0;
        }
        return;
    }

    /* Job-control commands operate on the current tab's active process group.
       Commands remain asynchronously serviced so the X11 event loop stays live. */
    if (strcmp(argv[0], "jobs") == 0) {
        int found = 0;
        ActiveCommand *list[2] = {&tab->command, &tab->background_command};
        for (int k = 0; k < 2; ++k) if (list[k]->active) {
            ActiveCommand *job = list[k]; char message[LINE_SIZE];
            int job_number = (tab->command.active && tab->background_command.active) ? k + 1 : 1;
            snprintf(message, sizeof(message), "[%d] %s: %.900s", job_number,
                     job->stopped ? "stopped" :
                     (job->background ? "running (background)" : "running"),
                     job->command_text);
            add_line(&tab->buffer, message); found = 1;
        }
        if (!found) add_line(&tab->buffer, "No running or stopped jobs in this tab.");
        tab->last_status = 0; return;
    }
    if (strcmp(argv[0], "fg") == 0 || strcmp(argv[0], "bg") == 0) {
        int to_bg = strcmp(argv[0], "bg") == 0;
        if (argc > 2 || (argc == 2 && strcmp(argv[1], "%1") != 0 &&
                          strcmp(argv[1], "1") != 0)) {
            add_line(&tab->buffer, to_bg ? "bg: usage: bg [%1]" : "fg: usage: fg [%1]");
            tab->last_status = 2; return;
        }
        ActiveCommand *job = tab->background_command.active ?
                             &tab->background_command : &tab->command;
        if (!job->active || job->pid <= 0) {
            add_line(&tab->buffer, to_bg ? "bg: no current job" : "fg: no current job");
            tab->last_status = 1; return;
        }
        if (job->stopped && kill(-job->pid, SIGCONT) == -1 && kill(job->pid, SIGCONT) == -1) {
            add_format(&tab->buffer, to_bg ? "bg: %s" : "fg: %s", strerror(errno));
            tab->last_status = 1; return;
        }
        job->stopped = 0;
        if (to_bg) {
            if (job == &tab->command) {
                if (tab->background_command.active) {
                    add_line(&tab->buffer, "bg: a background job is already active");
                    tab->last_status = 1; return;
                }
                tab->background_command = *job;
                memset(job, 0, sizeof(*job)); job->pid = -1; job->output_fd = -1;
            }
            tab->background_command.background = 1;
            add_line(&tab->buffer, "[1] continued in background.");
        } else {
            if (job == &tab->background_command) {
                if (tab->command.active) {
                    add_line(&tab->buffer, "fg: a foreground command is already running");
                    tab->last_status = 1; return;
                }
                tab->command = *job;
                memset(job, 0, sizeof(*job)); job->pid = -1; job->output_fd = -1;
            }
            tab->command.background = 0;
            add_line(&tab->buffer, "[1] continued in foreground.");
        }
        tab->last_status = 0; return;
    }

    if (strcmp(argv[0], "source") == 0 || strcmp(argv[0], ".") == 0) {
        if (argc != 2) {
            add_line(&tab->buffer, "source: usage: source filename");
            tab->last_status = 2;
            return;
        }
        char script_path[PATH_MAX];
        const char *requested_path = argv[1];
        if (requested_path[0] == '/') {
            if (snprintf(script_path, sizeof(script_path), "%s",
                         requested_path) >= (int)sizeof(script_path)) {
                add_line(&tab->buffer, "source: script path is too long");
                tab->last_status = 1;
                return;
            }
        } else if (requested_path[0] == '~' &&
                   (requested_path[1] == '/' || requested_path[1] == '\0')) {
            const char *home = getenv("HOME");
            if (home == NULL) home = "/";
            if (snprintf(script_path, sizeof(script_path), "%s%s",
                         home, requested_path + 1) >= (int)sizeof(script_path)) {
                add_line(&tab->buffer, "source: script path is too long");
                tab->last_status = 1;
                return;
            }
        } else if (snprintf(script_path, sizeof(script_path), "%s/%s",
                            tab->cwd, requested_path) >= (int)sizeof(script_path)) {
            add_line(&tab->buffer, "source: script path is too long");
            tab->last_status = 1;
            return;
        }
        FILE *script = fopen(script_path, "r");
        if (script == NULL) {
            char message[LINE_SIZE];
            snprintf(message, sizeof(message), "source: %s: %s",
                     requested_path, strerror(errno));
            add_line(&tab->buffer, message);
            tab->last_status = 1;
            return;
        }
        char script_line[INPUT_SIZE];
        unsigned long line_number = 0;
        int script_failed = 0;
        while (fgets(script_line, sizeof(script_line), script) != NULL) {
            ++line_number;
            size_t n = strlen(script_line);
            if (n > 0 && script_line[n - 1] == '\n')
                script_line[--n] = '\0';
            else if (!feof(script)) {
                char message[LINE_SIZE];
                snprintf(message, sizeof(message),
                         "source: %s:%lu: line too long", argv[1], line_number);
                add_line(&tab->buffer, message);
                script_failed = 1;
                int ch;
                while ((ch = fgetc(script)) != '\n' && ch != EOF) {}
                continue;
            }
            char *line = script_line;
            while (*line == ' ' || *line == '\t' || *line == '\r') ++line;
            if (*line == '\0' || *line == '#') continue;
            /* Strip trailing CR for Windows-created script files. */
            n = strlen(line);
            while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t' ||
                             line[n - 1] == '\r')) line[--n] = '\0';
            if (*line == '\0') continue;
            if (strcmp(line, "exit") == 0) break;
            start_command(tab, line);
            /* The existing command engine is asynchronous; wait for each script
               command to finish before interpreting the next line. */
            while (tab->command.active && !tab->command.stopped) {
                service_tab(tab);
                if (tab->command.active) {
                    struct timespec pause_time = {0, 10000000L};
                    (void)nanosleep(&pause_time, NULL);
                }
            }
            if (tab->command.stopped) {
                add_line(&tab->buffer, "source: command stopped; script paused.");
                script_failed = 1;
                break;
            }
            if (tab->last_status != 0)
                script_failed = 1;
        }
        if (ferror(script)) {
            char message[LINE_SIZE];
            snprintf(message, sizeof(message), "source: error reading %s", argv[1]);
            add_line(&tab->buffer, message);
            script_failed = 1;
        }
        fclose(script);
        tab->last_status = script_failed ? 1 : 0;
        return;
    }

    /* Start an external program. */
    int pipefd[2];

    if (pipe(pipefd) == -1) {
        add_format(&tab->buffer, "pipe: %s", strerror(errno));
        tab->last_status = 1;
        return;
    }

    pid_t pid = fork();

    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        add_format(&tab->buffer, "fork: %s", strerror(errno));
        tab->last_status = 1;
        return;
    }

    if (pid == 0) {
        (void)setpgid(0, 0);
        close(pipefd[0]);

        if (dup2(pipefd[1], STDERR_FILENO) == -1)
            _exit(126);

        if (output_path == NULL && dup2(pipefd[1], STDOUT_FILENO) == -1)
            _exit(126);
        close(pipefd[1]);

        if (chdir(tab->cwd) == -1) {
            dprintf(STDERR_FILENO, "chdir: %s\n", strerror(errno));
            _exit(126);
        }

        if (input_path) {
            int fd = open(input_path, O_RDONLY);
            if (fd == -1) { dprintf(STDERR_FILENO, "%s: %s\n", input_path, strerror(errno)); _exit(1); }
            if (dup2(fd, STDIN_FILENO) == -1) { dprintf(STDERR_FILENO, "dup2: %s\n", strerror(errno)); close(fd); _exit(1); }
            close(fd);
        }
        if (output_path) {
            int flags = O_WRONLY | O_CREAT | (append_output ? O_APPEND : O_TRUNC);
            int fd = open(output_path, flags, 0666);
            if (fd == -1) { dprintf(STDERR_FILENO, "%s: %s\n", output_path, strerror(errno)); _exit(1); }
            if (dup2(fd, STDOUT_FILENO) == -1) { dprintf(STDERR_FILENO, "dup2: %s\n", strerror(errno)); close(fd); _exit(1); }
            close(fd);
        }

        execvp(argv[0], argv);

        int exec_error = errno;
        if (exec_error == ENOENT)
            dprintf(STDERR_FILENO, "terminator: %s: command not found\n", argv[0]);
        else
            dprintf(STDERR_FILENO, "terminator: %s: %s\n",
                    argv[0], strerror(exec_error));
        _exit(exec_error == ENOENT ? 127 : 126);
    }

    (void)setpgid(pid, pid);
    close(pipefd[1]);

    int flags = fcntl(pipefd[0], F_GETFL, 0);

    if (flags == -1 ||
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) == -1) {
        close(pipefd[0]);
        (void)kill(-pid, SIGTERM);

        while (waitpid(pid, NULL, 0) == -1 && errno == EINTR)
            ;

        add_line(&tab->buffer, "Error configuring command output pipe");
        tab->last_status = 1;
        return;
    }

    tab->command.pid = pid;
    tab->command.output_fd = pipefd[0];
    tab->command.active = 1;
    tab->command.pipe_eof = 0;
    tab->command.child_reaped = 0;
    tab->command.stopped = 0;
    tab->command.background = 0;
    tab->command.partial_length = 0;
    tab->command.partial_line[0] = '\0';
    tab->command.exit_status = 0;

    snprintf(tab->command.command_text,
             sizeof(tab->command.command_text), "%s", original_command);
    if (run_in_background) {
        char message[LINE_SIZE];
        snprintf(message, sizeof(message), "[1] %ld started in background",
                 (long)pid);
        add_line(&tab->buffer, message);
        promote_background_command(tab);
    }
}

/* Read output without blocking the X11 event loop. */
static void service_one_command(TerminalTab *tab, ActiveCommand *cmd, int background)
{
    if (!cmd->active) return;
    if (!cmd->pipe_eof && cmd->output_fd >= 0) {
        char data[4096];
        for (;;) {
            ssize_t n = read(cmd->output_fd, data, sizeof(data));
            if (n > 0) consume_output(tab, cmd, data, (size_t)n);
            else if (n == 0) { cmd->pipe_eof=1; close(cmd->output_fd); cmd->output_fd=-1; break; }
            else if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            else { cmd->pipe_eof=1; close(cmd->output_fd); cmd->output_fd=-1; break; }
        }
    }
    if (!cmd->child_reaped) {
        int status=0;
        pid_t r=waitpid(cmd->pid,&status,WNOHANG|WUNTRACED|WCONTINUED);
        if (r==cmd->pid) {
            if (WIFSTOPPED(status)) { cmd->stopped=1; cmd->background=0; add_line(&tab->buffer,"[1] job stopped; use fg or bg."); }
            else if (WIFCONTINUED(status)) cmd->stopped=0;
            else { cmd->child_reaped=1; cmd->exit_status=WIFEXITED(status)?WEXITSTATUS(status):(WIFSIGNALED(status)?128+WTERMSIG(status):1); }
        } else if (r==-1 && errno!=EINTR) { cmd->child_reaped=1; cmd->exit_status=1; }
    }
    if (cmd->pipe_eof && cmd->child_reaped) {
        flush_partial_output(tab,cmd);
        if (!background) tab->last_status=cmd->exit_status;
        char msg[LINE_SIZE];
        snprintf(msg,sizeof(msg),"[%s completed; exit status %d]",background?"background job":"command",cmd->exit_status);
        add_line(&tab->buffer,msg); cmd->active=0; cmd->pid=-1;
        if (cmd->output_fd>=0) { close(cmd->output_fd); cmd->output_fd=-1; }
    }
}
static void service_tab(TerminalTab *tab)
{
    service_one_command(tab,&tab->command,0);
    service_one_command(tab,&tab->background_command,1);
}

/* Scroll the terminal output. Positive delta moves toward older output. */
static void scroll_buffer(TerminalTab *tab, int delta)
{
    int visible_rows =
        (window_height - HEADER_HEIGHT - 40) / LINE_HEIGHT;
    if (visible_rows < 1)
        visible_rows = 1;

    int max_start = tab->buffer.count > visible_rows
                    ? tab->buffer.count - visible_rows : 0;
    int current_start = tab->scroll_offset == 0
                        ? max_start : tab->scroll_offset - 1;

    current_start -= delta;
    if (current_start < 0)
        current_start = 0;
    if (current_start > max_start)
        current_start = max_start;

    /* Zero means follow the newest output. */
    tab->scroll_offset = (current_start == max_start)
                         ? 0 : current_start + 1;
}

/* ---------- Drawing ---------- */

static void draw_window(void)
{
    XClearWindow(display, window);

    int x = 10;

    for (int i = 0; i < tab_count; i++) {
        const char *base = strrchr(tabs[i].cwd, '/');

        if (base != NULL && base[1] != '\0')
            base++;
        else
            base = "/";

        char label[160];
        snprintf(label, sizeof(label), "[%c %d: %.60s%s]",
                 i == active_tab ? '*' : ' ',
                 i + 1, base,
                 (tabs[i].command.active || tabs[i].background_command.active || tabs[i].watch.active) ?
                     (tabs[i].command.stopped ? " (stopped)" : " (running)") : "");

        XDrawString(display, window, gc, x, 19,
                    label, (int)strlen(label));
        x += (int)strlen(label) * 8 + 10;
    }

    XDrawString(display, window, gc, x, 19,
                "Ctrl+T new | Ctrl+W close | Ctrl+1..9 switch",
                44);

    TerminalTab *tab = &tabs[active_tab];

    int visible_rows =
        (window_height - HEADER_HEIGHT - 40) / LINE_HEIGHT;

    if (visible_rows < 1)
        visible_rows = 1;

    int max_start = tab->buffer.count > visible_rows
                    ? tab->buffer.count - visible_rows : 0;

    int start;

    if (tab->scroll_offset == 0) {
        start = max_start;
    } else {
        start = tab->scroll_offset - 1;

        if (start < 0)
            start = 0;

        if (start > max_start)
            start = max_start;
    }

    int y = HEADER_HEIGHT + LINE_HEIGHT;

    for (int i = start;
         i < tab->buffer.count && y < window_height - LINE_HEIGHT;
         i++, y += LINE_HEIGHT) {
        XDrawString(display, window, gc, LEFT_MARGIN, y,
                    tab->buffer.lines[i],
                    (int)strlen(tab->buffer.lines[i]));
    }

    if ((!tab->command.active || tab->command.stopped || tab->command.background) && !tab->watch.active) {
        if (y > window_height - 5)
            y = window_height - 5;

        const char *visible_prompt = tab->continuation_mode ? "> " : PROMPT;
        XDrawString(display, window, gc, LEFT_MARGIN, y,
                    visible_prompt, (int)strlen(visible_prompt));

        int prompt_width =
            XTextWidth(font, visible_prompt, (int)strlen(visible_prompt));

        int input_x = LEFT_MARGIN + prompt_width;
        int available_width = window_width - input_x - LEFT_MARGIN;

        if (available_width > 0) {
            int input_len = (int)tab->input_length;
            int first = 0;

            while (first < input_len &&
                   XTextWidth(font, tab->input + first,
                              input_len - first) > available_width)
                first++;

            XDrawString(display, window, gc, input_x, y,
                        tab->input + first, input_len - first);
            int cursor_chars = (int)tab->input_cursor - first;
            if (cursor_chars < 0) cursor_chars = 0;
            if (cursor_chars > input_len - first) cursor_chars = input_len - first;
            int cursor_x = input_x + XTextWidth(font, tab->input + first, cursor_chars);
            XDrawLine(display, window, gc, cursor_x, y - 14, cursor_x, y + 3);
        }
    }

    draw_text_selection(tab, start, HEADER_HEIGHT + LINE_HEIGHT);
    XFlush(display);
}

/* ---------- Keyboard controls ---------- */

static void handle_key(XKeyEvent *event)
{
    char text[64] = {0};
    KeySym key = NoSymbol;

    int length = XLookupString(event, text, (int)sizeof(text),
                               &key, NULL);

    int ctrl = (event->state & ControlMask) != 0;
    int shift = (event->state & ShiftMask) != 0;

    if (ctrl && (key == XK_t || key == XK_T)) {
        if (tab_count < MAX_TABS) {
            init_tab(&tabs[tab_count]);
            active_tab = tab_count;
            tab_count++;
        } else {
            add_line(&tabs[active_tab].buffer,
                     "Maximum number of tabs reached.");
        }
        return;
    }

    if (ctrl && shift && (key == XK_w || key == XK_W)) {
        close_tab(active_tab);
        return;
    }

    if (ctrl && key >= XK_1 && key <= XK_9) {
        int index = (int)(key - XK_1);

        if (index < tab_count)
            active_tab = index;

        return;
    }

    if (key >= XK_F1 && key <= XK_F9) {
        int index = (int)(key - XK_F1);

        if (index < tab_count)
            active_tab = index;

        return;
    }

    TerminalTab *tab = &tabs[active_tab];

    if (key == XK_Page_Up) {
        int visible_rows =
            (window_height - HEADER_HEIGHT - 40) / LINE_HEIGHT;
        if (visible_rows < 1) visible_rows = 1;
        scroll_buffer(tab, visible_rows);
        return;
    }

    if (key == XK_Page_Down) {
        int visible_rows =
            (window_height - HEADER_HEIGHT - 40) / LINE_HEIGHT;
        if (visible_rows < 1) visible_rows = 1;
        scroll_buffer(tab, -visible_rows);
        return;
    }

    if (key == XK_Up && !(tab->command.active && !tab->command.stopped && !tab->command.background) && !tab->watch.active) {
        if (command_history.count > 0) {
            if (tab->history_position == 0) {
                memmove(tab->saved_input, tab->input, tab->input_length + 1);
                tab->history_position = command_history.count + 1;
            }
            if (tab->history_position > 1) tab->history_position--;
            size_t idx = (command_history.next + MAX_HISTORY -
                          (command_history.count - (tab->history_position - 1))) % MAX_HISTORY;
            if (tab->history_position <= command_history.count) {
                snprintf(tab->input, sizeof(tab->input), "%s", command_history.commands[idx]);
                tab->input_length = strlen(tab->input); tab->input_cursor = tab->input_length;
            }
        }
        return;
    }
    if (key == XK_Down && tab->history_position > 0) {
        if (tab->history_position < command_history.count) {
            tab->history_position++;
            size_t idx = (command_history.next + MAX_HISTORY -
                          (command_history.count - (tab->history_position - 1))) % MAX_HISTORY;
            snprintf(tab->input, sizeof(tab->input), "%s", command_history.commands[idx]);
            tab->input_length = strlen(tab->input); tab->input_cursor = tab->input_length;
        } else {
            tab->history_position = 0;
            memmove(tab->input, tab->saved_input, strlen(tab->saved_input) + 1);
            tab->input_length = strlen(tab->input); tab->input_cursor = tab->input_length;
        }
        return;
    }
    if (key == XK_Left) { if (tab->input_cursor > 0) tab->input_cursor--; return; }
    if (key == XK_Right) { if (tab->input_cursor < tab->input_length) tab->input_cursor++; return; }

    if (binding_pressed(event, key, binding_ctrl_a)) { tab->input_cursor=0; return; }
    if (binding_pressed(event, key, binding_ctrl_e)) { tab->input_cursor=tab->input_length; return; }
    if (binding_pressed(event, key, binding_ctrl_u)) {
        memmove(tab->input, tab->input + tab->input_cursor,
                tab->input_length - tab->input_cursor + 1);
        tab->input_length -= tab->input_cursor; tab->input_cursor=0; return;
    }
    if (binding_pressed(event, key, binding_ctrl_k)) {
        tab->input[tab->input_cursor]='\0'; tab->input_length=tab->input_cursor; return;
    }
    if (binding_pressed(event, key, binding_ctrl_w)) {
        size_t start=tab->input_cursor;
        while (start>0 && tab->input[start-1]==' ') start--;
        while (start>0 && tab->input[start-1]!=' ') start--;
        memmove(tab->input+start, tab->input+tab->input_cursor,
                tab->input_length-tab->input_cursor+1);
        tab->input_length -= tab->input_cursor-start; tab->input_cursor=start; return;
    }
    if (binding_pressed(event, key, binding_ctrl_l)) { tab->buffer.count=0; tab->scroll_offset=0; return; }
    if (binding_pressed(event, key, binding_ctrl_d)) {
        if (tab->input_length == 0) close_tab(active_tab);
        else if (tab->input_cursor < tab->input_length) {
            memmove(tab->input+tab->input_cursor, tab->input+tab->input_cursor+1,
                    tab->input_length-tab->input_cursor);
            tab->input_length--;
        }
        return;
    }
    if (binding_pressed(event, key, binding_copy)) {
        copy_text_selection();
        return;
    }
    if (binding_pressed(event, key, binding_paste)) { paste_clipboard(); return; }

    if (key == XK_Up) {
        int visible =
            (window_height - HEADER_HEIGHT - 40) / LINE_HEIGHT;

        int max_start = tab->buffer.count > visible
                        ? tab->buffer.count - visible : 0;

        if (max_start > 0) {
            if (tab->scroll_offset == 0)
                tab->scroll_offset = max_start;
            else if (tab->scroll_offset > 1)
                tab->scroll_offset--;
        }
        return;
    }

    if (key == XK_Down && tab->history_position == 0) {
        if (tab->scroll_offset > 0) {
            tab->scroll_offset++;

            int visible =
                (window_height - HEADER_HEIGHT - 40) / LINE_HEIGHT;

            int max_start = tab->buffer.count > visible
                            ? tab->buffer.count - visible : 0;

            if (tab->scroll_offset > max_start)
                tab->scroll_offset = 0;
        }
        return;
    }

    if (ctrl && (key == XK_l || key == XK_L)) {
        tab->buffer.count = 0;
        tab->scroll_offset = 0;
        return;
    }

    if (key == XK_Escape) {
        running = 0;
        return;
    }

    if (ctrl && (key == XK_c || key == XK_C)) {
        if (tab->watch.active) {
            stop_multiwatch(tab);
        } else if (tab->command.active && tab->command.pid > 0) {
            (void)kill(-tab->command.pid, SIGINT);
            (void)kill(tab->command.pid, SIGINT);
        }
        return;
    }

    if (ctrl && (key == XK_z || key == XK_Z)) {
        if (tab->command.active && tab->command.pid > 0 &&
            !tab->command.stopped) {
            if (kill(-tab->command.pid, SIGTSTP) == -1)
                (void)kill(tab->command.pid, SIGTSTP);
            add_line(&tab->buffer, "Sent Ctrl+Z (SIGTSTP) to current job.");
        }
        return;
    }

    if ((tab->command.active && !tab->command.stopped && !tab->command.background) || tab->watch.active)
        return;

    if (key == XK_BackSpace) {
        if (tab->input_cursor > 0) {
            memmove(tab->input + tab->input_cursor - 1, tab->input + tab->input_cursor,
                    tab->input_length - tab->input_cursor + 1);
            tab->input_cursor--; tab->input_length--;
        }
        return;
    }
    if (key == XK_Delete) {
        if (tab->input_cursor < tab->input_length) {
            memmove(tab->input + tab->input_cursor, tab->input + tab->input_cursor + 1,
                    tab->input_length - tab->input_cursor);
            tab->input_length--;
        }
        return;
    }

    if (key == XK_Return || key == XK_KP_Enter) {
        tab->input[tab->input_length] = '\0';

        char visible_line[LINE_SIZE];
        const char *line_prompt = tab->continuation_mode ? "> " : PROMPT;
        snprintf(visible_line, sizeof(visible_line), "%s%.1000s",
                 line_prompt, tab->input);
        add_line(&tab->buffer, visible_line);
        tab->scroll_offset = 0;

        size_t line_length = tab->input_length;
        size_t trailing_slashes = 0;
        for (size_t i = line_length; i > 0 &&
             tab->input[i - 1] == '\\'; --i)
            ++trailing_slashes;
        int backslash_continuation = (trailing_slashes % 2) == 1;
        if (backslash_continuation && line_length > 0)
            --line_length;

        size_t separator = (tab->pending_length > 0 &&
                            !tab->previous_backslash_continuation) ? 1 : 0;
        if (tab->pending_length + separator + line_length >=
            sizeof(tab->pending_command)) {
            add_line(&tab->buffer, "terminator: multiline command is too long");
            tab->pending_length = 0;
            tab->pending_command[0] = '\0';
            tab->continuation_mode = 0;
            tab->previous_backslash_continuation = 0;
            tab->input_length = 0;
            tab->input[0] = '\0';
            return;
        }

        if (separator)
            tab->pending_command[tab->pending_length++] = '\n';
        if (line_length > 0) {
            memcpy(tab->pending_command + tab->pending_length,
                   tab->input, line_length);
            tab->pending_length += line_length;
        }
        tab->pending_command[tab->pending_length] = '\0';

        tab->input_length = 0;
        tab->input_cursor = 0;
        tab->history_position = 0;
        tab->input[0] = '\0';

        if (backslash_continuation ||
            command_is_incomplete(tab->pending_command)) {
            tab->continuation_mode = 1;
            tab->previous_backslash_continuation = backslash_continuation;
            return;
        }

        char command[INPUT_SIZE];
        snprintf(command, sizeof(command), "%s", tab->pending_command);
        tab->pending_length = 0;
        tab->pending_command[0] = '\0';
        tab->continuation_mode = 0;
        tab->previous_backslash_continuation = 0;
        tab->suppress_next_echo = 1;
        start_command(tab, command);
        return;
    }

    if (!ctrl && length > 0 && tab->input_length + (size_t)length < sizeof(tab->input)) {
        for (int i = 0; i < length; i++) {
            unsigned char ch = (unsigned char)text[i];
            if (ch >= 32 && ch <= 126 && tab->input_length + 1 < sizeof(tab->input)) {
                memmove(tab->input + tab->input_cursor + 1,
                        tab->input + tab->input_cursor,
                        tab->input_length - tab->input_cursor + 1);
                tab->input[tab->input_cursor++] = (char)ch;
                tab->input_length++;
            }
        }
        tab->history_position = 0;
    }
}

/* ---------- Main event loop ---------- */

int main(void)
{
    display = XOpenDisplay(NULL);

    if (display == NULL) {
        fprintf(stderr,
                "Cannot open X display. Check your DISPLAY setting.\n");
        return EXIT_FAILURE;
    }

    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);

    window = XCreateSimpleWindow(
        display, root, 100, 100,
        WINDOW_WIDTH, WINDOW_HEIGHT, 1,
        BlackPixel(display, screen),
        WhitePixel(display, screen));

    XStoreName(display, window, "Terminator - Custom Terminal");

    wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    clipboard_atom = XInternAtom(display, "CLIPBOARD", False);
    utf8_atom = XInternAtom(display, "UTF8_STRING", False);
    targets_atom = XInternAtom(display, "TARGETS", False);
    string_atom = XInternAtom(display, "STRING", False);
    load_key_bindings();
    XSetWMProtocols(display, window, &wm_delete, 1);

    XSelectInput(display, window,
                 ExposureMask | KeyPressMask | ButtonPressMask |
                 ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);

    gc = XCreateGC(display, window, 0, NULL);

    if (gc == NULL) {
        fprintf(stderr, "Could not create graphics context.\n");
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return EXIT_FAILURE;
    }

    font = XLoadQueryFont(display, "fixed");

    if (font == NULL) {
        fprintf(stderr, "Could not load X11 font.\n");
        XFreeGC(display, gc);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return EXIT_FAILURE;
    }

    XSetFont(display, gc, font->fid);

    memset(&command_history, 0, sizeof(command_history));
    init_tab(&tabs[0]);

    XMapWindow(display, window);
    XFlush(display);

    int x_fd = ConnectionNumber(display);

    while (running) {
        struct pollfd fds[1 + 2 * MAX_TABS];
        int owners[1 + 2 * MAX_TABS];
        nfds_t nfds = 1;

        fds[0].fd = x_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        owners[0] = -1;

        for (int i = 0; i < tab_count; i++) {
            ActiveCommand *tracked[2] = {&tabs[i].command, &tabs[i].background_command};
            for (int k=0;k<2;k++) if (tracked[k]->active && tracked[k]->output_fd>=0) {
                fds[nfds].fd=tracked[k]->output_fd;
                fds[nfds].events=POLLIN|POLLHUP|POLLERR; fds[nfds].revents=0;
                owners[nfds]=i; nfds++;
            }
        }

        int result = poll(fds, nfds, 100);

        if (result == -1 && errno != EINTR) {
            perror("poll");
            break;
        }

        for (nfds_t i = 1; i < nfds; i++) {
            if (fds[i].revents == 0)
                continue;

            int owner = owners[i];

            if (owner < 0 || owner >= tab_count)
                continue;

            TerminalTab *tab = &tabs[owner];

            if ((tab->command.active && tab->command.output_fd==fds[i].fd) ||
                (tab->background_command.active && tab->background_command.output_fd==fds[i].fd))
                service_tab(tab);
        }

        for (int i = 0; i < tab_count; i++) {
            service_tab(&tabs[i]);
            service_multiwatch(&tabs[i]);
        }

        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);

            if (event.type == Expose) {
                if (event.xexpose.count == 0)
                    draw_window();
            } else if (event.type == ConfigureNotify) {
                window_width = event.xconfigure.width;
                window_height = event.xconfigure.height;
                draw_window();
            } else if (event.type == KeyPress) {
                handle_key(&event.xkey);
                draw_window();
            } else if (event.type == ButtonPress) {
                if (event.xbutton.button == Button4) {
                    scroll_buffer(&tabs[active_tab], 3);
                } else if (event.xbutton.button == Button5) {
                    scroll_buffer(&tabs[active_tab], -3);
                } else if (event.xbutton.button == Button1) {
                    int line, col;
                    if (selection_position_at(event.xbutton.x, event.xbutton.y,
                                              &line, &col)) {
                        text_selection.active = 1;
                        text_selection.dragging = 1;
                        text_selection.start_line = line;
                        text_selection.start_col = col;
                        text_selection.end_line = line;
                        text_selection.end_col = col;
                    } else {
                        text_selection.active = 0;
                        text_selection.dragging = 0;
                    }
                }
                draw_window();
            } else if (event.type == MotionNotify) {
                if (text_selection.dragging) {
                    int line, col;
                    if (selection_position_at(event.xmotion.x, event.xmotion.y,
                                              &line, &col)) {
                        text_selection.end_line = line;
                        text_selection.end_col = col;
                    }
                    draw_window();
                }
            } else if (event.type == ButtonRelease) {
                if (event.xbutton.button == Button1)
                    text_selection.dragging = 0;
                draw_window();
            } else if (event.type == SelectionRequest) {
                XSelectionRequestEvent *req = &event.xselectionrequest;
                XEvent reply; memset(&reply, 0, sizeof(reply));
                reply.xselection.type = SelectionNotify;
                reply.xselection.display = req->display; reply.xselection.requestor = req->requestor;
                reply.xselection.selection = req->selection; reply.xselection.target = req->target;
                reply.xselection.time = req->time; reply.xselection.property = None;
                int owns_requested_selection =
                    (req->selection == clipboard_atom || req->selection == XA_PRIMARY) &&
                    XGetSelectionOwner(display, req->selection) == window;
                if (owns_requested_selection &&
                    (req->target == utf8_atom || req->target == string_atom ||
                     req->target == XA_STRING)) {
                    Atom property = req->property == None ? req->target : req->property;
                    Atom type = (req->target == utf8_atom) ? utf8_atom : XA_STRING;
                    XChangeProperty(display, req->requestor, property, type, 8,
                                    PropModeReplace, (unsigned char *)clipboard_text,
                                    (int)strlen(clipboard_text));
                    reply.xselection.property = property;
                } else if (owns_requested_selection && req->target == targets_atom) {
                    Atom supported_targets[3] = {targets_atom, utf8_atom, string_atom};
                    Atom property = req->property == None ? req->target : req->property;
                    XChangeProperty(display, req->requestor, property, XA_ATOM, 32,
                                    PropModeReplace, (unsigned char *)supported_targets, 3);
                    reply.xselection.property = property;
                }
                XSendEvent(display, req->requestor, False, 0, &reply);
                XFlush(display);
            } else if (event.type == SelectionNotify) {
                XSelectionEvent *sel = &event.xselection;
                if (sel->property != None) {
                    Atom actual; int format; unsigned long nitems, after; unsigned char *data = NULL;
                    if (XGetWindowProperty(display, window, sel->property, 0, INPUT_SIZE,
                            True, AnyPropertyType, &actual, &format, &nitems, &after, &data) == Success && data) {
                        if (format != 8 || (actual != utf8_atom && actual != XA_STRING)) {
                            XFree(data);
                            continue;
                        }
                        TerminalTab *tab = &tabs[active_tab];
                        if (!(tab->command.active && !tab->command.stopped && !tab->command.background) && !tab->watch.active) {
                            size_t add = nitems;
                            if (add > sizeof(tab->input)-1-tab->input_length) add=sizeof(tab->input)-1-tab->input_length;
                            memmove(tab->input+tab->input_cursor+add, tab->input+tab->input_cursor,
                                    tab->input_length-tab->input_cursor+1);
                            memcpy(tab->input+tab->input_cursor, data, add);
                            tab->input_length += add; tab->input_cursor += add;
                        }
                        XFree(data); draw_window();
                    }
                }
            } else if (event.type == ClientMessage) {
                if ((Atom)event.xclient.data.l[0] == wm_delete)
                    running = 0;
            }
        }

        draw_window();
    }

    for (int i = 0; i < tab_count; i++)
        stop_tab_command(&tabs[i]);

    XFreeFont(display, font);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return EXIT_SUCCESS;
}
