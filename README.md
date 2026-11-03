# Terminator – A Custom Terminal Emulator

Terminator is a graphical terminal emulator and command shell written in C for Linux. It uses Xlib for its X11 interface and supports multiple tabs, command execution, job control, history, filename completion, configurable keyboard shortcuts, thread/process execution modes, and a limited in-process script interpreter.

## Features

### Graphical interface and tabs
- X11 window and terminal-style text display.
- Multiple tabs, with a separate terminal buffer, input state, working directory and command state per tab.
- Keyboard-driven command input, scrolling, tab switching and tab management.
- Text selection and X11 clipboard copy/paste.

### Shell commands
- External commands launched with `fork()` and `execvp()`.
- Built-ins include `cd`, `exit`, `history`, `multiWatch`, `jobs`, `fg`, `bg` and `source`.
- Last command status is available as `$?`.
- Background execution with `&`.
- Multiline input and quoting support.
- Input/output redirection and pipelines.

### Job control and multiWatch
- Foreground command interruption with Ctrl+C.
- Foreground command suspension with Ctrl+Z.
- Background job listing and management with `jobs`, `fg` and `bg`.
- `multiWatch` starts multiple commands and collects their output asynchronously.

### History and completion
- Persistent command history in `~/.terminator_history`.
- Up to 10,000 history entries are retained by the in-memory history structure.
- `history` displays up to the most recent 1,000 commands.
- Ctrl+R performs reverse history search.
- Tab performs filename/directory completion, including handling ambiguous matches.

### Configurable keyboard shortcuts
Bindings can be configured in `~/.terminatorrc`. An example is supplied at `config/terminatorrc.example`.

```bash
cp config/terminatorrc.example ~/.terminatorrc
nano ~/.terminatorrc
```

### Bash-script subset (Task 11)
Terminator includes a small script interpreter for the required subset of shell scripts. It supports:
- `source script.sh`, `bash script.sh` and direct `./script.sh` invocation.
- Comments and multiple commands.
- Script-local variable assignments and `$NAME` expansion.
- Positional parameters `$1` through `$9`, `$#`, `$?` and `$$`.
- `if`, `elif`, `else`, `fi` and nested conditional blocks.
- Test expressions including numeric comparisons (`-eq`, `-ne`, `-gt`, `-lt`, `-ge`, `-le`), string comparisons (`=`, `==`, `!=`), and file/directory tests (`-f`, `-d`).
- `exit [n]`.
- Commands can use the terminal's existing command execution support for pipelines and redirection.

This is **not a complete Bash implementation**. Bash loops and functions are outside the project requirements. Complex shell grammar and advanced parameter expansion are not guaranteed. Command substitution has limited support and should be tested against the exact forms used by a script.

## Execution modes

### Thread mode (default)

```bash
./terminator
```

The application uses one process. Tabs have worker threads for command/job servicing. X11 drawing and event handling remain in the GUI thread.

### Process mode

```bash
./terminator useProcess=True
```

The GUI parent communicates with tab child processes using IPC. The implementation also accepts:

```bash
./terminator --process
```

History synchronization uses a process-shared semaphore with shared memory in process mode. In thread mode, history access is protected by a pthread mutex.

## Requirements

- Linux (or a compatible environment with an X11 display).
- GCC.
- GNU Make.
- X11 development headers and library.
- POSIX threads and semaphores.

On Ubuntu/Debian:

```bash
sudo apt update
sudo apt install build-essential libx11-dev
```

A graphical X11 display is required. WSL users need WSLg or a configured X server.

## Build

From the repository root:

```bash
make
```

This builds `./terminator`. The Makefile uses C11, warning flags, optimization, pthread support and `-lX11`.

To clean the generated executable:

```bash
make clean
```

Equivalent direct compilation:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread \
    src/main.c -o terminator -pthread -lX11
```

## Run

```bash
./terminator
```

or:

```bash
./terminator useProcess=True
```

## Quick examples

```bash
pwd
echo "Hello, Terminator!"
ls
cd ..
sleep 10 &
jobs
history
```

Job-control walkthrough:
1. Run `sleep 100`.
2. Press Ctrl+Z.
3. Run `bg` to resume it in the background.
4. Run `jobs`.
5. Run `fg` to bring it to the foreground.
6. Press Ctrl+C to interrupt it.

Script example (`demo.sh`):

```bash
#!/bin/bash
NAME="Terminator"
AGE=23
echo "Hello $NAME"
if [ "$AGE" -gt 18 ]
then
    echo "Adult"
else
    echo "Minor"
fi
```

Run it inside Terminator using one of:

```bash
source demo.sh
bash demo.sh
./demo.sh
```

## Project layout

```text
terminator/
├── config/
│   └── terminatorrc.example
├── src/
│   └── main.c
├── terminator_test_tasks/
├── Makefile
├── README.md
└── DESIGN.md
```

## Known limitations

- Terminator targets Linux/POSIX environments and requires an X11 display.
- It implements a project-focused shell, not all Bash syntax or semantics.
- Script interpretation is intentionally limited to the constructs listed above; loops and functions are not implemented.
- Behaviour of external commands depends on the utilities installed on the host system.
- Terminal emulation is a custom X11 interface and is not intended to implement every feature or escape sequence of a full terminal standard.

## Project status

The `task11-script-interpreter` branch contains the implementation for Tasks 1–11. The `main` branch is intentionally retained as the earlier Tasks 1–10 version until final regression testing and merge approval.

## Author

Developed as part of the Computing Lab (CS69201) term project.
