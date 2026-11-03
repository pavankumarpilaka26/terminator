#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
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
#include <pthread.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/file.h>

#define MAX_TABS 9
#define MAX_LINES 4096
#define LINE_SIZE 1024
#define INPUT_SIZE 1024
#define MAX_ARGS 128
#define MAX_HISTORY 10000
#define MAX_WATCH_COMMANDS 16
#define MAX_BACKGROUND_JOBS 16

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
    int reverse_search;
    char search_query[INPUT_SIZE];
    char search_original_input[INPUT_SIZE];
    char search_match[INPUT_SIZE];
    int search_has_match;
    /* Accumulates multiline commands while quotes/substitutions continue. */
    char pending_command[INPUT_SIZE];
    size_t pending_length;
    int continuation_mode;
    int suppress_next_echo;
    int previous_backslash_continuation;
    char cwd[PATH_MAX];
    ActiveCommand command;
    ActiveCommand background_command; /* Process-mode UI summary only. */
    ActiveCommand background_jobs[MAX_BACKGROUND_JOBS];
    MultiWatch watch;
    int scroll_offset;
    int last_status;

    /* Pending filename-completion choices (1-9 select a displayed match). */
    int completion_active;
    size_t completion_start;
    size_t completion_dir_prefix_len;
    size_t completion_count;
    char completion_directory[PATH_MAX];
    char completion_matches[256][NAME_MAX + 1];

    /* One command/job-service worker belongs to each tab. */
    pthread_t worker;
    int worker_started;
    int worker_stop;
    int command_pending;
    char worker_command[INPUT_SIZE];

    /* Process-mode IPC state (used only by the GUI parent). */
    pid_t process_pid;
    int process_fd;
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
static unsigned long prompt_color;
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

/* Task 7: default key bindings, optionally overridden by ~/.terminatorrc. */
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
#define ACTIVE_HISTORY ((process_mode && shared_history) ? shared_history->history : command_history)
typedef struct {
    sem_t mutex;
    CommandHistory history;
} SharedHistory;
static SharedHistory *shared_history;
static int process_mode;
static int process_child_mode;
static int process_child_fd = -1;
static int process_child_exit;
static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int type;
    int status;
    int flags;
    pid_t command_pid;
    pid_t background_pid;
    char cwd[PATH_MAX];
    char text[LINE_SIZE];
} ProcessMessage;

enum { PROC_COMMAND = 1, PROC_INTERRUPT, PROC_STOP, PROC_SHUTDOWN,
       PROC_LINE, PROC_STATE, PROC_EXIT };
/* Serializes shared tab state; Xlib is still called only by the main thread. */
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_condition = PTHREAD_COND_INITIALIZER;
static int pending_close_index = -1;
static int worker_indices[MAX_TABS] = {0,1,2,3,4,5,6,7,8};

static void stop_multiwatch(TerminalTab *tab);
static void service_tab(TerminalTab *tab);
static void service_multiwatch(TerminalTab *tab);
static void start_command(TerminalTab *tab, const char *original_command);
static void close_tab(int index);

/* A GUI launched from a background shell may inherit ignored job-control
   signals.  Executed commands must receive normal interrupt/stop signals. */
static void reset_child_signals(void)
{
    (void)signal(SIGINT, SIG_DFL);
    (void)signal(SIGQUIT, SIG_DFL);
    (void)signal(SIGTSTP, SIG_DFL);
    (void)signal(SIGPIPE, SIG_DFL);
}

/* ---------- Task 7: configurable hotkey bindings ---------- */
static int binding_equal(KeyBinding a, KeyBinding b)
{
    return a.key == b.key && a.modifiers == b.modifiers;
}

/* Accept forms such as Ctrl+A, Ctrl+Shift+C, Alt+X, or F5. */
static int parse_binding(const char *value, KeyBinding *out)
{
    char copy[80];
    if (!value || !out || strlen(value) >= sizeof(copy)) return 0;
    strcpy(copy, value);
    char *save = NULL;
    char *token = strtok_r(copy, "+", &save);
    unsigned int modifiers = 0;
    KeySym key = NoSymbol;
    while (token) {
        if (strcasecmp(token, "Ctrl") == 0 || strcasecmp(token, "Control") == 0)
            modifiers |= ControlMask;
        else if (strcasecmp(token, "Shift") == 0)
            modifiers |= ShiftMask;
        else if (strcasecmp(token, "Alt") == 0)
            modifiers |= Mod1Mask;
        else {
            KeySym candidate = XStringToKeysym(token);
            if (candidate == NoSymbol && strlen(token) == 1)
                candidate = (KeySym)(unsigned char)token[0];
            if (candidate == NoSymbol || key != NoSymbol) return 0;
            if (candidate >= XK_A && candidate <= XK_Z)
                candidate = candidate - XK_A + XK_a;
            key = candidate;
        }
        token = strtok_r(NULL, "+", &save);
    }
    if (key == NoSymbol) return 0;
    out->key = key;
    out->modifiers = modifiers;
    return 1;
}

static void load_key_bindings(void)
{
    /* Always begin with the PDF defaults. */
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
    int path_len = snprintf(path, sizeof(path), "%s/.terminatorrc", home);
    if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
        fprintf(stderr, "terminator: config path is too long; using default hotkeys\n");
        return;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) return; /* A missing config file is normal. */

    char line[160];
    unsigned int line_number = 0;
    while (fgets(line, sizeof(line), fp)) {
        line_number++;
        if (!strchr(line, '\n') && !feof(fp)) {
            int ch;
            while ((ch = fgetc(fp)) != '\n' && ch != EOF) {}
            fprintf(stderr, "terminator: config line %u is too long; ignored\n", line_number);
            continue;
        }
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';
        char *start = line;
        while (*start == ' ' || *start == '\t') start++;
        char *end = start + strlen(start);
        while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
            *--end = '\0';
        if (*start == '\0') continue;
        char *equals = strchr(start, '=');
        if (!equals) {
            fprintf(stderr, "terminator: invalid config line %u; ignored\n", line_number);
            continue;
        }
        *equals++ = '\0';
        char *name_end = start + strlen(start);
        while (name_end > start && (name_end[-1] == ' ' || name_end[-1] == '\t')) *--name_end = '\0';
        while (*equals == ' ' || *equals == '\t') equals++;
        char *value_end = equals + strlen(equals);
        while (value_end > equals && (value_end[-1] == ' ' || value_end[-1] == '\t')) *--value_end = '\0';

        KeyBinding *target = NULL;
        if (strcmp(start, "ctrl_a") == 0) target = &binding_ctrl_a;
        else if (strcmp(start, "ctrl_e") == 0) target = &binding_ctrl_e;
        else if (strcmp(start, "ctrl_u") == 0) target = &binding_ctrl_u;
        else if (strcmp(start, "ctrl_k") == 0) target = &binding_ctrl_k;
        else if (strcmp(start, "ctrl_w") == 0) target = &binding_ctrl_w;
        else if (strcmp(start, "ctrl_l") == 0) target = &binding_ctrl_l;
        else if (strcmp(start, "ctrl_d") == 0) target = &binding_ctrl_d;
        else if (strcmp(start, "copy") == 0) target = &binding_copy;
        else if (strcmp(start, "paste") == 0) target = &binding_paste;
        else {
            fprintf(stderr, "terminator: unknown hotkey '%s' on config line %u; ignored\n", start, line_number);
            continue;
        }

        KeyBinding parsed;
        if (!parse_binding(equals, &parsed)) {
            fprintf(stderr, "terminator: invalid binding for %s on line %u; using default/current binding\n", start, line_number);
            continue;
        }
        KeyBinding *all[] = {&binding_ctrl_a, &binding_ctrl_e, &binding_ctrl_u, &binding_ctrl_k,
            &binding_ctrl_w, &binding_ctrl_l, &binding_ctrl_d, &binding_copy, &binding_paste};
        int conflict = 0;
        for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
            if (all[i] != target && binding_equal(parsed, *all[i])) { conflict = 1; break; }
        }
        if (conflict) {
            fprintf(stderr, "terminator: conflicting binding for %s on line %u; using default/current binding\n", start, line_number);
            continue;
        }
        *target = parsed;
    }
    if (ferror(fp)) fprintf(stderr, "terminator: error reading %s\n", path);
    fclose(fp);
}

