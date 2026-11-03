# Terminator – A Custom Terminal Emulator

Terminator is a graphical terminal emulator and command shell written in C for Linux. It uses Xlib for its X11 interface and supports multiple tabs, command execution, job control, persistent history, filename completion, configurable keyboard shortcuts, thread/process execution modes, and a limited in-process script interpreter.

## Features

### Graphical interface and tabs

- X11 window and terminal-style text display.
- Multiple tabs, each with its own terminal buffer, input state, working directory, and command/job state.
- Keyboard-driven command input, scrolling, tab switching, and tab management.
- Text selection and X11 clipboard copy/paste.
- X11 drawing and event handling are performed by the GUI thread/process.

### Shell commands

- External commands are launched using `fork()` and `execvp()`.
- Built-ins include `cd`, `exit`, `history`, `multiWatch`, `jobs`, `fg`, `bg`, and `source`.
- The last command status is available through `$?`.
- Background execution using `&`.
- Multiline input, quoting, and command substitution support.
- Input/output redirection and pipelines.

### Job control and multiWatch

- Ctrl+C interrupts the foreground command without closing Terminator.
- Ctrl+Z suspends the foreground command.
- Background jobs can be listed and managed with `jobs`, `fg`, and `bg`.
- `multiWatch` starts multiple commands and collects their output asynchronously.

### History and filename completion

- Persistent command history is stored in `~/.terminator_history`.
- History capacity is limited to 10,000 entries.
- The `history` command displays up to the most recent 1,000 commands.
- Ctrl+R performs reverse history search.
- Tab performs filename and directory completion, including common-prefix completion and selection when multiple matches exist.

### Configurable keyboard shortcuts

Keyboard bindings can be configured in `~/.terminatorrc`. An example configuration is provided at `config/terminatorrc.example`.

```bash
cp config/terminatorrc.example ~/.terminatorrc
nano ~/.terminatorrc
```

If a configuration is missing, the built-in defaults are used. Invalid or conflicting bindings are rejected and the applicable default/current binding is retained.

### Bash-script subset (Task 11)

Terminator includes a limited script interpreter for the project-required subset of shell scripts. The script is interpreted by Terminator; the `bash script.sh` form is supported as a Terminator command syntax and does not mean that the script is delegated to `/bin/bash`.

Supported constructs include:

- `./script.sh`, `bash script.sh`, and `source script.sh`.
- Comments and multiple commands.
- Variable assignments and `$NAME` expansion.
- Positional parameters `$1` through `$9`, `$#`, `$?`, and `$$`.
- `if`, `elif`, `else`, `fi`, including nested conditional blocks.
- Numeric tests: `-eq`, `-ne`, `-gt`, `-lt`, `-ge`, and `-le`.
- String tests: `=`, `==`, and `!=`.
- File and directory tests: `-f` and `-d`.
- `exit [n]`.
- Pipelines, redirection, and supported command substitution through the existing command-execution facilities.

This is **not a complete Bash implementation**. Loops and functions are outside the project requirements. Complex shell grammar, advanced parameter expansion, and all Bash semantics are not guaranteed. Command substitution has limited support; scripts should use the supported forms.

## Execution modes

### Thread mode (default)

```bash
./terminator
```

Thread mode runs the application in one process. Each tab has a worker thread for command and job servicing. The GUI thread owns the X11 connection and handles drawing and input events. Shared history access is protected by a pthread mutex.

### Process mode

```bash
./terminator useProcess=True
```

The equivalent option is:

```bash
./terminator --process
```

In process mode, the GUI parent communicates with tab child processes using local IPC sockets. The GUI parent owns X11. Shared history is placed in shared memory and synchronized with a process-shared semaphore.

## Requirements

- Linux or a compatible POSIX environment with an X11 display.
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

This builds `./terminator`. The Makefile uses C11, warning flags, optimization, pthread support, and `-lX11`.

To remove the generated executable:

```bash
make clean
```

Equivalent direct compilation:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread \
    src/main.c -o terminator -pthread -lX11
```

## Run

Default thread mode:

```bash
./terminator
```

Process mode:

```bash
./terminator useProcess=True
```

## Quick examples

Run these commands in the Terminator window:

```bash
pwd
echo "Hello, Terminator!"
ls
cd ..
sleep 10 &
jobs
history
```

### Job-control walkthrough

1. Run `sleep 100`.
2. Press Ctrl+Z to suspend it.
3. Run `bg` to resume it in the background.
4. Run `jobs` to list jobs.
5. Run `fg` to bring a job to the foreground.
6. Press Ctrl+C to interrupt the foreground job.

### Script example

Create `demo.sh`:

```bash
#!/bin/bash
NAME="Terminator"
AGE=23

echo "Hello $NAME"

if [ "$AGE" -gt 18 ]; then
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

The `#!/bin/bash` line is a script comment for this interpreter; it does not switch execution to the system Bash interpreter.

## Project layout

```text
terminator/
├── config/
│   └── terminatorrc.example
├── src/
│   └── main.c
├── Makefile
├── README.md
└── DESIGN.md
```

Additional files may be present depending on the checked-out branch or local development setup.

## Known limitations

- Terminator targets Linux/POSIX environments and requires an X11 display.
- It implements a project-focused shell, not all Bash syntax or semantics.
- Script interpretation is limited to the constructs listed above; loops and functions are not implemented.
- Command substitution and quoting have limited shell compatibility.
- Behaviour of external commands depends on the utilities installed on the host system.
- The custom X11 terminal interface is not intended to implement every feature or escape sequence of a full terminal standard.

## Project status
Project Has been completed sucessfully

## Author

Developed as part of the Computing Lab (CS69201) term project.
