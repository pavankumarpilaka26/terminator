# Terminator -- A Custom Terminal Emulator

Terminator is a custom graphical terminal emulator and shell implemented
in **C using X11** for Linux. It provides a graphical terminal interface
with command execution, multiple tabs, shell features, job control, and
two execution modes: thread-based and process-based.

## Features

### Terminal Interface

-   Graphical terminal window built with X11.
-   Multiple terminal tabs.
-   Terminal output display and scrolling.
-   Keyboard input and command execution.
-   Copy and paste support.
-   Text selection and configurable keyboard shortcuts.

### Shell Functionality

-   Execute external Linux commands.
-   Built-in commands:
    -   `cd`
    -   `exit`
    -   `history`
    -   `jobs`
    -   `fg`
    -   `bg`
    -   `source`
    -   `multiWatch`
-   Display the last command's exit status using `$?`.
-   Persistent command history shared across tabs.
-   Reverse incremental history search using `Ctrl+R`.
-   Filename and directory autocomplete using `Tab`.

### Command Execution

-   Process creation and execution using `fork()` and `execvp()`.
-   Input and output redirection.
-   Command pipelines.
-   Background command execution using `&`.
-   Multiline command support.

### Job Control

-   Suspend foreground commands using `Ctrl+Z`.
-   Interrupt foreground commands using `Ctrl+C`.
-   Execute commands in the background.
-   List background jobs using `jobs`.
-   Resume suspended jobs using `bg`.
-   Bring background jobs to the foreground using `fg`.

### Execution Modes

Terminator supports two execution modes:

-   **Thread Mode (default):** Each terminal tab has a worker thread
    that handles command/job servicing.
-   **Process Mode:** Each terminal tab is handled by a separate child
    process. The GUI parent communicates with tab processes using IPC.

Both modes have been tested during project development.

## Technologies Used

-   **Programming Language:** C
-   **GUI Library:** X11
-   **Concurrency:** POSIX threads (`pthread`)
-   **Process and IPC APIs:** `fork()`, `execvp()`, `waitpid()`,
    `pipe()`, `dup2()`, `socketpair()`, `mmap()`, and POSIX semaphores
-   **Compiler:** GCC
-   **Build environment:** Linux (including WSL with a working X11
    display)

## Project Structure

The source file and supporting files may be arranged as follows:

``` text
terminator/
├── src/
│   └── main.c
├── README.md
├── Makefile
├── .gitignore
└── LICENSE
```

Adjust the paths above if your repository uses a different layout.

## Prerequisites

On Ubuntu / Debian, install GCC, Make, and the X11 development
libraries:

``` bash
sudo apt update
sudo apt install build-essential libx11-dev
```

A working X11 display environment is required. When using WSL, configure
and start an X server/display environment supported by your WSL setup.

## Build

Clone the repository:

``` bash
git clone https://github.com/pavankumarpilaka26/terminator.git
cd terminator
```

Compile directly with GCC (from the directory containing `main.c`):

``` bash
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 main.c -o terminator -lX11 -pthread
```

If the project includes a configured `Makefile`, you can instead build
with:

``` bash
make
```

Remove the generated executable with:

``` bash
make clean
```

## Run

### Thread Mode (default)

``` bash
./terminator
```

### Process Mode

``` bash
./terminator useProcess=True
```

The equivalent option is:

``` bash
./terminator --process
```

## Example Commands

Try these commands inside Terminator:

``` bash
pwd
ls
echo "Hello, Terminator!"
cd ..
history
sleep 10 &
jobs
```

### Job Control Example

Start a long-running foreground command:

``` bash
sleep 100
```

-   Press `Ctrl+Z` to suspend it.
-   Run `bg` to resume it in the background.
-   Run `fg` to bring it back to the foreground.
-   Press `Ctrl+C` to interrupt a foreground command.

### History and Autocomplete

-   Press `Ctrl+R` and type text to search previously executed commands.
-   Press `Tab` to complete a filename or directory. If there are
    multiple matches, the terminal can display choices for selection.

## Project Status

**Tasks 1--10 have been implemented and tested** as part of the
Computing Lab project. This includes terminal functionality, execution
modes, command history/search, and filename autocomplete.

## Author

Developed as part of the Computing Lab project.

**Terminator -- A Custom Terminal Emulator**
