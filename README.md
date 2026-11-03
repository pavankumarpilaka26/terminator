# Terminator – A Custom Terminal Emulator

Terminator is a custom graphical terminal emulator and shell developed in **C using the X11 library** for Linux.

It provides a graphical terminal interface with multiple tabs, command execution, shell features, job control, command history, autocomplete, and two execution modes: thread-based and process-based.

## Features

### 1. Terminal Interface

- Graphical terminal window built using X11.
- Multiple terminal tabs.
- Terminal output display and scrolling.
- Keyboard input and command execution.
- Text selection.
- Copy and paste support.
- Configurable keyboard shortcuts.

### 2. Shell Functionality

- Execution of external Linux commands.
- Built-in commands:
  - `cd`
  - `exit`
  - `history`
  - `jobs`
  - `fg`
  - `bg`
  - `source`
  - `multiWatch`
- Display of the last command's exit status using `$?`.
- Persistent command history shared across tabs.
- Reverse incremental history search using `Ctrl+R`.
- Filename and directory autocomplete using `Tab`.

### 3. Command Execution

- Process creation and execution using `fork()` and `execvp()`.
- Input and output redirection.
- Command pipelines.
- Background command execution using `&`.
- Multiline command support.

### 4. Job Control

- Suspend foreground commands using `Ctrl+Z`.
- Interrupt foreground commands using `Ctrl+C`.
- Execute commands in the background.
- List background jobs using `jobs`.
- Resume suspended jobs using `bg`.
- Bring background jobs to the foreground using `fg`.

### 5. Execution Modes

Terminator supports two execution modes:

**Thread Mode (Default)**

- Each terminal tab has a worker thread for command and job servicing.
- Run this mode using:

```bash
./terminator
```

**Process Mode**

- Each terminal tab is handled by a separate child process.
- The GUI parent communicates with tab processes using inter-process communication (IPC).
- Run this mode using:

```bash
./terminator useProcess=True
```

The following alternative is also supported:

```bash
./terminator --process
```

Both execution modes were tested during project development.

### 6. Configurable Keyboard Shortcuts

Terminator supports configurable keyboard shortcuts through a user configuration file:

```text
~/.terminatorrc
```

An example configuration is provided in:

```text
config/terminatorrc.example
```

To create your configuration file, run:

```bash
cp config/terminatorrc.example ~/.terminatorrc
```

You can edit the configuration using:

```bash
nano ~/.terminatorrc
```

Example configuration:

```ini
ctrl_a = Alt+A
ctrl_e = Alt+E
ctrl_u = Ctrl+U
ctrl_k = Ctrl+K
ctrl_w = Ctrl+W
ctrl_l = Ctrl+L
ctrl_d = Ctrl+D
copy = Ctrl+Shift+C
paste = Ctrl+Shift+V
```

The configuration file allows users to customize supported keyboard shortcuts.

## Technologies Used

- **Programming Language:** C
- **GUI Library:** X11
- **Concurrency:** POSIX threads (`pthread`)
- **Process Management:** `fork()`, `execvp()`, `waitpid()`
- **IPC and Redirection:** `pipe()`, `dup2()`, `socketpair()`, `mmap()`
- **Synchronization:** POSIX semaphores
- **Compiler:** GCC
- **Build System:** GNU Make

## Project Structure

```text
terminator/
├── config/
│   └── terminatorrc.example
├── src/
│   └── main.c
├── Makefile
├── README.md
├── .gitignore
└── LICENSE
```

## Prerequisites

The following are required:

- Linux operating system or a compatible Linux environment.
- GCC compiler.
- GNU Make.
- X11 development libraries.
- A working graphical X11 display environment.

On Ubuntu or Debian, install the required packages:

```bash
sudo apt update
sudo apt install build-essential libx11-dev
```

When using WSL, a working graphical display environment such as WSLg or a supported X server is required.

## Installation

Clone the repository:

```bash
git clone https://github.com/pavankumarpilaka26/terminator.git
```

Move into the project directory:

```bash
cd terminator
```

Create the user configuration file:

```bash
cp config/terminatorrc.example ~/.terminatorrc
```

## Build Instructions

Build the project using Make:

```bash
make
```

This generates the executable:

```text
terminator
```

Alternatively, compile directly using GCC:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread src/main.c -o terminator -pthread -lX11
```

To remove the generated executable:

```bash
make clean
```

## Running Terminator

### Thread Mode (Default)

```bash
./terminator
```

### Process Mode

```bash
./terminator useProcess=True
```

Or:

```bash
./terminator --process
```

## Example Commands

The following commands can be tried inside Terminator:

```bash
pwd
ls
echo "Hello, Terminator!"
cd ..
history
sleep 10 &
jobs
```

### Job Control Example

Start a foreground process:

```bash
sleep 100
```

Then try:

1. Press `Ctrl+Z` to suspend the process.
2. Execute `bg` to resume it in the background.
3. Execute `jobs` to list background jobs.
4. Execute `fg` to bring a job back to the foreground.
5. Press `Ctrl+C` to interrupt a foreground process.

### History and Autocomplete

- Press `Ctrl+R` to search previously executed commands.
- Type a search string to find matching history entries.
- Press `Tab` to complete filenames and directories.
- When multiple matches are available, the terminal can display choices for selection.

## Project Status

Tasks 1–10 have been implemented and tested as part of the Computing Lab project.

The implementation includes the graphical terminal interface, multiple tabs, shell functionality, command execution, job control, thread and process execution modes, command history, autocomplete, and configurable keyboard shortcuts.

## Limitations

- Terminator requires a working X11 graphical display.
- It is designed for Linux environments and depends on Linux/Unix command-line utilities.
- Available commands depend on the commands and utilities installed on the host system.

## Author

Developed as part of the Computing Lab project.

**Terminator – A Custom Terminal Emulator**