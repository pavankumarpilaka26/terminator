
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 500
#define INPUT_SIZE 1024

static void draw_terminal(
    Display *display,
    Window window,
    GC gc,
    int screen,
    const char *input,
    const char *last_command)
{
    /* Clear the drawing area using the window background colour. */
    XSetForeground(display, gc, WhitePixel(display, screen));
    XFillRectangle(display, window, gc, 0, 0,
                   WINDOW_WIDTH, WINDOW_HEIGHT);

    /* Set the drawing colour to black. */
    XSetForeground(display, gc, BlackPixel(display, screen));

    XDrawString(display, window, gc, 20, 30,
                "Terminator - Terminal Prototype", 32);

    XDrawString(display, window, gc, 20, 70,
                "user@terminator> ", 17);

    XDrawString(display, window, gc, 190, 70,
                input, (int)strlen(input));

    if (last_command[0] != '\0') {
        XDrawString(display, window, gc, 20, 110,
                    "Last submitted command:", 24);

        XDrawString(display, window, gc, 20, 140,
                    last_command, (int)strlen(last_command));
    }

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

    Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete, 1);

    XSelectInput(display, window, ExposureMask | KeyPressMask);

    GC gc = XCreateGC(display, window, 0, NULL);

    if (gc == NULL) {
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return EXIT_FAILURE;
    }

    char input[INPUT_SIZE] = "";
    char last_command[INPUT_SIZE] = "";

    size_t input_length = 0;
    int running = 1;

    XMapWindow(display, window);

    while (running) {
        XEvent event;
        XNextEvent(display, &event);

        if (event.type == Expose) {
            if (event.xexpose.count == 0) {
                draw_terminal(display, window, gc, screen,
                              input, last_command);
            }
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
                    input[--input_length] = '\0';
                }
            }
            else if (key_symbol == XK_Return ||
                     key_symbol == XK_KP_Enter) {
                input[input_length] = '\0';

                strcpy(last_command, input);

                input_length = 0;
                input[0] = '\0';
            }
            else {
                for (int i = 0; i < length; i++) {
                    unsigned char ch = (unsigned char)key_text[i];

                    if (ch >= 32 && ch <= 126 &&
                        input_length < INPUT_SIZE - 1) {
                        input[input_length++] = (char)ch;
                        input[input_length] = '\0';
                    }
                }
            }

            draw_terminal(display, window, gc, screen,
                          input, last_command);
        }
        else if (event.type == ClientMessage) {
            if ((Atom)event.xclient.data.l[0] == wm_delete) {
                running = 0;
            }
        }
    }

    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return EXIT_SUCCESS;
}
