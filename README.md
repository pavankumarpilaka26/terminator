# Terminator – A Custom Terminal Emulator

A custom terminal emulator and shell implemented in **C using X11** for Linux. The project provides a graphical terminal interface with command execution, tab management, shell features, and job control.

## Features

### Terminal Interface
- Graphical terminal window using X11.
- Multiple terminal tabs.
- Terminal output display and scrolling.
- Keyboard input and command execution.
- Copy and paste support.

### Shell Functionality
- Execute external Linux commands.
- Built-in commands:
  - `cd`
  - `exit`
  - `history`
  - `jobs`
  - `fg`
  - `bg`
  - `source`
  - `multiWatch`
- Display command exit status using `$?`.
- Command history management.

### Command Execution
- Process creation and execution using `fork()` and `execvp()`.
- Input and output redirection.
- Command pipelines.
- Support for background command execution using `&`.

### Job Control
- Suspend foreground commands using `Ctrl+Z`.
- Interrupt foreground commands using `Ctrl+C`.
- Execute commands in the background.
- List background jobs using `jobs`.
- Resume suspended jobs using `bg`.
- Bring background jobs to the foreground using `fg`.

## Technologies Used

- **Programming Language:** C
- **GUI Library:** X11
- **System Calls:** `fork()`, `execvp()`, `waitpid()`, `pipe()`, `dup2()`, `kill()`
- **Compiler:** GCC
- **Build Tool:** GNU Make
- **Platform:** Linux

## Project Structure

```text
terminator/
│
├── src/
│   └── main.c
│
├── README.md
├── Makefile
├── .gitignore
└── LICENSE
```

## Prerequisites

Install GCC, Make, and the X11 development libraries.

For Ubuntu / Debian:

```bash
sudo apt update
sudo apt install build-essential libx11-dev
```

A working X11 display environment is also required.

## Compilation

Clone the repository:

```bash
git clone https://github.com/pavankumarpilaka26/terminator
cd terminator
```

Compile the project:

```bash
make
```

Clean the generated executable:

```bash
make clean
```

## Running the Application

After successful compilation:

```bash
./terminator
```

## Example Commands

Try the following commands inside Terminator:

```bash
pwd
ls
echo "Hello, Terminator!"
cd ..
history
sleep 10 &
jobs
```

Foreground job control:

```bash
sleep 100
```

Use `Ctrl+Z` to suspend the command, `bg` to resume it in the background, and `fg` to bring it back to the foreground. Use `Ctrl+C` to interrupt a foreground command.

## Project Status

Tasks 1–8 have been implemented and tested as part of the project.

---

**Terminator – A Custom Terminal Emulator**

Developed as part of the Computing Lab project.