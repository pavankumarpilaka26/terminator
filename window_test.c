
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

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 500
#define INPUT_SIZE 1024
#define MAX_LINES 1000
#define LINE_SIZE 1024
#define MAX_ARGS 128
#define LEFT_MARGIN 20
#define TOP_MARGIN 25
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

/* Add a complete line to the terminal history. */
static void add_line(TerminalBuffer *buffer, const char *text)
{
    if (buffer->count == MAX_LINES) {
        memmove(buffer->lines,
                buffer->lines + 1,
                (MAX_LINES - 1) * sizeof(buffer->lines[0]));
        buffer->count--;
    }

    snprintf(buffer->lines[buffer->count],
             LINE_SIZE, "%s", text);
    buffer->count++;
}

/* Append output to the history, separating it at newline characters. */
static void consume_output(
    TerminalBuffer *buffer,
    const char *data,
    size_t length,
    char *partial,
    size_t *partial_length)
{
    for (size_t i = 0; i < length; i++) {
        char ch = data[i];

        if (ch == '\n') {
            partial[*partial_length] = '\0';
            add_line(buffer, partial);
            *partial_length = 0;
            partial[0] = '\0';
        } else if (ch != '\r') {
            if (*partial_length < LINE_SIZE - 1) {
                partial[(*partial_length)++] = ch;
            }
        }
    }
}

/*
 * Basic command parser:
 * - Splits arguments at whitespace outside quotes.
 * - Removes matching single and double quotes.
 * - Handles a backslash escape outside single quotes.
 *
 * This is not a complete Bash parser.
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

/*
 * Execute cd in the parent process.
 * Return 1 when the command is handled, otherwise return 0.
 */
static int handle_cd(TerminalBuffer *buffer, const char *command)
{
    char copy[INPUT_SIZE];
    snprintf(copy, sizeof(copy), "%s", command);

    char *argv[MAX_ARGS];
    int argc = parse_command(copy, argv, MAX_ARGS);

    if (argc == 0 || strcmp(argv[0], "cd") != 0) {
        return 0;
    }

    if (argc > 2) {
        add_line(buffer, "cd: too many arguments");
        return 1;
    }

    const char *path;

    if (argc == 1) {
        path = getenv("HOME");

        if (path == NULL || path[0] == '\0') {
            add_line(buffer, "cd: HOME is not set");
            return 1;
        }
    } else {
        path = argv[1];
    }

    if (chdir(path) == -1) {
        char message[LINE_SIZE];

        snprintf(message, sizeof(message),
                 "cd: %s: %s", path, strerror(errno));
        add_line(buffer, message);
    }

    return 1;
}

/* Start an external command in a child process. */
static int start_command(
    TerminalBuffer *buffer,
    ActiveCommand *state,
    const char *command)
{
    if (state->active) {
        add_line(buffer, "A command is already running. Please wait.");
        return 0;
    }

    char copy[INPUT_SIZE];
    snprintf(copy, sizeof(copy), "%s", command);

    char *argv[MAX_ARGS];
    int argc = parse_command(copy, argv, MAX_ARGS);

    if (argc == 0) {
        return 0;
    }

    int pipefd[2];

    if (pipe(pipefd) == -1) {
        add_line(buffer, "Error: pipe() failed");
        return 0;
    }

    pid_t pid = fork();

    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        add_line(buffer, "Error: fork() failed");
        return 0;
    }

    if (pid == 0) {
        close(pipefd[0]);

        if (dup2(pipefd[1], STDOUT_FILENO) == -1 ||
            dup2(pipefd[1], STDERR_FILENO) == -1) {
            _exit(126);
        }

        close(pipefd[1]);

        execvp(argv[0], argv);

        dprintf(STDERR_FILENO,
                "terminator: %s: %s\n",
                argv[0], strerror(errno));
        _exit(127);
    }

    close(pipefd[1]);

    int flags = fcntl(pipefd[0], F_GETFL, 0);

    if (flags == -1 ||
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) == -1) {
        close(pipefd[0]);
        kill(pid, SIGTERM);

        while (waitpid(pid, NULL, 0) == -1 && errno == EINTR) {
        }

        add_line(buffer, "Error: could not configure output pipe");
        return 0;
    }

    state->pid = pid;
    state->output_fd = pipefd[0];
    state->active = 1;
    state->pipe_eof = 0;
    state->child_reaped = 0;
    state->partial_length = 0;
    state->partial_line[0] = '\0';

    return 1;
}

