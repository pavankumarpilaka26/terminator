
#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>

#define MAX_TABS 9
#define MAX_LINES 1000
#define LINE_SIZE 1024
#define INPUT_SIZE 1024
#define MAX_ARGS 128

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
    char partial_line[LINE_SIZE];
    size_t partial_length;
} ActiveCommand;

typedef struct {
    TerminalBuffer buffer;
    char input[INPUT_SIZE];
    size_t input_length;

    char cwd[PATH_MAX];

    ActiveCommand command;

    /*
     * scroll_offset == 0 means follow the newest output.
     * A positive value is the one-based starting history line.
     */
    int scroll_offset;
} TerminalTab;

static Display *display;
static Window window;
static GC gc;
static XFontStruct *font;
static Atom wm_delete;

static TerminalTab tabs[MAX_TABS];
static int tab_count = 1;
static int active_tab = 0;
static int running = 1;

static int window_width = WINDOW_WIDTH;
static int window_height = WINDOW_HEIGHT;

/* ---------------------------------------------------------
   Terminal history
   --------------------------------------------------------- */

static void add_line(TerminalBuffer *buffer, const char *text)
{
    if (buffer->count >= MAX_LINES) {
        memmove(buffer->lines,
                buffer->lines + 1,
                (MAX_LINES - 1) * sizeof(buffer->lines[0]));

        buffer->count = MAX_LINES - 1;
    }

    snprintf(buffer->lines[buffer->count],
             LINE_SIZE, "%s", text);

    buffer->count++;
}

/* Add command output to the correct tab. */
static void consume_output(
    TerminalTab *tab,
    const char *data,
    size_t length)
{
    ActiveCommand *cmd = &tab->command;

    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)data[i];

        if (ch == '\r') {
            continue;
        }

        if (ch == '\n') {
            cmd->partial_line[cmd->partial_length] = '\0';

            add_line(&tab->buffer, cmd->partial_line);

            cmd->partial_length = 0;
            cmd->partial_line[0] = '\0';
        } else if (ch == '\t') {
            for (int j = 0; j < 4; j++) {
                if (cmd->partial_length < LINE_SIZE - 1) {
                    cmd->partial_line[cmd->partial_length++] = ' ';
                }
            }
        } else if (ch >= 32 || ch == '\b') {
            if (cmd->partial_length < LINE_SIZE - 1) {
                cmd->partial_line[cmd->partial_length++] = (char)ch;
            }
        }
    }
}

static void flush_partial_output(TerminalTab *tab)
{
    ActiveCommand *cmd = &tab->command;

    if (cmd->partial_length == 0) {
        return;
    }

    cmd->partial_line[cmd->partial_length] = '\0';
    add_line(&tab->buffer, cmd->partial_line);

    cmd->partial_length = 0;
    cmd->partial_line[0] = '\0';
}

/* ---------------------------------------------------------
   Basic command parser
   --------------------------------------------------------- */

/*
 * Supports whitespace-separated arguments, basic single/double
 * quotes and backslash escaping.
 *
 * Pipelines, redirection and complete Bash syntax are not
 * implemented by this parser.
 */
static int parse_command(char *command, char *argv[], int max_args)
{
    char *read_pos = command;
    char *write_pos = command;
    int argc = 0;

    while (*read_pos != '\0') {
        while (*read_pos == ' ' ||
               *read_pos == '\t' ||
               *read_pos == '\n') {
            read_pos++;
        }

        if (*read_pos == '\0') {
            break;
        }

        if (argc >= max_args - 1) {
            break;
        }

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

            if (ch == '\\' && quote != '\'') {
                if (*read_pos != '\0') {
                    *write_pos++ = *read_pos++;
                } else {
                    *write_pos++ = '\\';
                }

                continue;
            }

            *write_pos++ = ch;
        }

        *write_pos++ = '\0';
    }

    argv[argc] = NULL;
    return argc;
}

