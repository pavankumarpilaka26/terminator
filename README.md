# Terminator – A Custom Terminal Emulator

Terminator is a graphical terminal emulator and command shell developed in C for Linux as part of the Computing Lab (CS69201) term project.

It uses **Xlib** for its X11 graphical interface and supports multiple tabs, command execution, job control, persistent command history, filename completion, configurable keyboard shortcuts, thread/process execution modes, and a limited in-process script interpreter.

## Features

### 1. Graphical Interface and Tabs

- X11 window and terminal-style text display.
- Multiple tabs, each with its own terminal buffer, input state, working directory, and command/job state.
- Keyboard-driven command input, scrolling, tab switching, and tab management.
- Text selection and X11 clipboard copy/paste.
- X11 drawing and event handling are performed by the GUI thread/process.

### 2. Shell Commands

- External commands are launched using `fork()` and `execvp()`.
- Built-in commands include `cd`, `exit`, `history`, `multiWatch`, `jobs`, `fg`, `bg`, and `source`.
- The last command status is available through `$?`.
- Background execution using `&`.
- Multiline input, quoting, and command substitution support.
- Input/output redirection and pipelines.

### 3. Job Control and multiWatch

- `Ctrl+C` interrupts the foreground command without closing Terminator.
- `Ctrl+Z` suspends the foreground command.
- Background jobs can be listed and managed using `jobs`, `fg`, and `bg`.
- `multiWatch` starts multiple commands and collects their output asynchronously.

### 4. Command History and Filename Completion

- Persistent command history is stored in `~/.terminator_history`.
- History capacity is limited to 10,000 entries.
- The `history` command displays up to the most recent 1,000 commands.
- `Ctrl+R` performs reverse history search.
- `Tab` performs filename and directory completion, including common-prefix completion and selection when multiple matches exist.

### 5. Configurable Keyboard Shortcuts

Keyboard bindings can be configured in `~/.terminatorrc`.

An example configuration is provided at `config/terminatorrc.example`.

```bash
cp config/terminatorrc.example ~/.terminatorrc
nano ~/.terminatorrc
```

If a configuration is missing, built-in defaults are used. Invalid or conflicting bindings are rejected, and the applicable default/current binding is retained.

### 6. Script Interpreter (Task 11)

Terminator includes a limited in-process script interpreter for the project-required subset of shell scripts.

Supported constructs include:

- `./script.sh`, `bash script.sh`, and `source script.sh`.
- Comments and multiple commands.
- Variable assignments and `$NAME` expansion.
- Positional parameters `$1` through `$9`, `$#`, `$?`, and `$$`.
- `if`, `elif`, `else`, and `fi`, including nested conditional blocks.
- Numeric tests: `-eq`, `-ne`, `-gt`, `-lt`, `-ge`, and `-le`.
- String tests: `=`, `==`, and `!=`.
- File and directory tests: `-f` and `-d`.
- `exit [n]`.
- Pipelines, redirection, and supported command substitution through existing command-execution facilities.

**Note:** This is not a complete Bash implementation. Loops and functions are outside the project requirements. Complex shell grammar, advanced parameter expansion, and all Bash semantics are not guaranteed.

The script is interpreted by Terminator. The `bash script.sh` form is supported as Terminator command syntax and does not delegate the script to `/bin/bash`.

## Execution Modes

### Thread Mode (Default)

Run using:

```bash
./terminator
```

Thread mode runs the application in one process. Each tab has a worker thread for command and job servicing. The GUI thread owns the X11 connection and handles drawing and input events. Shared history access is protected by a pthread mutex.

### Process Mode

Run using:

```bash
./terminator useProcess=True
```

Alternatively:

```bash
./terminator --process
```

In process mode, the GUI parent communicates with tab child processes using local IPC sockets. The GUI parent owns X11. Shared history is placed in shared memory and synchronized with a process-shared semaphore.

## Technologies Used

- **Programming Language:** C
- **Compiler:** GCC
- **Build System:** GNU Make
- **Graphical Interface:** Xlib / X11
- **Concurrency:** POSIX Threads (pthreads)
- **Process Management:** `fork()`, `execvp()`, and related system calls
- **Inter-Process Communication:** Local IPC sockets and shared memory
- **Synchronization:** Pthread mutexes and process-shared semaphores
- **Operating System:** Linux / POSIX-compatible environment

## Requirements

- Linux or a compatible POSIX environment with an X11 display.
- GCC.
- GNU Make.
- X11 development headers and library.
- POSIX threads and semaphores.

### Install Dependencies (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install build-essential libx11-dev
```

A graphical X11 display is required. WSL users need WSLg or a configured X server.

## Getting the Project from GitHub

The repository is hosted on GitHub:

**Repository:** https://github.com/pavankumarpilaka26/terminator

### Option 1: Clone the Repository (First-Time Setup)

Open a terminal and run:

```bash
git clone https://github.com/pavankumarpilaka26/terminator.git
```

Navigate into the project directory:

```bash
cd terminator
```

This downloads the repository, including its source code, documentation, and Makefile.

### Option 2: Pull the Latest Changes (Existing Clone)

If you have already cloned the repository and want to get the latest updates:

```bash
cd terminator
git switch main
git pull origin main
```

This updates your local `main` branch with the latest version from GitHub.

**Note:** If you have local uncommitted changes, commit or safely back them up before pulling.

## Build Instructions

From the repository root, run:

```bash
make
```

This compiles the project and generates the `terminator` executable.

The Makefile uses C11, warning flags, optimization, pthread support, and `-lX11`.

To remove the generated executable:

```bash
make clean
```

### Direct Compilation (Alternative)

You can also compile the source directly using:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread \
    src/main.c -o terminator -pthread -lX11
```

## Running the Project

After successful compilation, run the application in the default Thread Mode:

```bash
./terminator
```

To run in Process Mode:

```bash
./terminator useProcess=True
```

Alternatively:

```bash
./terminator --process
```

## Quick Usage Examples

Run these commands inside the Terminator window:

```bash
pwd
echo "Hello, Terminator!"
ls
cd ..
sleep 10 &
jobs
history
```

### Job-Control Demonstration

1. Run `sleep 100`.
2. Press `Ctrl+Z` to suspend it.
3. Run `bg` to resume it in the background.
4. Run `jobs` to list background jobs.
5. Run `fg` to bring a job to the foreground.
6. Press `Ctrl+C` to interrupt the foreground job.

### Script Interpreter Demonstration

Create a script named `demo.sh`:

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

Run it inside Terminator using one of the following:

```bash
source demo.sh
```

or

```bash
bash demo.sh
```

or

```bash
./demo.sh
```

The `#!/bin/bash` line is treated as a script comment by this interpreter. It does not switch execution to the system Bash interpreter.

## Project Structure

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

## Known Limitations

- Terminator targets Linux/POSIX environments and requires an X11 display.
- It implements a project-focused shell, not all Bash syntax or semantics.
- Script interpretation is limited to the constructs listed above; loops and functions are not implemented.
- Command substitution and quoting have limited shell compatibility.
- Behaviour of external commands depends on the utilities installed on the host system.
- The custom X11 terminal interface does not implement every feature or escape sequence of a full terminal standard.

## Project Status

**Status: Completed**

All 11 project tasks have been implemented and integrated into the `main` branch.

The project has been tested in both Thread Mode and Process Mode, and the final source code has been successfully compiled.

## Author

Developed as part of the **Computing Lab (CS69201) Term Project**.