static int binding_pressed(XKeyEvent *event, KeySym key, KeyBinding binding)
{
    unsigned int state = event->state & (ControlMask | ShiftMask | Mod1Mask);
    KeySym actual = XLookupKeysym(event, 0);
    (void)key;
    return actual == binding.key && state == binding.modifiers;
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
    XSetForeground(display, gc, WhitePixel(display, DefaultScreen(display)));
    XFillRectangle(display, window, gc, x1, y - 15,
                   (unsigned int)(x2 - x1), (unsigned int)LINE_HEIGHT);
    XSetForeground(display, gc, BlackPixel(display, DefaultScreen(display)));
    XDrawString(display, window, gc, x1, y, text + from, to - from);
    XSetForeground(display, gc, WhitePixel(display, DefaultScreen(display)));
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
                XSetForeground(display, gc, WhitePixel(display, DefaultScreen(display)));
                XFillRectangle(display, window, gc, x1, y - 15,
                               (unsigned int)(x2 - x1), (unsigned int)LINE_HEIGHT);
                XSetForeground(display, gc, BlackPixel(display, DefaultScreen(display)));
                XDrawString(display, window, gc, x1, y, tab->input + from, to - from);
                XSetForeground(display, gc, WhitePixel(display, DefaultScreen(display)));
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

static int process_send(int fd, const ProcessMessage *message)
{
    ssize_t n;
    do { n = send(fd, message, sizeof(*message), MSG_NOSIGNAL); }
    while (n < 0 && errno == EINTR);
    return n == (ssize_t)sizeof(*message) ? 0 : -1;
}

static void history_lock(void)
{
    if (process_mode && shared_history) {
        while (sem_wait(&shared_history->mutex) == -1 && errno == EINTR) { }
    } else {
        pthread_mutex_lock(&history_mutex);
    }
}

static void history_unlock(void)
{
    if (process_mode && shared_history)
        (void)sem_post(&shared_history->mutex);
    else
        pthread_mutex_unlock(&history_mutex);
}

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
    if (process_child_mode && process_child_fd >= 0) {
        ProcessMessage message;
        memset(&message, 0, sizeof(message));
        message.type = PROC_LINE;
        snprintf(message.text, sizeof(message.text), "%s", buffer->lines[buffer->count - 1]);
        (void)process_send(process_child_fd, &message);
    }
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

static void history_file_path(char *path, size_t capacity)
{
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') home = ".";
    (void)snprintf(path, capacity, "%s/.terminator_history", home);
}

static void history_load(void)
{
    char path[PATH_MAX];
    history_file_path(path, sizeof(path));
    FILE *file = fopen(path, "r");
    if (file == NULL) return;

    CommandHistory *history = (process_mode && shared_history) ?
                              &shared_history->history : &command_history;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    while ((length = getline(&line, &capacity, file)) != -1) {
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = '\0';
        if (length == 0) continue;
        if (length >= INPUT_SIZE) line[INPUT_SIZE - 1] = '\0';
        snprintf(history->commands[history->next], INPUT_SIZE, "%s", line);
        history->next = (history->next + 1) % MAX_HISTORY;
        if (history->count < MAX_HISTORY) history->count++;
    }
    free(line);
    fclose(file);

    /* Keep the persistent file bounded to the newest MAX_HISTORY entries. */
    file = fopen(path, "w");
    if (file != NULL) {
        size_t first = (history->next + MAX_HISTORY - history->count) % MAX_HISTORY;
        for (size_t i = 0; i < history->count; ++i) {
            size_t index = (first + i) % MAX_HISTORY;
            fprintf(file, "%s\n", history->commands[index]);
        }
        fclose(file);
    }
}

static int is_history_builtin_line(const char *command)
{
    if (command == NULL) return 0;
    while (*command == ' ' || *command == '\t') command++;
    return strncmp(command, "history", 7) == 0 &&
           (command[7] == '\0' || command[7] == ' ' || command[7] == '\t');
}

static void history_persist(const char *command)
{
    /* Keep the history command in interactive navigation, but not on disk. */
    if (is_history_builtin_line(command)) return;

    char path[PATH_MAX];
    history_file_path(path, sizeof(path));

    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd == -1) return;

    if (flock(fd, LOCK_EX) == -1) {
        close(fd);
        return;
    }

    /*
     * Trim while holding the file lock.  Keeping the newest MAX_HISTORY - 1
     * existing lines leaves room for the command being appended.  Rewriting
     * the same locked inode avoids races between simultaneous Terminator
     * instances that could occur with a temporary-file/rename approach.
     */
    char **entries = calloc(MAX_HISTORY, sizeof(*entries));
    if (entries == NULL) {
        (void)flock(fd, LOCK_UN);
        close(fd);
        return;
    }

    int read_fd = dup(fd);
    FILE *input = read_fd == -1 ? NULL : fdopen(read_fd, "r");
    size_t count = 0;
    size_t next = 0;
    char *line = NULL;
    size_t line_capacity = 0;

    if (input != NULL) {
        while (getline(&line, &line_capacity, input) != -1) {
            size_t len = strcspn(line, "\r\n");
            line[len] = '\0';
            if (len == 0) continue;
            if (len >= INPUT_SIZE) len = INPUT_SIZE - 1;

            char *copy = malloc(len + 1);
            if (copy == NULL) continue;
            memcpy(copy, line, len);
            copy[len] = '\0';

            free(entries[next]);
            entries[next] = copy;
            next = (next + 1) % (MAX_HISTORY - 1);
            if (count < MAX_HISTORY - 1) count++;
        }
        free(line);
        fclose(input);
    } else if (read_fd != -1) {
        close(read_fd);
    }

    char clean_command[INPUT_SIZE];
    size_t command_len = 0;
    while (command[command_len] && command_len < INPUT_SIZE - 1) {
        char c = command[command_len];
        clean_command[command_len] = (c == '\n' || c == '\r') ? ' ' : c;
        command_len++;
    }
    clean_command[command_len] = '\0';

    if (ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) != (off_t)-1) {
        size_t first = (next + (MAX_HISTORY - 1) - count) % (MAX_HISTORY - 1);
        for (size_t i = 0; i < count; ++i) {
            size_t index = (first + i) % (MAX_HISTORY - 1);
            if (entries[index] != NULL) {
                (void)dprintf(fd, "%s\n", entries[index]);
            }
        }
        (void)dprintf(fd, "%s\n", clean_command);
    }

    for (size_t i = 0; i < MAX_HISTORY; ++i) free(entries[i]);
    free(entries);
    (void)flock(fd, LOCK_UN);
    close(fd);
}

static void update_reverse_search(TerminalTab *tab)
{
    tab->search_has_match = 0;
    tab->search_match[0] = '\0';

    history_lock();

    CommandHistory *history = (process_mode && shared_history) ?
                              &shared_history->history : &command_history;

    size_t limit = history->count;

    /* Search newest to oldest; accept only a real substring match. */
    for (size_t offset = 0; offset < limit; ++offset) {
        size_t index =
            (history->next + MAX_HISTORY - 1 - offset) % MAX_HISTORY;
        const char *candidate = history->commands[index];

        if (tab->search_query[0] == '\0' ||
            strstr(candidate, tab->search_query) != NULL) {
            snprintf(tab->search_match, sizeof(tab->search_match),
                     "%s", candidate);
            tab->search_has_match = 1;
            break;
        }
    }

    history_unlock();
}