/* ---------------------------------------------------------
   Per-tab working directory
   --------------------------------------------------------- */

/*
 * Validate a directory in a temporary child process.
 *
 * This is important: calling chdir() in the GUI process would
 * make all tabs share one working directory.
 */
static int change_directory(TerminalTab *tab, const char *path)
{
    char target[PATH_MAX];

    if (path == NULL || path[0] == '\0' ||
        strcmp(path, "~") == 0) {
        const char *home = getenv("HOME");

        if (home == NULL || home[0] == '\0') {
            add_line(&tab->buffer, "cd: HOME is not set");
            return -1;
        }

        if (snprintf(target, sizeof(target), "%s", home)
            >= (int)sizeof(target)) {
            add_line(&tab->buffer, "cd: path is too long");
            return -1;
        }
    } else if (path[0] == '~' && path[1] == '/') {
        const char *home = getenv("HOME");

        if (home == NULL) {
            home = "/";
        }

        if (snprintf(target, sizeof(target),
                     "%s%s", home, path + 1)
            >= (int)sizeof(target)) {
            add_line(&tab->buffer, "cd: path is too long");
            return -1;
        }
    } else if (path[0] == '/') {
        if (snprintf(target, sizeof(target), "%s", path)
            >= (int)sizeof(target)) {
            add_line(&tab->buffer, "cd: path is too long");
            return -1;
        }
    } else {
        if (snprintf(target, sizeof(target),
                     "%s/%s", tab->cwd, path)
            >= (int)sizeof(target)) {
            add_line(&tab->buffer, "cd: path is too long");
            return -1;
        }
    }

    int pipefd[2];

    if (pipe(pipefd) == -1) {
        char message[LINE_SIZE];

        snprintf(message, sizeof(message),
                 "cd: pipe: %s", strerror(errno));

        add_line(&tab->buffer, message);
        return -1;
    }

    pid_t pid = fork();

    if (pid == -1) {
        char message[LINE_SIZE];

        snprintf(message, sizeof(message),
                 "cd: fork: %s", strerror(errno));

        add_line(&tab->buffer, message);

        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);

        if (chdir(target) == -1) {
            dprintf(pipefd[1], "ERROR:%s", strerror(errno));
        } else {
            char resolved[PATH_MAX];

            if (getcwd(resolved, sizeof(resolved)) != NULL) {
                dprintf(pipefd[1], "%s", resolved);
            } else {
                dprintf(pipefd[1], "ERROR:%s", strerror(errno));
            }
        }

        close(pipefd[1]);
        _exit(0);
    }

    close(pipefd[1]);

    char result[PATH_MAX + 64];
    size_t used = 0;

    while (used < sizeof(result) - 1) {
        ssize_t n = read(pipefd[0],
                         result + used,
                         sizeof(result) - 1 - used);

        if (n > 0) {
            used += (size_t)n;
        } else if (n == -1 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }

    close(pipefd[0]);

    while (waitpid(pid, NULL, 0) == -1 && errno == EINTR) {
    }

    result[used] = '\0';

    if (used == 0) {
        add_line(&tab->buffer, "cd: could not resolve directory");
        return -1;
    }

    if (strncmp(result, "ERROR:", 6) == 0) {
        char message[LINE_SIZE];

        snprintf(message, sizeof(message),
                 "cd: %s", result + 6);

        add_line(&tab->buffer, message);
        return -1;
    }

if (strlen(result) >= sizeof(tab->cwd)) {
    add_line(&tab->buffer, "cd: resolved path is too long");
    return -1;
}

memcpy(tab->cwd, result, strlen(result) + 1);
    return 0;
}

/* ---------------------------------------------------------
   Tab initialization and closing
   --------------------------------------------------------- */