/* Read command output without blocking the GUI. */
static void service_command(
    TerminalBuffer *buffer,
    ActiveCommand *state)
{
    if (!state->active) {
        return;
    }

    if (!state->pipe_eof && state->output_fd != -1) {
        char data[4096];

        for (;;) {
            ssize_t n = read(state->output_fd, data, sizeof(data));

            if (n > 0) {
                consume_output(buffer, data, (size_t)n,
                               state->partial_line,
                               &state->partial_length);
            } else if (n == 0) {
                state->pipe_eof = 1;
                close(state->output_fd);
                state->output_fd = -1;
                break;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                add_line(buffer, "Error: reading command output failed");
                state->pipe_eof = 1;
                close(state->output_fd);
                state->output_fd = -1;
                break;
            }
        }
    }

    if (!state->child_reaped) {
        int status;
        pid_t result = waitpid(state->pid, &status, WNOHANG);

        if (result == state->pid) {
            state->child_reaped = 1;
        } else if (result == -1 && errno != EINTR) {
            add_line(buffer, "Error: waitpid() failed");
            state->child_reaped = 1;
        }
    }

    if (state->pipe_eof && state->child_reaped) {
        /* Flush the final output line even if it has no trailing newline. */
        if (state->partial_length > 0) {
            state->partial_line[state->partial_length] = '\0';
            add_line(buffer, state->partial_line);
            state->partial_length = 0;
            state->partial_line[0] = '\0';
        }

        state->active = 0;
        state->pid = -1;
    }
}

/*
 * Draw history in chronological order.
 * If a command is running, its output is shown as it arrives.
 * If no command is running, draw the current prompt after history.
 */
static void draw_terminal(
    Display *display,
    Window window,
    GC gc,
    int screen,
    int width,
    int height,
    const TerminalBuffer *buffer,
    const char *input,
    int command_active,
    XFontStruct *font)
{
    XSetForeground(display, gc, WhitePixel(display, screen));
    XFillRectangle(display, window, gc, 0, 0,
                   (unsigned int)width,
                   (unsigned int)height);

    XSetForeground(display, gc, BlackPixel(display, screen));

    int visible_rows = (height - TOP_MARGIN - 10) / LINE_HEIGHT;

    if (visible_rows < 1) {
        visible_rows = 1;
    }

    /*
     * Reserve one row for the active prompt.
     * While a command is running, history can use all visible rows.
     */
    int history_rows = visible_rows;

    if (!command_active) {
        history_rows = visible_rows - 1;

        if (history_rows < 0) {
            history_rows = 0;
        }
    }

    int start = buffer->count - history_rows;

    if (start < 0) {
        start = 0;
    }

    int y = TOP_MARGIN + LINE_HEIGHT;

    for (int i = start; i < buffer->count; i++) {
        if (y > height - 5) {
            break;
        }

        XDrawString(display, window, gc,
                    LEFT_MARGIN, y,
                    buffer->lines[i],
                    (int)strlen(buffer->lines[i]));

        y += LINE_HEIGHT;
    }

    /*
     * Place the current prompt immediately after the last history line.
     * Do not draw a prompt while an external command is still running.
     */
    if (!command_active) {
        if (y > height - 5) {
            y = height - 5;
        }

        XDrawString(display, window, gc,
                    LEFT_MARGIN, y,
                    PROMPT, (int)strlen(PROMPT));

        int prompt_width = XTextWidth(
            font, PROMPT, (int)strlen(PROMPT));

        int input_x = LEFT_MARGIN + prompt_width;
        int available_width = width - input_x - LEFT_MARGIN;

        if (available_width > 0) {
            int input_length = (int)strlen(input);
            int first = 0;

            while (first < input_length &&
                   XTextWidth(font, input + first,
                              input_length - first) > available_width) {
                first++;
            }

            XDrawString(display, window, gc,
                        input_x, y,
                        input + first, input_length - first);
        }
    }

    XFlush(display);
}