static void history_add(const char *command)
{
    if (command == NULL || command[0] == '\0')
        return;

    history_lock();
    CommandHistory *history = (process_mode && shared_history) ?
                              &shared_history->history : &command_history;
    snprintf(history->commands[history->next], INPUT_SIZE, "%s", command);
    history->next = (history->next + 1) % MAX_HISTORY;
    if (history->count < MAX_HISTORY) history->count++;
    history_unlock();
    history_persist(command);
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
    char snapshot[1000][INPUT_SIZE];
    size_t shown = 0;

    history_lock();
    CommandHistory *history = (process_mode && shared_history) ?
                              &shared_history->history : &command_history;

    /* Walk newest-to-oldest, skipping history invocations, until 1000 real
       commands have been collected. Then reverse them for chronological output. */
    for (size_t offset = 0; offset < history->count && shown < 1000; ++offset) {
        size_t index = (history->next + MAX_HISTORY - 1 - offset) % MAX_HISTORY;
        const char *candidate = history->commands[index];
        if (is_history_builtin_line(candidate)) continue;
        snprintf(snapshot[shown], sizeof(snapshot[shown]), "%s", candidate);
        shown++;
    }
    history_unlock();

    char line[LINE_SIZE];
    for (size_t i = 0; i < shown; ++i) {
        snprintf(line, sizeof(line), "%zu  %.1000s", shown - i,
                 snapshot[shown - i - 1]);
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

/* Background jobs are stored in stable numbered slots (slot + 1). */
static int has_background_jobs(const TerminalTab *tab)
{
    if (tab->background_command.active) return 1; /* GUI summary in Process Mode. */
    for (int i = 0; i < MAX_BACKGROUND_JOBS; ++i)
        if (tab->background_jobs[i].active) return 1;
    return 0;
}

static int find_free_background_slot(const TerminalTab *tab)
{
    for (int i = 0; i < MAX_BACKGROUND_JOBS; ++i)
        if (!tab->background_jobs[i].active) return i;
    return -1;
}

/* ---------- Tab initialization and cleanup ---------- */

static void init_tab(TerminalTab *tab)
{
    memset(tab, 0, sizeof(*tab));

    tab->command.pid = -1;
    tab->command.output_fd = -1;
    tab->background_command.pid = -1;
    tab->background_command.output_fd = -1;
    for (int i = 0; i < MAX_BACKGROUND_JOBS; ++i) {
        tab->background_jobs[i].pid = -1;
        tab->background_jobs[i].output_fd = -1;
    }
    tab->process_fd = -1;
    tab->process_pid = -1;
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
    for (int i = 0; i < MAX_BACKGROUND_JOBS; ++i)
        stop_one_command(&tab->background_jobs[i]);
}

static void process_child_loop(int index, int fd)
{
    process_child_mode = 1;
    process_child_fd = fd;
    process_child_exit = 0;
    if (display) (void)close(ConnectionNumber(display));
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    TerminalTab *tab = &tabs[index];
    tab->process_fd = -1;
    int alive = 1;
    while (alive && !process_child_exit) {
        struct pollfd pfd = {fd, POLLIN | POLLHUP, 0};
        int pr = poll(&pfd, 1, 20);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            ProcessMessage message;
            ssize_t n = recv(fd, &message, sizeof(message), 0);
            if (n == (ssize_t)sizeof(message)) {
                if (message.type == PROC_COMMAND) {
                    tab->suppress_next_echo = 1;
                    start_command(tab, message.text);
                } else if (message.type == PROC_INTERRUPT) {
                    if (tab->watch.active) stop_multiwatch(tab);
                    if (tab->command.active && tab->command.pid > 0) {
                        (void)kill(-tab->command.pid, SIGINT);
                        (void)kill(tab->command.pid, SIGINT);
                    }
                } else if (message.type == PROC_STOP) {
                    if (tab->command.active && tab->command.pid > 0 && !tab->command.stopped) {
                        (void)kill(-tab->command.pid, SIGTSTP);
                        (void)kill(tab->command.pid, SIGTSTP);
                    }
                } else if (message.type == PROC_SHUTDOWN) {
                    alive = 0;
                }
            } else if (n == 0) alive = 0;
        } else if (pr > 0 && (pfd.revents & (POLLHUP | POLLERR))) {
            alive = 0;
        } else if (pr < 0 && errno != EINTR) alive = 0;

        service_tab(tab);
        service_multiwatch(tab);

        ProcessMessage state;
        memset(&state, 0, sizeof(state));
        state.type = PROC_STATE;
        state.status = tab->last_status;
        state.flags = (tab->command.active ? 1 : 0) |
                      (tab->command.stopped ? 2 : 0) |
                      (has_background_jobs(tab) ? 4 : 0) |
                      (tab->watch.active ? 8 : 0);
        state.command_pid = tab->command.pid;
        state.background_pid = -1;
        for (int j = 0; j < MAX_BACKGROUND_JOBS; ++j) {
            if (tab->background_jobs[j].active) {
                state.background_pid = tab->background_jobs[j].pid;
                break;
            }
        }
        snprintf(state.cwd, sizeof(state.cwd), "%s", tab->cwd);
        if (process_send(fd, &state) == -1) alive = 0;
    }
    stop_tab_command(tab);
    ProcessMessage done;
    memset(&done, 0, sizeof(done));
    done.type = PROC_EXIT;
    done.status = process_child_exit ? 1 : 0;
    (void)process_send(fd, &done);
    close(fd);
    _exit(EXIT_SUCCESS);
}

static int start_process_tab(int index)
{
    int pair[2];
    if (index < 0 || index >= MAX_TABS || socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == -1)
        return -1;
    (void)fcntl(pair[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pair[1], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid == -1) { close(pair[0]); close(pair[1]); return -1; }
    if (pid == 0) {
        close(pair[0]);
        /* Do not keep sibling tabs' IPC endpoints alive in this child. */
        for (int i = 0; i < tab_count; ++i) {
            if (i != index && tabs[i].process_fd >= 0)
                close(tabs[i].process_fd);
        }
        process_child_loop(index, pair[1]);
    }
    close(pair[1]);
    tabs[index].process_pid = pid;
    tabs[index].process_fd = pair[0];
    tabs[index].command.pid = -1;
    tabs[index].background_command.pid = -1;
    for (int j = 0; j < MAX_BACKGROUND_JOBS; ++j) {
        tabs[index].background_jobs[j].pid = -1;
        tabs[index].background_jobs[j].output_fd = -1;
    }
    return 0;
}

static void service_process_tabs(void)
{
    for (int i = 0; i < tab_count; ++i) {
        TerminalTab *tab = &tabs[i];
        if (tab->process_fd < 0) continue;
        for (;;) {
            ProcessMessage message;
            ssize_t n = recv(tab->process_fd, &message, sizeof(message), MSG_DONTWAIT);
            if (n == (ssize_t)sizeof(message)) {
                if (message.type == PROC_LINE) add_line(&tab->buffer, message.text);
                else if (message.type == PROC_STATE) {
                    snprintf(tab->cwd, sizeof(tab->cwd), "%s", message.cwd);
                    tab->last_status = message.status;
                    tab->command.active = (message.flags & 1) != 0;
                    tab->command.stopped = (message.flags & 2) != 0;
                    tab->command.pid = message.command_pid;
                    tab->background_command.active = (message.flags & 4) != 0;
                    tab->background_command.pid = message.background_pid;
                    tab->watch.active = (message.flags & 8) != 0;
                } else if (message.type == PROC_EXIT) {
                    int should_close_tab = message.status == 1;
                    close(tab->process_fd);
                    tab->process_fd = -1;
                    if (tab->process_pid > 0) {
                        int child_status;
                        while (waitpid(tab->process_pid, &child_status, 0) == -1 && errno == EINTR) { }
                    }
                    tab->process_pid = -1;
                    if (should_close_tab) {
                        if (tab_count > 1) {
                            close_tab(i);
                            --i;
                        } else {
                            running = 0;
                        }
                    } else {
                        add_line(&tab->buffer, "[tab process exited unexpectedly]");
                    }
                    break;
                }
            } else {
                if (n == 0) {
                    close(tab->process_fd);
                    tab->process_fd = -1;
                    if (tab->process_pid > 0) {
                        int child_status;
                        while (waitpid(tab->process_pid, &child_status, 0) == -1 && errno == EINTR) { }
                    }
                    tab->process_pid = -1;
                    add_line(&tab->buffer, "[tab process exited unexpectedly]");
                }
                break;
            }
        }
    }
}

static void close_tab(int index)
{
    if (process_mode) {
        if (index < 0 || index >= tab_count) return;
        if (tab_count == 1) {
            add_line(&tabs[0].buffer, "Cannot close the last tab. Use exit or Escape.");
            return;
        }
        TerminalTab *closing = &tabs[index];
        if (closing->process_fd >= 0) {
            ProcessMessage shutdown;
            memset(&shutdown, 0, sizeof(shutdown)); shutdown.type = PROC_SHUTDOWN;
            (void)process_send(closing->process_fd, &shutdown);
            close(closing->process_fd);
            closing->process_fd = -1;
        }
        if (closing->process_pid > 0) {
            int status;
            while (waitpid(closing->process_pid, &status, 0) == -1 && errno == EINTR) { }
        }
        for (int i = index; i < tab_count - 1; ++i) tabs[i] = tabs[i + 1];
        tab_count--;
        memset(&tabs[tab_count], 0, sizeof(tabs[tab_count]));
        tabs[tab_count].process_fd = -1;
        if (active_tab >= tab_count) active_tab = tab_count - 1;
        else if (active_tab > index) active_tab--;
        return;
    }
    /* Actual joining/compaction is performed by the GUI thread after it
       releases state_mutex.  This also works when called by a tab worker
       for the exit built-in. */
    if (index < 0 || index >= tab_count || pending_close_index >= 0)
        return;

    if (tab_count == 1) {
        add_line(&tabs[0].buffer,
                 "Cannot close the last tab. Use exit or Escape.");
        return;
    }

    pending_close_index = index;

    /* Stop the closing tab's jobs now, before allowing its worker to exit. */
    TerminalTab *closing = &tabs[index];
    if (closing->command.active && closing->command.pid > 0) {
        (void)kill(-closing->command.pid, SIGTERM);
        (void)kill(closing->command.pid, SIGTERM);
    }
    for (int j = 0; j < MAX_BACKGROUND_JOBS; ++j) {
        ActiveCommand *job = &closing->background_jobs[j];
        if (job->active && job->pid > 0) {
            (void)kill(-job->pid, SIGTERM);
            (void)kill(job->pid, SIGTERM);
        }
    }
    stop_multiwatch(closing);

    /* All workers are stopped before the fixed tab array is compacted. */
    for (int i = 0; i < tab_count; ++i)
        tabs[i].worker_stop = 1;
    pthread_cond_broadcast(&worker_condition);
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
            _exit(1);_exit(1);
        }
        if (pid == 0) {
            reset_child_signals();
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
        reset_child_signals();
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
        reset_child_signals();
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
    if (tab->command.active || has_background_jobs(tab) || tab->watch.active) {
        add_line(&tab->buffer, "A command or multiWatch is already running in this tab.");
        tab->last_status = 1;
        return;
    }
    tab->watch = parsed;
    tab->watch.active = 1;
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
        reset_child_signals();
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
    int slot = find_free_background_slot(tab);
    if (slot < 0) {
        add_line(&tab->buffer, "Maximum background jobs reached in this tab.");
        (void)kill(-tab->command.pid, SIGTERM);
        (void)kill(tab->command.pid, SIGTERM);
        return;
    }
    tab->command.background = 1;
    tab->background_jobs[slot] = tab->command;
    memset(&tab->command, 0, sizeof(tab->command));
    tab->command.pid = -1;
    tab->command.output_fd = -1;
}


/* ---------------- Task 11: small in-process shell-script interpreter ---------------- */
#define SCRIPT_MAX_LINES 512
#define SCRIPT_MAX_VARS 64
#define SCRIPT_VALUE 512

typedef struct { char name[64]; char value[SCRIPT_VALUE]; } ScriptVar;
typedef struct {
    ScriptVar vars[SCRIPT_MAX_VARS]; int var_count;
    char *args[MAX_ARGS]; int argc;
    int exit_requested; int exit_status;
} ScriptContext;

static void script_set_var(ScriptContext *ctx, const char *name, const char *value)
{
    for (int i=0;i<ctx->var_count;i++) if (!strcmp(ctx->vars[i].name,name)) {
        snprintf(ctx->vars[i].value,sizeof(ctx->vars[i].value),"%s",value); return;
    }
    if (ctx->var_count < SCRIPT_MAX_VARS) {
        ScriptVar *v=&ctx->vars[ctx->var_count++];
        snprintf(v->name,sizeof(v->name),"%.63s",name);
        snprintf(v->value,sizeof(v->value),"%s",value);
    }
}
static const char *script_get_var(ScriptContext *ctx,const char *name)
{
    for(int i=0;i<ctx->var_count;i++) if(!strcmp(ctx->vars[i].name,name)) return ctx->vars[i].value;
    return "";
}
static void script_expand(TerminalTab *tab, ScriptContext *ctx, const char *src,
                          char *dst,size_t cap)
{
    size_t o=0;
    for(size_t i=0;src[i] && o+1<cap;){
        if(src[i]=='$'){
            char key[64]={0}; size_t k=0; const char *val=NULL; i++;
            if(src[i]=='?'){ snprintf(key,sizeof(key),"%d",tab->last_status); val=key; i++; }
            else if(src[i]=='#'){ snprintf(key,sizeof(key),"%d",ctx->argc); val=key; i++; }
            else if(src[i]=='$'){ snprintf(key,sizeof(key),"%ld",(long)getpid()); val=key; i++; }
            else if(isdigit((unsigned char)src[i])){ int n=src[i++]-'0'; val=(n>0&&n<=ctx->argc)?ctx->args[n-1]:""; }
            else if(src[i]=='{') { i++; while(src[i] && src[i]!='}' && k+1<sizeof(key)) key[k++]=src[i++]; if(src[i]=='}')i++; val=script_get_var(ctx,key); }
            else if(isalpha((unsigned char)src[i])||src[i]=='_') { while((isalnum((unsigned char)src[i])||src[i]=='_')&&k+1<sizeof(key)) key[k++]=src[i++]; val=script_get_var(ctx,key); }
            else { if(o+1<cap)dst[o++]='$'; continue; }
            for(size_t j=0;val && val[j] && o+1<cap;j++) dst[o++]=val[j];
        } else dst[o++]=src[i++];
    }
    dst[o]='\0';
}
static char *script_trim(char *s)
{
    while(isspace((unsigned char)*s))s++;
    size_t n=strlen(s); while(n&&isspace((unsigned char)s[n-1]))s[--n]='\0'; return s;
}
static int script_test(TerminalTab *tab, char *expr)
{
    char copy[INPUT_SIZE]; snprintf(copy,sizeof(copy),"%s",expr);
    char *a[8]; int n=0; char *p=copy;
    /* Tokenize test expressions and remove simple surrounding quotes. */
    while (*p && n < 8) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char *start = p; char quote = '\0';
        while (*p) {
            if (quote) { if (*p == quote) quote = '\0'; p++; continue; }
            if (*p == '\'' || *p == '\"') { quote = *p++; continue; }
            if (isspace((unsigned char)*p)) break;
            p++;
        }
        if (*p) *p++ = '\0';
        /* Compact away quote characters in this token. */
        char *r = start, *w = start; quote = '\0';
        while (*r) {
            if (!quote && (*r == '\'' || *r == '\"')) { quote = *r++; continue; }
            if (quote && *r == quote) { quote = '\0'; r++; continue; }
            *w++ = *r++;
        }
        *w = '\0'; a[n++] = start;
    }
    if(n&&(!strcmp(a[0],"[")||!strcmp(a[0],"test"))){
        if(!strcmp(a[0],"[")){ if(n<2||strcmp(a[n-1],"]"))return 0; n--; }
        for(int i=0;i<n;i++) a[i]=a[i];
        if(n==2){ if(!strcmp(a[1],"-n"))return 0; if(!strcmp(a[1],"-z"))return 1; return a[1][0]!=0; }
        if(n==3){
            struct stat st; char testpath[PATH_MAX]; const char *p=a[2];
            if((!strcmp(a[1],"-f")||!strcmp(a[1],"-d")) && p[0]!='/'){
                if(snprintf(testpath,sizeof(testpath),"%s/%s",tab->cwd,p)>=(int)sizeof(testpath))return 0;
                p=testpath;
            }
            if(!strcmp(a[1],"-f"))return stat(p,&st)==0&&S_ISREG(st.st_mode);
            if(!strcmp(a[1],"-d"))return stat(p,&st)==0&&S_ISDIR(st.st_mode);
            if(!strcmp(a[1],"-n"))return a[2][0]!=0;
            if(!strcmp(a[1],"-z"))return a[2][0]==0;
        }
        if(n==4){ if(!strcmp(a[2],"=")||!strcmp(a[2],"=="))return !strcmp(a[1],a[3]); if(!strcmp(a[2],"!="))return strcmp(a[1],a[3])!=0;
            char *e1=NULL,*e2=NULL; long x=strtol(a[1],&e1,10),y=strtol(a[3],&e2,10); if(!*e1&&!*e2){ if(!strcmp(a[2],"-eq"))return x==y; if(!strcmp(a[2],"-ne"))return x!=y; if(!strcmp(a[2],"-gt"))return x>y; if(!strcmp(a[2],"-lt"))return x<y; if(!strcmp(a[2],"-ge"))return x>=y; if(!strcmp(a[2],"-le"))return x<=y; }
        }
    }
    (void)tab; return 0;
}
static int script_find_close(char lines[][INPUT_SIZE],int count,int from,int *elseidx,int *fiidx)
{
    int depth=0; *elseidx=-1; *fiidx=-1;
    for(int i=from+1;i<count;i++){
        char tmp[INPUT_SIZE]; snprintf(tmp,sizeof(tmp),"%s",lines[i]); char *s=script_trim(tmp);
        if(!strncmp(s,"if ",3))depth++;
        else if(!strcmp(s,"fi")){ if(depth==0){*fiidx=i;return 0;} depth--; }
        else if(depth==0&&(!strncmp(s,"elif ",5)||!strcmp(s,"else"))){ if(*elseidx<0)*elseidx=i; }
    }
    return -1;
}
static int script_capture_substitution(const char *value, char *out, size_t out_size)
{
    size_t n = strlen(value);
    if (n < 3 || value[0] != '$' || value[1] != '(' || value[n-1] != ')') return 0;
    char command[INPUT_SIZE];
    size_t command_len = n - 3;
    if (command_len >= sizeof(command)) return -1;
    memcpy(command, value + 2, command_len);
    command[command_len] = '\0';
    FILE *pipe = popen(command, "r");
    if (!pipe) return -1;
    size_t used = 0;
    while (used + 1 < out_size) {
        size_t got = fread(out + used, 1, out_size - used - 1, pipe);
        used += got;
        if (got == 0) break;
    }
    out[used] = '\0';
    int status = pclose(pipe);
    while (used && (out[used-1] == '\n' || out[used-1] == '\r')) out[--used] = '\0';
    return status == -1 ? -1 : 1;
}

static int script_exec_range(TerminalTab *tab,ScriptContext *ctx,char lines[][INPUT_SIZE],int count,int begin,int end,const char *path)
{
    for(int i=begin;i<end&&!ctx->exit_requested;i++){
        char raw[INPUT_SIZE]; snprintf(raw,sizeof(raw),"%s",lines[i]); char *line=script_trim(raw);
        if(!*line||*line=='#')continue;
        if(!strncmp(line,"if ",3)){
            int els=-1,fi=-1; if(script_find_close(lines,count,i,&els,&fi)!=0||fi<0||fi>=end){ char msg[LINE_SIZE]; snprintf(msg,sizeof(msg),"%s:%d: syntax error: missing fi",path,i+1); add_line(&tab->buffer,msg); return 2; }
            int branch=i; int taken=0;
            while(branch<fi){
                char condline[INPUT_SIZE]; snprintf(condline,sizeof(condline),"%s",lines[branch]); char *c=script_trim(condline);
                int body=branch+1, next=fi, e=-1,f=-1; (void)script_find_close(lines,count,branch,&e,&f);
                if(e>=0&&e<fi)next=e;
                int ok=0;
                if(!strncmp(c,"if ",3)||!strncmp(c,"elif ",5)){ char expanded[INPUT_SIZE]; script_expand(tab,ctx,c+(c[0]=='i'?3:5),expanded,sizeof(expanded)); ok=script_test(tab,expanded); }
                else if(!strcmp(c,"else"))ok=1;
                if(ok&&!taken){ script_exec_range(tab,ctx,lines,count,body,next,path); taken=1; }
                if(next==fi)break;
                branch=next;
                if(!strncmp(script_trim(lines[branch]),"elif ",5)||!strcmp(script_trim(lines[branch]),"else"))continue;
                break;
            }
            i=fi; continue;
        }
        if(!strcmp(line,"fi")||!strcmp(line,"then")||!strcmp(line,"else")||!strncmp(line,"elif ",5))continue;
        if(!strncmp(line,"exit",4)&&(line[4]=='\0'||isspace((unsigned char)line[4]))){ char *p=script_trim(line+4); ctx->exit_status=*p?atoi(p):tab->last_status; ctx->exit_requested=1; break; }
        char expanded[INPUT_SIZE]; script_expand(tab,ctx,line,expanded,sizeof(expanded));
        /* Simple NAME=value assignments are kept in this script's context. */
        char *eq=strchr(expanded,'=');
        if(eq&&eq!=expanded){
            int valid=1; size_t name_len=(size_t)(eq-expanded);
            for(char *p=expanded;p<eq;p++)if(!(isalnum((unsigned char)*p)||*p=='_')||(p==expanded&&isdigit((unsigned char)*p)))valid=0;
            char *value=eq+1; size_t value_len=strlen(value); char captured[SCRIPT_VALUE];
            int substitution = script_capture_substitution(value, captured, sizeof(captured));
            if (substitution < 0) valid = 0;
            if (substitution == 0) {
                if(value_len>=2 && ((value[0]=='"'&&value[value_len-1]=='"') || (value[0]=='\''&&value[value_len-1]=='\''))){ value[value_len-1]='\0'; value++; }
                else for(size_t j=0;j<value_len;j++)if(isspace((unsigned char)value[j]))valid=0;
            } else if (substitution > 0) value = captured;
            if(valid&&name_len<64){ *eq='\0'; script_set_var(ctx,expanded,value); tab->last_status=0; continue; }
        }
        start_command(tab,expanded);
        while(tab->command.active&&!tab->command.stopped){ service_tab(tab); if(tab->command.active){struct timespec ts={0,10000000L};nanosleep(&ts,NULL);} }
        if(tab->command.stopped){add_line(&tab->buffer,"script: command stopped; aborting script");return 1;}
    }
    return 0;
}
static int run_script_file(TerminalTab *tab,const char *requested,int argc,char **argv)
{
    char path[PATH_MAX];
    if(requested[0]=='/')snprintf(path,sizeof(path),"%s",requested);
    else if(requested[0]=='~'&&(requested[1]=='/'||requested[1]=='\0')){const char *h=getenv("HOME");if(!h)h="/";snprintf(path,sizeof(path),"%s%s",h,requested+1);}
    else if(snprintf(path,sizeof(path),"%s/%s",tab->cwd,requested)>=(int)sizeof(path)){add_line(&tab->buffer,"script: path is too long");tab->last_status=1;return 1;}
    FILE *fp=fopen(path,"r"); if(!fp){char msg[LINE_SIZE];snprintf(msg,sizeof(msg),"script: %s: %s",requested,strerror(errno));add_line(&tab->buffer,msg);tab->last_status=1;return 1;}
    char lines[SCRIPT_MAX_LINES][INPUT_SIZE]; int count=0,err=0; char buf[INPUT_SIZE];
    while(fgets(buf,sizeof(buf),fp)){ if(count>=SCRIPT_MAX_LINES){err=1;break;} size_t n=strlen(buf); if(n&&buf[n-1]=='\n')buf[--n]='\0'; else if(!feof(fp)){err=1;break;} if(n&&buf[n-1]=='\r')buf[--n]='\0'; snprintf(lines[count++],INPUT_SIZE,"%s",buf); }
    if(ferror(fp))err=1;
    fclose(fp);
    if(err){add_format(&tab->buffer,"script: %s: read error or script exceeds limits",requested);tab->last_status=1;return 1;}
    /* Validate if/elif/else/fi nesting before executing any command. */
    int stack[SCRIPT_MAX_LINES],seen_else[SCRIPT_MAX_LINES],top=0;
    for(int i=0;i<count;i++){
        char tmp[INPUT_SIZE]; memcpy(tmp,lines[i],INPUT_SIZE); char *s=script_trim(tmp);
        if(!strncmp(s,"if ",3)){
            if(top>=SCRIPT_MAX_LINES){add_line(&tab->buffer,"script: too many nested if blocks");tab->last_status=2;return 2;}
            stack[top]=i; seen_else[top]=0; top++;
        } else if(!strcmp(s,"fi")){
            if(!top){char msg[LINE_SIZE];snprintf(msg,sizeof(msg),"%s:%d: unexpected fi",requested,i+1);add_line(&tab->buffer,msg);tab->last_status=2;return 2;}
            top--;
        } else if(!strcmp(s,"else")||!strncmp(s,"elif ",5)){
            if(!top || seen_else[top-1]){
                char msg[LINE_SIZE];snprintf(msg,sizeof(msg),"%s:%d: unexpected or misplaced %s",requested,i+1,s);add_line(&tab->buffer,msg);tab->last_status=2;return 2;
            }
            if(!strcmp(s,"else"))seen_else[top-1]=1;
        }
    }
    if(top){char msg[LINE_SIZE];snprintf(msg,sizeof(msg),"%s:%d: syntax error: missing fi",requested,stack[top-1]+1);add_line(&tab->buffer,msg);tab->last_status=2;return 2;}
    ScriptContext ctx; memset(&ctx,0,sizeof(ctx)); ctx.argc=argc; for(int i=0;i<argc&&i<MAX_ARGS;i++)ctx.args[i]=argv[i];
    int rc=script_exec_range(tab,&ctx,lines,count,0,count,requested);
    tab->last_status=ctx.exit_requested?ctx.exit_status:(rc?rc:tab->last_status);
    return tab->last_status;
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
            snprintf(message, sizeof(message), "[%d] %ld started in background",
                     find_free_background_slot(tab) + 1, (long)tab->command.pid);
            add_line(&tab->buffer, message);
            promote_background_command(tab);
        }
        return;
    }

    if (has_pipeline(expanded)) {
        start_pipeline(tab, original_command, expanded);
        if (run_in_background && tab->command.active) {
            char message[LINE_SIZE];
            snprintf(message, sizeof(message), "[%d] %ld started in background",
                     find_free_background_slot(tab) + 1, (long)tab->command.pid);
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
        if (process_child_mode) {
            process_child_exit = 1;
            return;
        }
        if (tab_count > 1) {
            close_tab((int)(tab - tabs));
        } else {
            running = 0;
        }
        return;
    }

    /* Job-control commands operate on jobs owned by this tab. */
    if (strcmp(argv[0], "jobs") == 0) {
        int found = 0;
        if (tab->command.active) {
            char message[LINE_SIZE];
            snprintf(message, sizeof(message), "[1] %s: %.900s",
                     tab->command.stopped ? "stopped" : "running",
                     tab->command.command_text);
            add_line(&tab->buffer, message);
            found = 1;
        }
        for (int i = 0; i < MAX_BACKGROUND_JOBS; ++i) {
            ActiveCommand *job = &tab->background_jobs[i];
            if (!job->active) continue;
            char message[LINE_SIZE];
            snprintf(message, sizeof(message), "[%d] %s: %.900s", i + 1,
                     job->stopped ? "stopped" : "running (background)",
                     job->command_text);
            add_line(&tab->buffer, message);
            found = 1;
        }
        if (!found) add_line(&tab->buffer, "No running or stopped jobs in this tab.");
        tab->last_status = 0;
        return;
    }
    if (strcmp(argv[0], "fg") == 0 || strcmp(argv[0], "bg") == 0) {
        int to_bg = strcmp(argv[0], "bg") == 0;
        int slot = -1;
        if (argc > 2) {
            add_line(&tab->buffer, to_bg ? "bg: usage: bg [%job]" : "fg: usage: fg [%job]");
            tab->last_status = 2;
            return;
        }
        if (argc == 2) {
            const char *id = argv[1];
            if (*id == '%') ++id;
            char *tail = NULL;
            long number = strtol(id, &tail, 10);
            if (!*id || !tail || *tail || number < 1 || number > MAX_BACKGROUND_JOBS) {
                add_line(&tab->buffer, to_bg ? "bg: invalid job number" : "fg: invalid job number");
                tab->last_status = 2;
                return;
            }
            slot = (int)number - 1;
            if (slot == 0 && !tab->background_jobs[0].active &&
                tab->command.active && tab->command.stopped)
                slot = -2;
        } else {
            for (int i = MAX_BACKGROUND_JOBS - 1; i >= 0; --i) {
                if (tab->background_jobs[i].active) { slot = i; break; }
            }
            if (slot < 0 && tab->command.active && tab->command.stopped) slot = -2;
        }
        ActiveCommand *job = slot == -2 ? &tab->command :
                             (slot >= 0 ? &tab->background_jobs[slot] : NULL);
        if (!job || !job->active || job->pid <= 0) {
            add_line(&tab->buffer, to_bg ? "bg: no such job" : "fg: no such job");
            tab->last_status = 1;
            return;
        }
        if (!to_bg && tab->command.active && job != &tab->command) {
            add_line(&tab->buffer, "fg: a foreground command is already running");
            tab->last_status = 1;
            return;
        }
        if (job->stopped && kill(-job->pid, SIGCONT) == -1 && kill(job->pid, SIGCONT) == -1) {
            add_format(&tab->buffer, to_bg ? "bg: %s" : "fg: %s", strerror(errno));
            tab->last_status = 1;
            return;
        }
        job->stopped = 0;
        if (to_bg) {
            if (job == &tab->command) {
                int free_slot = find_free_background_slot(tab);
                if (free_slot < 0) {
                    add_line(&tab->buffer, "bg: maximum background jobs reached");
                    tab->last_status = 1;
                    return;
                }
                tab->background_jobs[free_slot] = *job;
                memset(job, 0, sizeof(*job)); job->pid = -1; job->output_fd = -1;
                slot = free_slot;
                job = &tab->background_jobs[slot];
            }
            job->background = 1;
            { char msg[LINE_SIZE]; snprintf(msg, sizeof(msg), "[%d] continued in background.", slot + 1); add_line(&tab->buffer, msg); }
        } else {
            tab->command = *job;
            memset(job, 0, sizeof(*job)); job->pid = -1; job->output_fd = -1;
            tab->command.background = 0;
            { char msg[LINE_SIZE]; snprintf(msg, sizeof(msg), "[%d] continued in foreground.", slot + 1); add_line(&tab->buffer, msg); }
        }
        tab->last_status = 0;
        return;
    }

    if (strcmp(argv[0], "source") == 0 || strcmp(argv[0], ".") == 0) {
        if (argc < 2) { add_line(&tab->buffer, "source: usage: source filename [args ...]"); tab->last_status=2; return; }
        (void)run_script_file(tab, argv[1], argc-2, &argv[2]);
        return;
    }

    /* Task 11: bash script.sh and executable ./script.sh are interpreted here. */
    if ((strcmp(argv[0], "bash") == 0 && argc >= 2 && argv[1][0] != '-') ||
        (argv[0][0] == '.' && argv[0][1] == '/')) {
        int script_index = strcmp(argv[0], "bash") == 0 ? 1 : 0;
        int nscript = argc - script_index - 1;
        (void)run_script_file(tab, argv[script_index], nscript, &argv[script_index+1]);
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
        reset_child_signals();
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
            int fd = open(input_path, O_RDONLY);if (fd == -1) { dprintf(STDERR_FILENO, "%s: %s\n", input_path, strerror(errno)); _exit(1); }
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
        snprintf(message, sizeof(message), "[%d] %ld started in background",
                 find_free_background_slot(tab) + 1, (long)pid);
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
        if (background) {
            char msg[LINE_SIZE];
            snprintf(msg, sizeof(msg),
                     "[background job completed; exit status %d]",
                     cmd->exit_status);
            add_line(&tab->buffer, msg);
        }
        cmd->active=0; cmd->pid=-1;
        if (cmd->output_fd>=0) { close(cmd->output_fd); cmd->output_fd=-1; }
    }
}
static void service_tab(TerminalTab *tab)
{
    service_one_command(tab,&tab->command,0);
    for (int i = 0; i < MAX_BACKGROUND_JOBS; ++i)
        service_one_command(tab, &tab->background_jobs[i], 1);
}