static void init_tab(TerminalTab *tab)
{
    memset(tab, 0, sizeof(*tab));

    tab->command.pid = -1;
    tab->command.output_fd = -1;

    if (getcwd(tab->cwd, sizeof(tab->cwd)) == NULL) {
        snprintf(tab->cwd, sizeof(tab->cwd), "/");
    }

    add_line(&tab->buffer, "Terminator - Custom Terminal");
    add_line(&tab->buffer,
             "Ctrl+T: new tab | Ctrl+W: close tab | Ctrl+1..9: switch");
}

static void close_tab(int index)
{
    if (index < 0 || index >= tab_count) {
        return;
    }

    if (tab_count == 1) {
        add_line(&tabs[0].buffer,
                 "Cannot close the last tab. Press Escape to exit.");
        return;
    }

    TerminalTab *tab = &tabs[index];

    if (tab->command.active && tab->command.pid > 0) {
        /*
         * This prototype starts one child per tab. The process-group
         * signal also targets ordinary descendants that inherit the
         * child's process group.
         */
        (void)kill(-tab->command.pid, SIGTERM);
        (void)kill(tab->command.pid, SIGTERM);

        while (waitpid(tab->command.pid, NULL, 0) == -1 &&
               errno == EINTR) {
        }
    }

    if (tab->command.output_fd >= 0) {
        close(tab->command.output_fd);
    }

    for (int i = index; i < tab_count - 1; i++) {
        tabs[i] = tabs[i + 1];
    }

    tab_count--;

    if (active_tab >= tab_count) {
        active_tab = tab_count - 1;
    }

    if (active_tab > index) {
        active_tab--;
    }
}

/* ---------------------------------------------------------
   Start and monitor external commands
   --------------------------------------------------------- */

static void start_command(TerminalTab *tab, const char *command)
{
    if (tab->command.active) {
        add_line(&tab->buffer,
                 "A command is already running in this tab.");
        return;
    }

    char copy[INPUT_SIZE];

    snprintf(copy, sizeof(copy), "%s", command);

    char *argv[MAX_ARGS];
    int argc = parse_command(copy, argv, MAX_ARGS);

    if (argc == 0) {
        return;
    }

    char submitted[LINE_SIZE];

    snprintf(submitted, sizeof(submitted),
             "%s%s", PROMPT, command);

    add_line(&tab->buffer, submitted);

    if (strcmp(argv[0], "cd") == 0) {
        if (argc > 2) {
            add_line(&tab->buffer, "cd: too many arguments");
        } else {
            const char *path = argc == 1 ? NULL : argv[1];
            change_directory(tab, path);
        }

        return;
    }

    if (strcmp(argv[0], "exit") == 0) {
        running = 0;
        return;
    }

    int pipefd[2];

    if (pipe(pipefd) == -1) {
        char message[LINE_SIZE];

        snprintf(message, sizeof(message),
                 "pipe: %s", strerror(errno));

        add_line(&tab->buffer, message);
        return;
    }

    pid_t pid = fork();

    if (pid == -1) {
        char message[LINE_SIZE];

        snprintf(message, sizeof(message),
                 "fork: %s", strerror(errno));

        add_line(&tab->buffer, message);

        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }

    if (pid == 0) {
        (void)setpgid(0, 0);

        close(pipefd[0]);

        if (dup2(pipefd[1], STDOUT_FILENO) == -1 ||
            dup2(pipefd[1], STDERR_FILENO) == -1) {
            _exit(126);
        }

        close(pipefd[1]);

        if (chdir(tab->cwd) == -1) {
            dprintf(STDERR_FILENO,
                    "chdir: %s\n", strerror(errno));
            _exit(126);
        }

        execvp(argv[0], argv);

        dprintf(STDERR_FILENO,
                "terminator: %s: %s\n",
                argv[0], strerror(errno));

        _exit(127);
    }

    (void)setpgid(pid, pid);

    close(pipefd[1]);

    int flags = fcntl(pipefd[0], F_GETFL, 0);

    if (flags == -1 ||
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) == -1) {
        close(pipefd[0]);
        (void)kill(-pid, SIGTERM);

        while (waitpid(pid, NULL, 0) == -1 && errno == EINTR) {
        }

        add_line(&tab->buffer,
                 "Error: could not configure command output pipe");
        return;
    }

    tab->command.pid = pid;
    tab->command.output_fd = pipefd[0];
    tab->command.active = 1;
    tab->command.pipe_eof = 0;
    tab->command.child_reaped = 0;
    tab->command.partial_length = 0;
    tab->command.partial_line[0] = '\0';
}

