
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

/* Add command output while preserving line boundaries. */
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
 * Execute a simple external command.
 *
 * This first parser splits arguments on spaces and tabs.
 * It does not yet support quotes, pipes, or redirection.
 */
static void execute_command(
    TerminalBuffer *buffer,
    const char *command)
{
    char command_copy[INPUT_SIZE];

    snprintf(command_copy, sizeof(command_copy),
             "%s", command);

    char *argv[MAX_ARGS];
    int argc = 0;
    char *saveptr = NULL;

    char *token = strtok_r(command_copy, " \t", &saveptr);

    while (token != NULL && argc < MAX_ARGS - 1) {
        argv[argc++] = token;
        token = strtok_r(NULL, " \t", &saveptr);
    }

    argv[argc] = NULL;

    if (argc == 0) {
        add_line(buffer, "Empty command.");
        return;
    }

    int pipefd[2];

    if (pipe(pipefd) == -1) {
        add_line(buffer, "Error: pipe() failed.");
        return;
    }

    pid_t pid = fork();

    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        add_line(buffer, "Error: fork() failed.");
        return;
    }

    if (pid == 0) {
        /* Child process: send stdout and stderr into the pipe. */
        close(pipefd[0]);

        if (dup2(pipefd[1], STDOUT_FILENO) == -1 ||
            dup2(pipefd[1], STDERR_FILENO) == -1) {
            _exit(126);
        }

        close(pipefd[1]);

        execvp(argv[0], argv);

        /* Reached only if execvp() fails. */
        dprintf(STDERR_FILENO,
                "terminator: %s: command not found or could not execute\n",
                argv[0]);

        _exit(127);
    }

    /* Parent process: read the child's output. */
    close(pipefd[1]);

    char data[4096];
    char partial_line[LINE_SIZE] = "";
    size_t partial_length = 0;

    for (;;) {
        ssize_t bytes_read = read(pipefd[0], data, sizeof(data));

        if (bytes_read > 0) {
            consume_output(
                buffer,
                data,
                (size_t)bytes_read,
                partial_line,
                &partial_length);
        } else if (bytes_read == 0) {
            /* EOF: the pipe's write end has been closed. */
            break;
        } else if (errno == EINTR) {
            /* A signal interrupted read(); try again. */
            continue;
        } else {
            add_line(buffer, "Error: could not read command output.");
            break;
        }
    }

    close(pipefd[0]);

    /* Save any output that did not end with a newline. */
    if (partial_length > 0) {
        partial_line[partial_length] = '\0';
        add_line(buffer, partial_line);
    }

    /* Collect the child's exit status. */
    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            add_line(buffer, "Error: waitpid() failed.");
            break;
        }
    }
}

/* Draw the terminal contents and current input. */
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

    const char *title = "Terminator - Terminal Prototype";

    XDrawString(display, window, gc,
                LEFT_MARGIN, TOP_MARGIN,
                title, (int)strlen(title));

    int output_top = TOP_MARGIN + LINE_HEIGHT;

    int available_height =
        window_height - output_top - LINE_HEIGHT;

    int visible_lines = available_height / LINE_HEIGHT;

    if (visible_lines < 0) {
        visible_lines = 0;
    }

    int first_line = buffer->count - visible_lines;

    if (first_line < 0) {
        first_line = 0;
    }

    int y = output_top;

    for (int i = first_line; i < buffer->count; i++) {
        XDrawString(display, window, gc,
                    LEFT_MARGIN, y,
                    buffer->lines[i],
                    (int)strlen(buffer->lines[i]));

        y += LINE_HEIGHT;
    }

    int input_y = y + LINE_HEIGHT / 2;

    int prompt_width = XTextWidth(
        font, PROMPT, (int)strlen(PROMPT));

    XDrawString(display, window, gc,
                LEFT_MARGIN, input_y,
                PROMPT, (int)strlen(PROMPT));

    XDrawString(display, window, gc,
                LEFT_MARGIN + prompt_width, input_y,
                input, (int)strlen(input));

    XFlush(display);
}

int main(void)
{
    Display *display = XOpenDisplay(NULL);

    if (display == NULL) {
        fprintf(stderr, "Cannot open X display\n");
        return EXIT_FAILURE;
    }

    int screen = DefaultScreen(display);

    Window window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        100, 100,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        1,
        BlackPixel(display, screen),
        WhitePixel(display, screen)
    );

    XStoreName(display, window, "Terminator - Terminal Prototype");

    Atom wm_delete = XInternAtom(
        display, "WM_DELETE_WINDOW", False);

    XSetWMProtocols(display, window, &wm_delete, 1);

    XSelectInput(display, window,
                 ExposureMask |
                 KeyPressMask |
                 StructureNotifyMask);

    GC gc = XCreateGC(display, window, 0, NULL);

    if (gc == NULL) {
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return EXIT_FAILURE;
    }

    XFontStruct *font = XLoadQueryFont(display, "fixed");

    if (font == NULL) {
        fprintf(stderr, "Could not load X11 font\n");

        XFreeGC(display, gc);
        XDestroyWindow(display, window);
        XCloseDisplay(display);

        return EXIT_FAILURE;
    }

    XSetFont(display, gc, font->fid);

    TerminalBuffer buffer = {0};

    char input[INPUT_SIZE] = "";
    size_t input_length = 0;

    int window_width = WINDOW_WIDTH;
    int window_height = WINDOW_HEIGHT;
    int running = 1;

    XMapWindow(display, window);

    while (running) {
        XEvent event;
        XNextEvent(display, &event);

        if (event.type == Expose) {
            if (event.xexpose.count == 0) {
                draw_terminal(
                    display, window, gc, screen,
                    window_width, window_height,
                    &buffer, input, font);
            }
        }
        else if (event.type == ConfigureNotify) {
            window_width = event.xconfigure.width;
            window_height = event.xconfigure.height;

            draw_terminal(
                display, window, gc, screen,
                window_width, window_height,
                &buffer, input, font);
        }
        else if (event.type == KeyPress) {
            char key_text[32];
            KeySym key_symbol;

            int length = XLookupString(
                &event.xkey,
                key_text,
                sizeof(key_text),
                &key_symbol,
                NULL
            );

            if (key_symbol == XK_Escape) {
                running = 0;
            }
            else if (key_symbol == XK_BackSpace) {
                if (input_length > 0) {
                    input_length--;
                    input[input_length] = '\0';
                }
            }
            else if (key_symbol == XK_Return ||
                     key_symbol == XK_KP_Enter) {

                char submitted[INPUT_SIZE + 32];

                snprintf(submitted, sizeof(submitted),
                         "%s%s", PROMPT, input);

                add_line(&buffer, submitted);

                if (input_length == 0) {
                    add_line(&buffer, "Empty command.");
                } else {
                    execute_command(&buffer, input);
                }

                input_length = 0;
                input[0] = '\0';
            }
            else {
                for (int i = 0;
                     i < length &&
                     input_length < INPUT_SIZE - 1;
                     i++) {

                    unsigned char ch =
                        (unsigned char)key_text[i];

                    if (ch >= 32 && ch <= 126) {
                        input[input_length++] = (char)ch;
                        input[input_length] = '\0';
                    }
                }
            }

            draw_terminal(
                display, window, gc, screen,
                window_width, window_height,
                &buffer, input, font);
        }
        else if (event.type == ClientMessage) {
            if ((Atom)event.xclient.data.l[0] == wm_delete) {
                running = 0;
            }
        }
    }

    XFreeFont(display, font);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return EXIT_SUCCESS;
}