/* Worker threads never call Xlib.  They own command launch and job servicing. */
static void *tab_worker_main(void *argument)
{
    int index = *(const int *)argument;

    for (;;) {
        pthread_mutex_lock(&state_mutex);
        if (index < 0 || index >= tab_count || tabs[index].worker_stop) {
            pthread_mutex_unlock(&state_mutex);
            break;
        }

        TerminalTab *tab = &tabs[index];
        if (tab->command_pending) {
            char command[INPUT_SIZE];
            snprintf(command, sizeof(command), "%s", tab->worker_command);
            tab->worker_command[0] = '\0';
            tab->command_pending = 0;
            start_command(tab, command);
        }

        service_tab(tab);
        service_multiwatch(tab);

        if (!tab->worker_stop && !tab->command_pending) {
            struct timespec deadline;
            (void)clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += 20000000L;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }
            (void)pthread_cond_timedwait(&worker_condition, &state_mutex,
                                         &deadline);
        }
        pthread_mutex_unlock(&state_mutex);
    }
    return NULL;
}

static int start_tab_worker(int index)
{
    if (index < 0 || index >= MAX_TABS)
        return -1;
    tabs[index].worker_stop = 0;
    tabs[index].command_pending = 0;
    tabs[index].worker_command[0] = '\0';
    int error = pthread_create(&tabs[index].worker, NULL,
                               tab_worker_main, &worker_indices[index]);
    if (error != 0) {
        char message[LINE_SIZE];
        snprintf(message, sizeof(message), "pthread_create: %s", strerror(error));
        add_line(&tabs[index].buffer, message);
        tabs[index].worker_started = 0;
        return -1;
    }
    tabs[index].worker_started = 1;
    return 0;
}