int main(void)
{
    Display *display = XOpenDisplay(NULL);

    if (display == NULL) {
        fprintf(stderr,
                "Cannot open X display. Check your DISPLAY setting.\n");
        return EXIT_FAILURE;
    }

    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);

    Window window = XCreateSimpleWindow(
        display, root, 100, 100,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        1, BlackPixel(display, screen),
        WhitePixel(display, screen));

    XStoreName(display, window, "Terminator - Custom Terminal");

    Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete, 1);

    XSelectInput(display, window,
                 ExposureMask | KeyPressMask | StructureNotifyMask);

    GC gc = XCreateGC(display, window, 0, NULL);

    if (gc == NULL) {
        fprintf(stderr, "Could not create graphics context.\n");
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return EXIT_FAILURE;
    }

    XFontStruct *font = XLoadQueryFont(display, "fixed");

    if (font == NULL) {
        fprintf(stderr, "Could not load X11 font.\n");
        XFreeGC(display, gc);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return EXIT_FAILURE;
    }

    XSetFont(display, gc, font->fid);

    TerminalBuffer buffer = {0};

    ActiveCommand command_state = {
        .pid = -1,
        .output_fd = -1,
        .active = 0,
        .pipe_eof = 0,
        .child_reaped = 0,
        .partial_length = 0
    };

    char input[INPUT_SIZE] = "";
    size_t input_length = 0;

    int width = WINDOW_WIDTH;
    int height = WINDOW_HEIGHT;
    int running = 1;

    XMapWindow(display, window);
    XFlush(display);

    int x_fd = ConnectionNumber(display);

    while (running) {
        struct pollfd fds[2];
        nfds_t nfds = 1;

        fds[0].fd = x_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        if (command_state.active && command_state.output_fd != -1) {
            fds[1].fd = command_state.output_fd;
            fds[1].events = POLLIN | POLLHUP | POLLERR;
            fds[1].revents = 0;
            nfds = 2;
        }

        int result = poll(fds, nfds, 50);

        if (result == -1 && errno != EINTR) {
            fprintf(stderr, "poll() failed: %s\n", strerror(errno));
            break;
        }

        if (command_state.active) {
            service_command(&buffer, &command_state);

            draw_terminal(display, window, gc, screen,
                          width, height, &buffer, input,
                          command_state.active, font);
        }

        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);

            if (event.type == Expose) {
                if (event.xexpose.count == 0) {
                    draw_terminal(display, window, gc, screen,
                                  width, height, &buffer, input,
                                  command_state.active, font);
                }
            } else if (event.type == ConfigureNotify) {
                width = event.xconfigure.width;
                height = event.xconfigure.height;

                draw_terminal(display, window, gc, screen,
                              width, height, &buffer, input,
                              command_state.active, font);
            } else if (event.type == KeyPress) {
                char key_text[32];
                KeySym key_symbol;

                int length = XLookupString(
                    &event.xkey, key_text,
                    (int)sizeof(key_text),
                    &key_symbol, NULL);

                if (key_symbol == XK_Escape) {
                    running = 0;
                } else if (key_symbol == XK_BackSpace) {
                    if (input_length > 0 && !command_state.active) {
                        input[--input_length] = '\0';
                    }
                } else if (key_symbol == XK_Return ||
                           key_symbol == XK_KP_Enter) {
                    if (command_state.active) {
                        /* Do not submit another command while one is running. */
                        add_line(&buffer,
                                 "A command is already running. Please wait.");
                    } else {
                        char submitted[INPUT_SIZE + 32];

                        snprintf(submitted, sizeof(submitted),
                                 "%s%s", PROMPT, input);

                        /* Record the command before executing it. */
                        add_line(&buffer, submitted);

                        if (input_length > 0) {
                            if (!handle_cd(&buffer, input)) {
                                (void)start_command(
                                    &buffer, &command_state, input);
                            }
                        }

                        input_length = 0;
                        input[0] = '\0';
                    }
                } else if (!command_state.active) {
                    for (int i = 0;
                         i < length && input_length < INPUT_SIZE - 1;
                         i++) {
                        unsigned char ch = (unsigned char)key_text[i];

                        if (ch >= 32 && ch <= 126) {
                            input[input_length++] = (char)ch;
                            input[input_length] = '\0';
                        }
                    }
                }

                draw_terminal(display, window, gc, screen,
                              width, height, &buffer, input,
                              command_state.active, font);
            } else if (event.type == ClientMessage) {
                if ((Atom)event.xclient.data.l[0] == wm_delete) {
                    running = 0;
                }
            }
        }
    }

    if (command_state.active) {
        (void)kill(command_state.pid, SIGTERM);

        if (command_state.output_fd != -1) {
            close(command_state.output_fd);
            command_state.output_fd = -1;
        }

        while (waitpid(command_state.pid, NULL, 0) == -1 &&
               errno == EINTR) {
        }
    }

    XFreeFont(display, font);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return EXIT_SUCCESS;
}