static void service_tab(TerminalTab *tab)
{
    ActiveCommand *cmd = &tab->command;

    if (!cmd->active) {
        return;
    }

    if (!cmd->pipe_eof && cmd->output_fd >= 0) {
        char data[4096];

        for (;;) {
            ssize_t n = read(cmd->output_fd, data, sizeof(data));

            if (n > 0) {
                consume_output(tab, data, (size_t)n);
            } else if (n == 0) {
                cmd->pipe_eof = 1;
                close(cmd->output_fd);
                cmd->output_fd = -1;
                break;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                cmd->pipe_eof = 1;
                close(cmd->output_fd);
                cmd->output_fd = -1;
                break;
            }
        }
    }

    if (!cmd->child_reaped) {
        int status;
        pid_t result = waitpid(cmd->pid, &status, WNOHANG);

        if (result == cmd->pid) {
            cmd->child_reaped = 1;
        } else if (result == -1 && errno != EINTR) {
            cmd->child_reaped = 1;
        }
    }

    if (cmd->pipe_eof && cmd->child_reaped) {
        flush_partial_output(tab);

        cmd->active = 0;
        cmd->pid = -1;

        add_line(&tab->buffer, "[command completed]");
    }
}

/* ---------------------------------------------------------
   Drawing
   --------------------------------------------------------- */

static void draw_window(void)
{
    XClearWindow(display, window);

    /* Tab bar */
    int x = 10;

    for (int i = 0; i < tab_count; i++) {
        const char *base = strrchr(tabs[i].cwd, '/');

        if (base != NULL && base[1] != '\0') {
            base++;
        } else {
            base = "/";
        }

        char label[160];

        snprintf(label, sizeof(label),
                 "[%c %d: %.80s%s]",
                 i == active_tab ? '*' : ' ',
                 i + 1,
                 base,
                 tabs[i].command.active ? " (running)" : "");

        XDrawString(display, window, gc,
                    x, 19, label, (int)strlen(label));

        x += (int)strlen(label) * 8 + 12;
    }

    XDrawString(display, window, gc,
                x, 19,
                "Ctrl+T new | Ctrl+W close | Ctrl+1..9 switch",
                44);

    TerminalTab *tab = &tabs[active_tab];

    int visible_rows =
        (window_height - HEADER_HEIGHT - 40) / LINE_HEIGHT;

    if (visible_rows < 1) {
        visible_rows = 1;
    }

    int max_start = tab->buffer.count > visible_rows
                    ? tab->buffer.count - visible_rows : 0;

    int start;

    if (tab->scroll_offset == 0) {
        start = max_start;
    } else {
        start = tab->scroll_offset - 1;

        if (start < 0) {
            start = 0;
        }

        if (start > max_start) {
            start = max_start;
        }
    }

    int y = HEADER_HEIGHT + LINE_HEIGHT;

    for (int i = start;
         i < tab->buffer.count && y < window_height - LINE_HEIGHT;
         i++, y += LINE_HEIGHT) {
        XDrawString(display, window, gc,
                    LEFT_MARGIN, y,
                    tab->buffer.lines[i],
                    (int)strlen(tab->buffer.lines[i]));
    }

    /*
     * Draw the prompt immediately after history when idle.
     * While a command is running, show its status in the tab bar.
     */
    if (!tab->command.active) {
        if (y > window_height - 5) {
            y = window_height - 5;
        }

        XDrawString(display, window, gc,
                    LEFT_MARGIN, y,
                    PROMPT, (int)strlen(PROMPT));

        int prompt_width = XTextWidth(
            font, PROMPT, (int)strlen(PROMPT));

        int input_x = LEFT_MARGIN + prompt_width;
        int available_width = window_width - input_x - LEFT_MARGIN;

        if (available_width > 0) {
            int input_len = (int)tab->input_length;
            int first = 0;

            while (first < input_len &&
                   XTextWidth(font, tab->input + first,
                              input_len - first) > available_width) {
                first++;
            }

            XDrawString(display, window, gc,
                        input_x, y,
                        tab->input + first,
                        input_len - first);
        }
    }

    XFlush(display);
}