static void enqueue_tab_command(TerminalTab *tab, const char *command)
{
    if (process_mode) {
        ProcessMessage message;
        memset(&message, 0, sizeof(message));
        message.type = PROC_COMMAND;
        snprintf(message.text, sizeof(message.text), "%s", command);
        if (tab->process_fd < 0 || process_send(tab->process_fd, &message) == -1) {
            add_line(&tab->buffer, "This tab's process worker is not available.");
            tab->last_status = 1;
        }
        return;
    }
    if (!tab->worker_started || tab->worker_stop) {
        add_line(&tab->buffer, "This tab's command worker is not available.");
        tab->last_status = 1;
        return;
    }
    if (tab->command_pending) {
        add_line(&tab->buffer, "A command is already queued in this tab.");
        tab->last_status = 1;
        return;
    }
    snprintf(tab->worker_command, sizeof(tab->worker_command), "%s", command);
    tab->command_pending = 1;
    pthread_cond_broadcast(&worker_condition);
}

/* Called by the main thread only, with state_mutex unlocked. */
static void finish_pending_tab_close(void)
{
    int index = pending_close_index;
    if (index < 0)
        return;

    for (int i = 0; i < tab_count; ++i) {
        if (tabs[i].worker_started) {
            (void)pthread_join(tabs[i].worker, NULL);
            tabs[i].worker_started = 0;
        }
    }

    pthread_mutex_lock(&state_mutex);
    if (index >= 0 && index < tab_count && tab_count > 1) {
        stop_tab_command(&tabs[index]);
        for (int i = index; i < tab_count - 1; ++i)
            tabs[i] = tabs[i + 1];
        tab_count--;
        if (active_tab >= tab_count)
            active_tab = tab_count - 1;
        else if (active_tab > index)
            active_tab--;
    }
    pending_close_index = -1;

    for (int i = 0; i < tab_count; ++i) {
        tabs[i].worker_started = 0;
        tabs[i].worker_stop = 0;
        tabs[i].command_pending = 0;
        (void)start_tab_worker(i);
    }
    pthread_mutex_unlock(&state_mutex);
}

