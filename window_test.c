
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
#define TOP_MARGIN 30
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

/* Add a line to the terminal buffer. */
static void add_line(TerminalBuffer *buffer, const char *text)
{
    if (buffer->count < MAX_LINES) {
        snprintf(buffer->lines[buffer->count],
                 LINE_SIZE, "%s", text);
        buffer->count++;
    } else {
        memmove(buffer->lines,
                buffer->lines + 1,
                (MAX_LINES - 1) * sizeof(buffer->lines[0]));

        snprintf(buffer->lines[MAX_LINES - 1],
                 LINE_SIZE, "%s", text);
    }
}

/* Split command output into lines while preserving incomplete lines. */
static void consume_output(
    TerminalBuffer *buffer,
    const char *data,
    size_t length,
    char *partial_line,
    size_t *partial_length)
{
    for (size_t i = 0; i < length; i++) {
        char ch = data[i];

        if (ch == '\n') {
            partial_line[*partial_length] = '\0';
            add_line(buffer, partial_line);
            *partial_length = 0;
            partial_line[0] = '\0';
        } else if (ch != '\r') {
            if (*partial_length < LINE_SIZE - 1) {
                partial_line[*partial_length] = ch;
                (*partial_length)++;
            }
        }
    }
}

/*
 * Basic command parser.
 * - Spaces separate arguments outside quotes.
 * - Single and double quotes group text and are removed.
 * - Backslash escapes the next character outside single quotes.
 *
 * This is not a full Bash parser yet.
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

            if (quote == '\0' &&
                (ch == '\'' || ch == '"')) {
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

/* Start an external command without blocking the GUI. */
static int start_command(
    TerminalBuffer *buffer,
    ActiveCommand *state,
    const char *command)
{
    if (state->active) {
        add_line(buffer,
                 "A command is already running. Please wait.");
        return 0;
    }

    char command_copy[INPUT_SIZE];
    snprintf(command_copy, sizeof(command_copy), "%s", command);

    char *argv[MAX_ARGS];
    int argc = parse_command(command_copy, argv, MAX_ARGS);

    if (argc == 0) {
        add_line(buffer, "Empty command.");
        return 0;
    }

    int pipefd[2];

    if (pipe(pipefd) == -1) {
        add_line(buffer, "Error: pipe() failed.");
        return 0;
    }

    pid_t pid = fork();

    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        add_line(buffer, "Error: fork() failed.");
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
                "terminator: %s: command not found or could not execute\n",
                argv[0]);

        _exit(127);
    }

    close(pipefd[1]);

    int flags = fcntl(pipefd[0], F_GETFL, 0);

    if (flags == -1 ||
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) == -1) {
        close(pipefd[0]);
        (void)kill(pid, SIGTERM);

        while (waitpid(pid, NULL, 0) == -1 && errno == EINTR) {
        }

        add_line(buffer,
                 "Error: could not configure nonblocking output.");
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

/* Read available output and check whether the child has exited. */
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
            } else if (errno == EAGAIN ||
                       errno == EWOULDBLOCK) {
                break;
            } else {
                add_line(buffer,
                         "Error: could not read command output.");
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
            add_line(buffer, "Error: waitpid() failed.");
            state->child_reaped = 1;
        }
    }

    if (state->pipe_eof && state->child_reaped) {
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

/* Draw terminal output, then the current prompt immediately after it. */
static void draw_terminal(
    Display *display,
    Window window,
    GC gc,
    int screen,
    int window_width,
    int window_height,
    const TerminalBuffer *buffer,
    const char *input,
    XFontStruct *font)
{
    XSetForeground(display, gc, WhitePixel(display, screen));

    XFillRectangle(display, window, gc,
                   0, 0,
                   (unsigned int)window_width,
                   (unsigned int)window_height);

    XSetForeground(display, gc, BlackPixel(display, screen));

    const char *title = "Terminator - Custom Terminal";

    XDrawString(display, window, gc,
                LEFT_MARGIN, TOP_MARGIN,
                title, (int)strlen(title));

    int output_top = TOP_MARGIN + LINE_HEIGHT;
    int prompt_width = XTextWidth(
        font, PROMPT, (int)strlen(PROMPT));

    int max_output_lines =
        (window_height - output_top - LINE_HEIGHT - 10) / LINE_HEIGHT;

    if (max_output_lines < 0) {
        max_output_lines = 0;
    }

    int start = buffer->count - max_output_lines;

    if (start < 0) {
        start = 0;
    }

    int visible_count = buffer->count - start;
    int y = output_top;

    for (int i = start; i < buffer->count; i++) {
        if (y + LINE_HEIGHT >= window_height) {
            break;
        }

        XDrawString(display, window, gc,
                    LEFT_MARGIN, y,
                    buffer->lines[i],
                    (int)strlen(buffer->lines[i]));

        y += LINE_HEIGHT;
    }

    int prompt_y = output_top + visible_count * LINE_HEIGHT;

    if (prompt_y + LINE_HEIGHT >= window_height) {
        prompt_y = window_height - LINE_HEIGHT - 5;
    }

    XDrawString(display, window, gc,
                LEFT_MARGIN, prompt_y,
                PROMPT, (int)strlen(PROMPT));

    XDrawString(display, window, gc,
                LEFT_MARGIN + prompt_width, prompt_y,
                input, (int)strlen(input));

    XFlush(display);
}

int main(void)
{
    Display *display = XOpenDisplay(NULL);

    if (display == NULL) {
        fprintf(stderr,
                "Cannot open X display. Check your DISPLAY environment.\n");
        return EXIT_FAILURE;
    }

    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);

    Window window = XCreateSimpleWindow(
        display, root,
        100, 100,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        1,
        BlackPixel(display, screen),
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
        .partial_line = "",
        .partial_length = 0
    };

    char input[INPUT_SIZE] = "";
    size_t input_length = 0;

    int window_width = WINDOW_WIDTH;
    int window_height = WINDOW_HEIGHT;
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

        if (command_state.active &&
            command_state.output_fd != -1) {
            fds[1].fd = command_state.output_fd;
            fds[1].events = POLLIN | POLLHUP | POLLERR;
            fds[1].revents = 0;
            nfds = 2;
        }

        int ready = poll(fds, nfds, 50);

        if (ready == -1 && errno != EINTR) {
            fprintf(stderr, "poll() failed: %s\n", strerror(errno));
            break;
        }

        if (command_state.active) {
            service_command(&buffer, &command_state);

            draw_terminal(display, window, gc, screen,
                          window_width, window_height,
                          &buffer, input, font);
        }

        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);

            if (event.type == Expose) {
                if (event.xexpose.count == 0) {
                    draw_terminal(display, window, gc, screen,
                                  window_width, window_height,
                                  &buffer, input, font);
                }
            } else if (event.type == ConfigureNotify) {
                window_width = event.xconfigure.width;
                window_height = event.xconfigure.height;

                draw_terminal(display, window, gc, screen,
                              window_width, window_height,
                              &buffer, input, font);
            } else if (event.type == KeyPress) {
                char key_text[32];
                KeySym key_symbol;

                int length = XLookupString(
                    &event.xkey,
                    key_text,
                    (int)sizeof(key_text),
                    &key_symbol,
                    NULL);

                if (key_symbol == XK_Escape) {
                    running = 0;
                } else if (key_symbol == XK_BackSpace) {
                    if (input_length > 0) {
                        input_length--;
                        input[input_length] = '\0';
                    }
                } else if (key_symbol == XK_Return ||
                           key_symbol == XK_KP_Enter) {
                    char submitted[INPUT_SIZE + 32];

                    snprintf(submitted, sizeof(submitted),
                             "%s%s", PROMPT, input);
                    add_line(&buffer, submitted);

                    if (input_length == 0) {
                        add_line(&buffer, "Empty command.");
                    } else if (command_state.active) {
                        add_line(&buffer,
                                 "A command is already running. Please wait.");
                    } else {
                        (void)start_command(&buffer,
                                            &command_state,
                                            input);
                    }

                    input_length = 0;
                    input[0] = '\0';
                } else {
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
                              window_width, window_height,
                              &buffer, input, font);
            } else if (event.type == ClientMessage) {
                if ((Atom)event.xclient.data.l[0] == wm_delete) {
                    running = 0;
                }
            }
        }
    }

    /* Clean up an active child if the window is closed. */
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