/* ---------------------------------------------------------
   Keyboard controls
   --------------------------------------------------------- */

static void handle_key(XKeyEvent *event)
{
    char text[64] = {0};
    KeySym key = NoSymbol;

    int length = XLookupString(
        event, text, (int)sizeof(text), &key, NULL);

    int ctrl = (event->state & ControlMask) != 0;

    /* Ctrl+T: new tab */
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

    /* Ctrl+W: close active tab */
    if (ctrl && (key == XK_w || key == XK_W)) {
        close_tab(active_tab);
        return;
    }

    /* Ctrl+number: switch tabs */
    if (ctrl && key >= XK_1 && key <= XK_9) {
        int index = (int)(key - XK_1);

        if (index < tab_count) {
            active_tab = index;
        }

        return;
    }

    /* F1-F9: switch tabs */
    if (key >= XK_F1 && key <= XK_F9) {
        int index = (int)(key - XK_F1);

        if (index < tab_count) {
            active_tab = index;
        }

        return;
    }

    TerminalTab *tab = &tabs[active_tab];

    /* Scroll history up */
    if (key == XK_Up) {
        int visible =
            (window_height - HEADER_HEIGHT - 40) / LINE_HEIGHT;

        int max_start = tab->buffer.count > visible
                        ? tab->buffer.count - visible : 0;

        if (max_start > 0) {
            if (tab->scroll_offset == 0) {
                tab->scroll_offset = max_start;
            } else if (tab->scroll_offset > 1) {
                tab->scroll_offset--;
            }
        }

        return;
    }

    /* Scroll history down */
    if (key == XK_Down) {
        if (tab->scroll_offset > 0) {
            tab->scroll_offset++;

            int visible =
                (window_height - HEADER_HEIGHT - 40) / LINE_HEIGHT;

            int max_start = tab->buffer.count > visible
                            ? tab->buffer.count - visible : 0;

            if (tab->scroll_offset > max_start) {
                tab->scroll_offset = 0;
            }
        }

        return;
    }

    /* Ctrl+L: clear visible history */
    if (ctrl && (key == XK_l || key == XK_L)) {
        tab->buffer.count = 0;
        tab->scroll_offset = 0;
        return;
    }

    /* Escape: exit prototype */
    if (key == XK_Escape) {
        running = 0;
        return;
    }

    if (tab->command.active) {
        return;
    }

    /* Backspace */
    if (key == XK_BackSpace) {
        if (tab->input_length > 0) {
            tab->input[--tab->input_length] = '\0';
        }

        return;
    }

    /* Enter */
    if (key == XK_Return || key == XK_KP_Enter) {
        tab->input[tab->input_length] = '\0';

        char command[INPUT_SIZE];

        snprintf(command, sizeof(command), "%s", tab->input);

        tab->input_length = 0;
        tab->input[0] = '\0';
        tab->scroll_offset = 0;

        start_command(tab, command);
        return;
    }

    /* Regular printable characters */
    if (!ctrl && length > 0 &&
        tab->input_length + (size_t)length < sizeof(tab->input)) {
        for (int i = 0; i < length; i++) {
            unsigned char ch = (unsigned char)text[i];

            if (ch >= 32 && ch <= 126) {
                tab->input[tab->input_length++] = (char)ch;
            }
        }

        tab->input[tab->input_length] = '\0';
    }
}