static void stop_all_tab_workers(void)
{
    if (process_mode) {
        for (int i = 0; i < tab_count; ++i) {
            if (tabs[i].process_fd >= 0) {
                ProcessMessage shutdown; memset(&shutdown, 0, sizeof(shutdown));
                shutdown.type = PROC_SHUTDOWN;
                (void)process_send(tabs[i].process_fd, &shutdown);
                close(tabs[i].process_fd); tabs[i].process_fd = -1;
            }
            if (tabs[i].process_pid > 0) {
                int status;
                while (waitpid(tabs[i].process_pid, &status, 0) == -1 && errno == EINTR) { }
                tabs[i].process_pid = -1;
            }
        }
        return;
    }
    pthread_mutex_lock(&state_mutex);
    for (int i = 0; i < tab_count; ++i)
        tabs[i].worker_stop = 1;
    pthread_cond_broadcast(&worker_condition);
    pthread_mutex_unlock(&state_mutex);

    for (int i = 0; i < tab_count; ++i) {
        if (tabs[i].worker_started) {
            (void)pthread_join(tabs[i].worker, NULL);
            tabs[i].worker_started = 0;
        }
    }
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

    /* Divide the header evenly so all nine tabs remain visible. */
    int tab_area_width = window_width - 20;
    int tab_cell_width = tab_count > 0 ? tab_area_width / tab_count : tab_area_width;

    for (int i = 0; i < tab_count; i++) {
        const char *base = strrchr(tabs[i].cwd, '/');
        if (base != NULL && base[1] != '\0')
            base++;
        else
            base = "/";

        const char *status =
            (tabs[i].command.active || has_background_jobs(&tabs[i]) || tabs[i].watch.active)
                ? (tabs[i].command.stopped ? " (stopped)" : " (running)") : "";
        char label[160];
        snprintf(label, sizeof(label), "[%c%d:%s%s]",
                 i == active_tab ? '*' : ' ', i + 1, base, status);

        int max_text_width = tab_cell_width - 8;
        size_t label_len = strlen(label);
        while (label_len > 0 && XTextWidth(font, label, (int)label_len) > max_text_width) {
            /* Keep the tab number and active marker; shorten the directory/status text. */
            if (label_len > 5) {
                label[label_len - 2] = ']';
                label[label_len - 1] = '\0';
                label_len--;
            } else {
                break;
            }
        }

        int tab_x = 10 + i * tab_cell_width;
        XDrawString(display, window, gc, tab_x, 19, label, (int)label_len);
    }

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
        const char *line = tab->buffer.lines[i];
        if (strncmp(line, PROMPT, strlen(PROMPT)) == 0) {
            const char *user_part = "user@terminator";
            const char *suffix_and_command = line + strlen(user_part);
            int user_width = XTextWidth(font, user_part, (int)strlen(user_part));
            XSetForeground(display, gc, prompt_color);
            XDrawString(display, window, gc, LEFT_MARGIN, y,
                        user_part, (int)strlen(user_part));
            XSetForeground(display, gc, WhitePixel(display, DefaultScreen(display)));
            XDrawString(display, window, gc, LEFT_MARGIN + user_width, y,
                        suffix_and_command, (int)strlen(suffix_and_command));
        } else {
            XDrawString(display, window, gc, LEFT_MARGIN, y,
                        line, (int)strlen(line));
        }
    }

    if ((!tab->command.active || tab->command.stopped || tab->command.background) && !tab->watch.active) {
        if (tab->reverse_search) {
            /* Inline reverse search in the existing terminal window (no popup). */
            int bar_height = LINE_HEIGHT * 3 + 10;
            int bar_y = window_height - bar_height;
            if (bar_y < HEADER_HEIGHT) bar_y = HEADER_HEIGHT;
            unsigned long foreground = WhitePixel(display, DefaultScreen(display));
            unsigned long background = BlackPixel(display, DefaultScreen(display));

            XSetForeground(display, gc, background);
            XFillRectangle(display, window, gc, 0, bar_y,
                           (unsigned int)window_width,
                           (unsigned int)(window_height - bar_y));
            XSetForeground(display, gc, foreground);
            XDrawLine(display, window, gc, 0, bar_y,
                      window_width, bar_y);

            char search_line[INPUT_SIZE + 64];
            snprintf(search_line, sizeof(search_line), "(reverse-i-search) Enter search term: %s%s",
                     tab->search_query, tab->search_query[0] ? "" : "_");
            int search_len = (int)strlen(search_line);
            int max_width = window_width - 2 * LEFT_MARGIN;
            while (search_len > 0 && XTextWidth(font, search_line, search_len) > max_width) {
                memmove(search_line, search_line + 1, (size_t)search_len);
                search_len--;
            }
            XDrawString(display, window, gc, LEFT_MARGIN, bar_y + LINE_HEIGHT,
                        search_line, search_len);

            char match_line[INPUT_SIZE + 64];
            if (tab->search_has_match)
                snprintf(match_line, sizeof(match_line), "Match: %s", tab->search_match);
            else
                snprintf(match_line, sizeof(match_line), "Match: (no matching command)");
            int match_len = (int)strlen(match_line);
            while (match_len > 0 && XTextWidth(font, match_line, match_len) > max_width) {
                memmove(match_line, match_line + 1, (size_t)match_len);
                match_len--;
            }
            XDrawString(display, window, gc, LEFT_MARGIN, bar_y + 2 * LINE_HEIGHT,
                        match_line, match_len);
            XDrawString(display, window, gc, LEFT_MARGIN, bar_y + 3 * LINE_HEIGHT,
                        "Enter: accept    Esc: cancel    Ctrl+R: next match",
                        (int)strlen("Enter: accept    Esc: cancel    Ctrl+R: next match"));
        } else {
            if (y > window_height - 5)
                y = window_height - 5;
            const char *visible_prompt = tab->continuation_mode ? "> " : PROMPT;
            int prompt_width = XTextWidth(font, visible_prompt, (int)strlen(visible_prompt));
            if (tab->continuation_mode) {
                XSetForeground(display, gc, WhitePixel(display, DefaultScreen(display)));
                XDrawString(display, window, gc, LEFT_MARGIN, y,
                            visible_prompt, (int)strlen(visible_prompt));
            } else {
                const char *user_part = "user@terminator";
                const char *prompt_suffix = "> ";
                int user_width = XTextWidth(font, user_part, (int)strlen(user_part));
                XSetForeground(display, gc, prompt_color);
                XDrawString(display, window, gc, LEFT_MARGIN, y,
                            user_part, (int)strlen(user_part));
                XSetForeground(display, gc, WhitePixel(display, DefaultScreen(display)));
                XDrawString(display, window, gc, LEFT_MARGIN + user_width, y,
                            prompt_suffix, (int)strlen(prompt_suffix));
            }
            int input_x = LEFT_MARGIN + prompt_width;
            int available_width = window_width - input_x - LEFT_MARGIN;
            if (available_width > 0) {
                int visible_length = (int)tab->input_length;
                int first = 0;
                while (first < visible_length &&
                       XTextWidth(font, tab->input + first, visible_length - first) > available_width)
                    first++;
                XDrawString(display, window, gc, input_x, y,
                            tab->input + first, visible_length - first);
                int cursor_chars = (int)tab->input_cursor - first;
                if (cursor_chars < 0) cursor_chars = 0;
                if (cursor_chars > visible_length - first) cursor_chars = visible_length - first;
                int cursor_x = input_x + XTextWidth(font, tab->input + first, cursor_chars);
                XDrawLine(display, window, gc, cursor_x, y - 14, cursor_x, y + 3);
            }
        }
    }

    draw_text_selection(tab, start, HEADER_HEIGHT + LINE_HEIGHT);
    XFlush(display);
}