/* ---------------------------------------------------------
   Main event loop
   --------------------------------------------------------- */

int main(void)
{
    display = XOpenDisplay(NULL);

    if (display == NULL) {
        fprintf(stderr,
                "Cannot open X display. Check DISPLAY.\n");
        return EXIT_FAILURE;
    }

    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);

    window = XCreateSimpleWindow(
        display, root,
        100, 100,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        1,
        BlackPixel(display, screen),
        WhitePixel(display, screen));

    XStoreName(display, window, "Terminator - Custom Terminal");

    wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete, 1);

    XSelectInput(display, window,
                 ExposureMask | KeyPressMask | StructureNotifyMask);

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

    init_tab(&tabs[0]);

    XMapWindow(display, window);
    XFlush(display);

    int x_fd = ConnectionNumber(display);

    while (running) {
        /*
         * Poll the X11 connection and every active tab's output pipe.
         * Hidden tabs are serviced just like the active tab.
         */
        struct pollfd fds[1 + MAX_TABS];
        int owners[1 + MAX_TABS];
        nfds_t nfds = 1;

        fds[0].fd = x_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        owners[0] = -1;

        for (int i = 0; i < tab_count; i++) {
            if (tabs[i].command.active &&
                tabs[i].command.output_fd >= 0) {
                fds[nfds].fd = tabs[i].command.output_fd;
                fds[nfds].events = POLLIN | POLLHUP | POLLERR;
                fds[nfds].revents = 0;
                owners[nfds] = i;
                nfds++;
            }
        }

        int result = poll(fds, nfds, 100);

        if (result == -1 && errno != EINTR) {
            perror("poll");
            break;
        }

        /*
         * Read output from the tab that owns each pipe.
         * Do not assume that only the visible tab can produce output.
         */
        for (nfds_t i = 1; i < nfds; i++) {
            if (fds[i].revents == 0) {
                continue;
            }

            int owner = owners[i];

            if (owner < 0 || owner >= tab_count) {
                continue;
            }

            TerminalTab *tab = &tabs[owner];

            /*
             * The pipe descriptor can be closed only by servicing this
             * tab or closing the tab. Both happen in the event loop.
             */
            if (!tab->command.active ||
                tab->command.output_fd != fds[i].fd) {
                continue;
            }

            service_tab(tab);
        }

        /* Also reap children that exited without more output. */
        for (int i = 0; i < tab_count; i++) {
            service_tab(&tabs[i]);
        }

        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);

            if (event.type == Expose) {
                if (event.xexpose.count == 0) {
                    draw_window();
                }
            } else if (event.type == ConfigureNotify) {
                window_width = event.xconfigure.width;
                window_height = event.xconfigure.height;
                draw_window();
            } else if (event.type == KeyPress) {
                handle_key(&event.xkey);
                draw_window();
            } else if (event.type == ClientMessage) {
                if ((Atom)event.xclient.data.l[0] == wm_delete) {
                    running = 0;
                }
            }
        }

        draw_window();
    }

    /* Stop running children before exiting. */
    for (int i = 0; i < tab_count; i++) {
        TerminalTab *tab = &tabs[i];

        if (tab->command.active && tab->command.pid > 0) {
            (void)kill(-tab->command.pid, SIGTERM);
            (void)kill(tab->command.pid, SIGTERM);

            while (waitpid(tab->command.pid, NULL, 0) == -1 &&
                   errno == EINTR) {
            }
        }

        if (tab->command.output_fd >= 0) {
            close(tab->command.output_fd);
        }
    }

    XFreeFont(display, font);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return EXIT_SUCCESS;
}