/* ---------- Keyboard controls ---------- */

static void select_autocomplete_match(TerminalTab *tab, size_t choice)
{
    if (!tab->completion_active || choice >= tab->completion_count) return;

    const char *name = tab->completion_matches[choice];
    size_t start = tab->completion_start;
    size_t dir_len = tab->completion_dir_prefix_len;
    size_t name_len = strlen(name);
    size_t old_len = tab->input_length;
    size_t old_cursor = tab->input_cursor;
    if (start > old_cursor || old_cursor > old_len || start + dir_len > old_cursor) {
        tab->completion_active = 0;
        return;
    }

    char replacement[INPUT_SIZE];
    size_t new_token_len = dir_len + name_len;
    size_t tail_len = old_len - old_cursor;
    if (start + new_token_len + tail_len + 2 >= sizeof(replacement)) {
        tab->completion_active = 0;
        return;
    }

    memcpy(replacement, tab->input, start);
    if (dir_len > 0)
        memcpy(replacement + start, tab->input + start, dir_len);
    memcpy(replacement + start + dir_len, name, name_len);
    memcpy(replacement + start + new_token_len, tab->input + old_cursor, tail_len);
    size_t new_len = start + new_token_len + tail_len;
    replacement[new_len] = '\0';

    char full_path[PATH_MAX];
    struct stat st;
    int is_directory = 0;
    if (snprintf(full_path, sizeof(full_path), "%s/%s",
                 tab->completion_directory, name) < (int)sizeof(full_path) &&
        stat(full_path, &st) == 0 && S_ISDIR(st.st_mode))
        is_directory = 1;

    char suffix = is_directory ? '/' : ' ';
    if (new_len + 1 < sizeof(replacement)) {
        memmove(replacement + start + new_token_len + 1,
                replacement + start + new_token_len,
                tail_len + 1);
        replacement[start + new_token_len] = suffix;
        new_len++;
    }

    memcpy(tab->input, replacement, new_len + 1);
    tab->input_length = new_len;
    tab->input_cursor = start + new_token_len + 1;
    tab->completion_active = 0;
    tab->completion_count = 0;
}

static void autocomplete_filename(TerminalTab *tab)
{
    size_t cursor = tab->input_cursor;
    size_t start = cursor;
    while (start > 0 && tab->input[start - 1] != ' ' && tab->input[start - 1] != '\t' &&
           tab->input[start - 1] != '\n' && tab->input[start - 1] != '"' && tab->input[start - 1] != '\'')
        --start;
    char token[PATH_MAX];
    size_t token_len = cursor - start;
    if (token_len >= sizeof(token)) return;
    memcpy(token, tab->input + start, token_len); token[token_len] = '\0';

    char directory_part[PATH_MAX] = "";
    char prefix[PATH_MAX];
    snprintf(prefix, sizeof(prefix), "%s", token);
    char *slash = strrchr(prefix, '/');
    if (slash) {
        *slash = '\0';
        snprintf(directory_part, sizeof(directory_part), "%s", prefix);
        memmove(prefix, slash + 1, strlen(slash + 1) + 1);
    }

    char directory[PATH_MAX];
    if (directory_part[0] == '/') snprintf(directory, sizeof(directory), "%s", directory_part);
    else if (directory_part[0] == '\0') snprintf(directory, sizeof(directory), "%s", tab->cwd);
    else if (snprintf(directory, sizeof(directory), "%s/%s", tab->cwd, directory_part) >= (int)sizeof(directory)) return;

    DIR *dir = opendir(directory);
    if (!dir) { add_format(&tab->buffer, "completion: cannot open %s", directory); return; }
    char matches[256][NAME_MAX + 1];
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < 256) {
        if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0) continue;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        snprintf(matches[count], sizeof(matches[count]), "%s", entry->d_name);
        count++;
    }
    closedir(dir);
    if (count == 0) return;

    size_t common = strlen(matches[0]);
    for (size_t i = 1; i < count; ++i) {
        size_t j = 0;
        while (j < common && matches[i][j] == matches[0][j]) ++j;
        common = j;
    }
    size_t prefix_len = strlen(prefix);
    size_t insert_len = common > prefix_len ? common - prefix_len : 0;
    if (count == 1 || insert_len > 0) {
        char completed[INPUT_SIZE];
        size_t old_len = tab->input_length;
        if (old_len + insert_len >= sizeof(completed)) return;
        memcpy(completed, tab->input, start);
        memcpy(completed + start, tab->input + start, token_len);
        /* Replace the typed basename suffix with the common completion. */
        size_t dir_prefix_len = slash ? strlen(directory_part) + 1 : 0;
        size_t token_base_start = start + dir_prefix_len;
        size_t new_token_len = dir_prefix_len + common;
        size_t tail_start = cursor;
        size_t tail_len = old_len - tail_start;
        memmove(completed + start + new_token_len, tab->input + tail_start, tail_len + 1);
        if (dir_prefix_len) memcpy(completed + start, token, dir_prefix_len);
        memcpy(completed + token_base_start, matches[0], common);
        size_t new_len = start + new_token_len + tail_len;
        if (new_len >= sizeof(tab->input)) return;
        memcpy(tab->input, completed, new_len + 1);
        tab->input_length = new_len;
        tab->input_cursor = start + new_token_len;
        if (count == 1) {
            char full[PATH_MAX];
            if (snprintf(full, sizeof(full), "%s/%s", directory, matches[0]) < (int)sizeof(full)) {
                struct stat st;
                if (stat(full, &st) == 0 && S_ISDIR(st.st_mode) && tab->input_cursor < sizeof(tab->input) - 1) {
                    memmove(tab->input + tab->input_cursor + 1, tab->input + tab->input_cursor,
                            tab->input_length - tab->input_cursor + 1);
                    tab->input[tab->input_cursor++] = '/'; tab->input_length++;
                } else if (tab->input_cursor < sizeof(tab->input) - 1) {
                    memmove(tab->input + tab->input_cursor + 1, tab->input + tab->input_cursor,
                            tab->input_length - tab->input_cursor + 1);
                    tab->input[tab->input_cursor++] = ' '; tab->input_length++;
                }
            }
        }
    } else {
        tab->completion_active = 1;
        tab->completion_count = count < 9 ? count : 9;
        tab->completion_start = start;
        tab->completion_dir_prefix_len = slash ? strlen(directory_part) + 1 : 0;
        snprintf(tab->completion_directory, sizeof(tab->completion_directory), "%s", directory);
        for (size_t i = 0; i < tab->completion_count; ++i) {
            snprintf(tab->completion_matches[i], sizeof(tab->completion_matches[i]), "%s", matches[i]);
            char line[LINE_SIZE];
            snprintf(line, sizeof(line), "%zu) %.900s", i + 1, matches[i]);
            add_line(&tab->buffer, line);
        }
        add_line(&tab->buffer, "Press 1-9 to select a match, or Esc to cancel.");
    }
}

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
            if (process_mode) {
                if (start_process_tab(active_tab) == -1)
                    add_line(&tabs[active_tab].buffer, "Could not create tab process.");
            } else {
                (void)start_tab_worker(active_tab);
            }
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
    CommandHistory *history = (process_mode && shared_history) ? &shared_history->history : &command_history;

    if (ctrl && (key == XK_r || key == XK_R) &&
        !(tab->command.active && !tab->command.stopped && !tab->command.background) && !tab->watch.active) {
        if (!tab->reverse_search) {
            memmove(tab->search_original_input, tab->input, tab->input_length + 1);
            tab->search_query[0] = '\0';
            tab->reverse_search = 1;
        }
        update_reverse_search(tab);
        return;
    }
    if (tab->reverse_search) {
        if (key == XK_Escape) {
            tab->reverse_search = 0;
            memmove(tab->input, tab->search_original_input, strlen(tab->search_original_input) + 1);
            tab->input_length = strlen(tab->input); tab->input_cursor = tab->input_length;
            return;
        }
        if (key == XK_Return || key == XK_KP_Enter) {
            if (tab->search_has_match)
                memmove(tab->input, tab->search_match, strlen(tab->search_match) + 1);
            else
                memmove(tab->input, tab->search_original_input, strlen(tab->search_original_input) + 1);
            tab->input_length = strlen(tab->input); tab->input_cursor = tab->input_length;
            tab->reverse_search = 0; tab->history_position = 0;
            return;
        }
        if (key == XK_BackSpace) {
            size_t n = strlen(tab->search_query);
            if (n > 0) tab->search_query[n - 1] = '\0';
            update_reverse_search(tab);
            return;
        }
        if (!ctrl && length > 0) {
            size_t n = strlen(tab->search_query);
            for (int i = 0; i < length && n + 1 < sizeof(tab->search_query); ++i) {
                unsigned char ch = (unsigned char)text[i];
                if (ch >= 32 && ch <= 126) tab->search_query[n++] = (char)ch;
            }
            tab->search_query[n] = '\0';
            update_reverse_search(tab);
            return;
        }
        return;
    }

    if (tab->completion_active) {
        if (key >= XK_1 && key <= XK_9) {
            select_autocomplete_match(tab, (size_t)(key - XK_1));
            return;
        }
        if (key == XK_Escape) {
            tab->completion_active = 0;
            tab->completion_count = 0;
            return;
        }
        /* Any other key cancels the old list; process the key normally. */
        tab->completion_active = 0;
        tab->completion_count = 0;
    }

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
        history_lock();
        if (history->count > 0) {
            if (tab->history_position == 0) {
                memmove(tab->saved_input, tab->input, tab->input_length + 1);
                tab->history_position = history->count + 1;
            }
            if (tab->history_position > 1) tab->history_position--;
            size_t idx = (history->next + MAX_HISTORY -
                          (history->count - (tab->history_position - 1))) % MAX_HISTORY;
            if (tab->history_position <= history->count) {
                snprintf(tab->input, sizeof(tab->input), "%s", history->commands[idx]);
                tab->input_length = strlen(tab->input); tab->input_cursor = tab->input_length;
            }
        }
        history_unlock();
        return;
    }
    if (key == XK_Down && tab->history_position > 0) {
        history_lock();
        if (tab->history_position < history->count) {
            tab->history_position++;
            size_t idx = (history->next + MAX_HISTORY -
                          (history->count - (tab->history_position - 1))) % MAX_HISTORY;
            snprintf(tab->input, sizeof(tab->input), "%s", history->commands[idx]);
            tab->input_length = strlen(tab->input); tab->input_cursor = tab->input_length;
        } else {
            tab->history_position = 0;
            memmove(tab->input, tab->saved_input, strlen(tab->saved_input) + 1);
            tab->input_length = strlen(tab->input); tab->input_cursor = tab->input_length;
        }
        history_unlock();
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

    if (key == XK_Escape) {
        running = 0;
        return;
    }

    if (ctrl && (key == XK_c || key == XK_C)) {
        if (process_mode && tab->process_fd >= 0) {
            ProcessMessage message; memset(&message, 0, sizeof(message));
            message.type = PROC_INTERRUPT; (void)process_send(tab->process_fd, &message);
        } else if (tab->watch.active) {
            stop_multiwatch(tab);
        } else if (tab->command.active && tab->command.pid > 0) {
            (void)kill(-tab->command.pid, SIGINT);
            (void)kill(tab->command.pid, SIGINT);
        }
        return;
    }

    if (ctrl && (key == XK_z || key == XK_Z)) {
        if (process_mode && tab->process_fd >= 0) {
            ProcessMessage message; memset(&message, 0, sizeof(message));
            message.type = PROC_STOP; (void)process_send(tab->process_fd, &message);
        } else if (tab->command.active && tab->command.pid > 0 &&
            !tab->command.stopped) {
            if (kill(-tab->command.pid, SIGTSTP) == -1)
                (void)kill(tab->command.pid, SIGTSTP);
            add_line(&tab->buffer, "Sent Ctrl+Z (SIGTSTP) to current job.");
        }
        return;
    }

    if ((tab->command.active && !tab->command.stopped && !tab->command.background) || tab->watch.active)
        return;

    if (key == XK_Tab) {
        autocomplete_filename(tab);
        return;
    }

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
        enqueue_tab_command(tab, command);
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

int main(int argc, char **argv)
{
    /* Use Indian Standard Time for external commands and local timestamps. */
    if (setenv("TZ", "Asia/Kolkata", 1) == 0)
        tzset();

    process_mode = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "useProcess=True") == 0 || strcmp(argv[i], "--process") == 0)
            process_mode = 1;
        else if (strcmp(argv[i], "useProcess=False") != 0) {
            fprintf(stderr, "Usage: %s [useProcess=True|useProcess=False|--process]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (process_mode) {
        shared_history = mmap(NULL, sizeof(*shared_history), PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (shared_history == MAP_FAILED) { perror("mmap shared history"); return EXIT_FAILURE; }
        memset(shared_history, 0, sizeof(*shared_history));
        if (sem_init(&shared_history->mutex, 1, 1) == -1) {
            perror("sem_init"); munmap(shared_history, sizeof(*shared_history)); return EXIT_FAILURE;
        }
    }
    history_load();
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
        WhitePixel(display, screen),
        BlackPixel(display, screen));

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

    XSetForeground(display, gc, WhitePixel(display, screen));
    XColor green_exact;
    XColor green_color;
    if (XAllocNamedColor(display, DefaultColormap(display, screen),
                         "#00FF00", &green_color, &green_exact)) {
        prompt_color = green_color.pixel;
    } else {
        prompt_color = WhitePixel(display, screen);
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

    /* command_history was initialized by static storage and populated by
       history_load(); do not clear it here or Thread Mode loses persisted history. */
    init_tab(&tabs[0]);
    if (process_mode) {
        if (start_process_tab(0) == -1) {
            fprintf(stderr, "Could not create initial tab process.\n");
            XFreeFont(display, font); XFreeGC(display, gc);
            XDestroyWindow(display, window); XCloseDisplay(display);
            return EXIT_FAILURE;
        }
    } else {
        (void)start_tab_worker(0);
    }

    XMapWindow(display, window);
    XFlush(display);

    int x_fd = ConnectionNumber(display);

    for (;;) {
        pthread_mutex_lock(&state_mutex);
        int keep_running = running;
        pthread_mutex_unlock(&state_mutex);
        if (!keep_running)
            break;

        struct pollfd x_poll;
        x_poll.fd = x_fd;
        x_poll.events = POLLIN;
        x_poll.revents = 0;
        int result = poll(&x_poll, 1, 50);
        if (result == -1 && errno != EINTR) {
            perror("poll");
            break;
        }

        if (process_mode) service_process_tabs();

        /* Workers and GUI share tab state, but only this thread touches X11. */
        pthread_mutex_lock(&state_mutex);
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
            if (pending_close_index >= 0)
                break;
        }

        draw_window();
        int close_requested = pending_close_index >= 0;
        pthread_mutex_unlock(&state_mutex);

        if (close_requested)
            finish_pending_tab_close();
    }

    stop_all_tab_workers();
    if (!process_mode) {
        pthread_mutex_lock(&state_mutex);
        for (int i = 0; i < tab_count; i++)
            stop_tab_command(&tabs[i]);
        pthread_mutex_unlock(&state_mutex);
    }

    XFreeFont(display, font);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    if (process_mode && shared_history) {
        (void)sem_destroy(&shared_history->mutex);
        (void)munmap(shared_history, sizeof(*shared_history));
        shared_history = NULL;
    }

    return EXIT_SUCCESS;
}