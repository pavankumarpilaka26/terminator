# Terminator – Design Document

## 1. Overview

Terminator is a Linux graphical terminal emulator and project-focused command shell implemented in C using Xlib. It provides a tabbed terminal interface, command execution, pipelines and redirection, job control, persistent command history, filename completion, configurable keyboard shortcuts, and a limited script interpreter.

The application supports two execution modes:

- **Thread mode:** one application process with a worker thread for each tab.
- **Process mode:** a GUI parent process with a separate child process for each tab.

Both modes present the same tab-oriented interface. The GUI side remains responsible for X11 operations.

## 2. High-level architecture

The implementation is organized around the following responsibilities:

1. **GUI and event handling:** creates the X11 window, draws the terminal contents, receives keyboard/mouse events, and manages tab interaction.
2. **Per-tab state:** stores the input line, cursor position, output buffer, working directory, command state, history navigation state, and job information.
3. **Command execution:** parses commands and launches external programs using process creation and `execvp()`.
4. **Pipelines and redirection:** connects command processes with pipes and redirects standard input/output using file descriptors.
5. **Job servicing:** tracks foreground/background commands and handles asynchronous output and process status.
6. **History and completion:** maintains persistent command history and completes filenames/directories.
7. **Script interpreter:** interprets the supported Task 11 script constructs.

## 3. GUI and event loop

### 3.1 X11 ownership

The X11 display connection, window, graphics context, font, drawing, and event processing are owned by the GUI thread in Thread mode or the GUI parent in Process mode. Worker threads and tab child processes do not draw directly to the X11 window.

### 3.2 Event loop

The GUI loop waits for X11 events using `poll()` on the X connection. It processes events such as:

- Expose and window resize.
- Key presses.
- Mouse button presses and releases.
- Pointer movement for text selection.
- Clipboard selection requests and responses.
- Window-manager close requests.

The loop periodically services process-mode tab IPC and redraws the active tab. Waiting on the event connection avoids a continuously spinning busy-wait loop.

### 3.3 Tab state

Each tab maintains independent terminal state, including:

- Output/text buffer and scroll position.
- Input text, input length, and cursor position.
- Current working directory.
- Pending multiline command state.
- Foreground command and background job state.
- History navigation/search state.
- In process mode, the tab's IPC endpoint and child process identifier.

This separation allows a command or working-directory change in one tab to remain independent of another tab.

## 4. Execution modes

### 4.1 Thread mode

Thread mode is the default and is started with:

```bash
./terminator
```

The application runs in one process. Each tab is serviced by a worker thread. The GUI thread handles X11 operations and user input, while tab workers service command output and process/job state.

Shared in-process state is protected with synchronization primitives. History operations use a pthread mutex. Tab state coordination uses the application's state synchronization and worker notification mechanisms.

### 4.2 Process mode

Process mode is started with either:

```bash
./terminator useProcess=True
```

or:

```bash
./terminator --process
```

The GUI parent creates a child process for each tab. A local Unix-domain socket pair provides communication between the parent and each tab child. The parent sends command and control messages; children return output lines, status information, working-directory updates, and exit notifications.

The parent remains the only process that performs X11 operations. The child processes service commands and jobs for their own tabs.

### 4.3 Mode comparison

| Concern | Thread mode | Process mode |
|---|---|---|
| Application structure | One process | GUI parent plus tab child processes |
| Per-tab servicing | Worker thread | Child process |
| GUI/X11 ownership | GUI thread | GUI parent |
| Tab communication | Shared process memory and synchronization | Unix-domain socket IPC |
| History synchronization | pthread mutex | Shared memory and process-shared semaphore |

## 5. Command input and parsing

Keyboard input is maintained in each tab's input buffer. The input cursor supports insertion and deletion within the line. The interface also supports history navigation, reverse search, configurable shortcuts, and filename completion.

The command handling path recognizes built-ins and external commands. External commands are launched with `fork()` followed by `execvp()`. The parent-side servicing logic tracks the launched process and collects its output through pipes.

The shell maintains the last command status so it can be expanded through `$?`.

Multiline input handling supports incomplete commands, continuation prompts, quotes, and command-substitution forms within the implemented subset.

## 6. Pipelines and redirection

### 6.1 Pipelines

A pipeline with N command stages requires N−1 pipes. Each stage is launched as a child process. The standard output of one stage is connected to the standard input of the next stage using `dup2()`.

The implementation supports multiple pipeline stages, subject to its configured stage limit. Redirection can be applied to pipeline stages.

### 6.2 Redirection

The supported redirection forms include:

- `< file` – use a file as standard input.
- `> file` – create or truncate a file for standard output.
- `>> file` – append standard output to a file.

The child process opens the requested file and uses `dup2()` to replace the appropriate standard file descriptor. A missing input file is reported as an error rather than running the command with an unintended input source.

## 7. Process and job control

Each active command is tracked with process information and output state. Foreground and background commands are serviced asynchronously so that the GUI remains responsive.

Supported interactions include:

- Ctrl+C: send an interrupt signal to the foreground job.
- Ctrl+Z: send a stop signal to the foreground job.
- `jobs`: list known background/stopped jobs.
- `bg`: resume a stopped job in the background.
- `fg`: bring a job to the foreground.
- `&`: start a command in the background.

Where process groups are used, signals are directed to the relevant job/process group so that unrelated tabs are not intentionally interrupted.

## 8. multiWatch

`multiWatch` starts multiple commands concurrently and collects their output asynchronously. Each watched command has a pipe for output collection. The event/service logic monitors available output rather than blocking the GUI while waiting for one command.

Commands are rerun after the configured interval while the watch remains active. Ctrl+C stops the active multiWatch children and returns control to the terminal.

## 9. History management

Command history is persistent in `~/.terminator_history`.

The history structure has a maximum capacity of 10,000 entries. The `history` built-in displays up to the most recent 1,000 commands. Ctrl+R provides reverse history search, including exact-match and fallback matching behaviour implemented by the project.

History access is synchronized according to the execution mode:

- Thread mode uses a pthread mutex.
- Process mode uses shared memory and a process-shared semaphore.

This allows tabs to share history while protecting concurrent updates.

## 10. Filename completion

Tab completion examines the filename or directory component immediately before the input cursor, using the tab's current working directory as the base for relative paths.

- A unique match completes the component.
- Multiple matches extend the input to their common prefix where possible.
- If ambiguity remains, the implementation displays the matching choices and supports selecting a listed match.
- If there is no match, the input remains unchanged.

The text after the cursor is preserved when completion updates the input line.

## 11. Configurable keyboard shortcuts

Keyboard bindings are loaded from `~/.terminatorrc`. The repository provides `config/terminatorrc.example` as a starting point.

The configuration supports the project's named editing and clipboard actions. If the configuration file is absent, built-in defaults are used. Invalid or conflicting entries are reported and do not replace the applicable valid binding.

## 12. Script interpreter (Task 11)

Terminator includes a limited in-process interpreter for the required script subset. It does not delegate the script body to `/bin/bash`.

The supported entry forms are:

```bash
./script.sh
bash script.sh
source script.sh
```

The implemented subset includes:

- Comments and sequential commands.
- Variable assignments and `$NAME` expansion.
- Positional parameters `$1` through `$9`, `$#`, `$?`, and `$$`.
- `if`, `elif`, `else`, `fi`, and nested conditional blocks.
- Numeric tests `-eq`, `-ne`, `-gt`, `-lt`, `-ge`, and `-le`.
- String tests `=`, `==`, and `!=`.
- File/directory tests `-f` and `-d`.
- `exit [n]`.
- Integration with supported command execution, pipelines, redirection, and command substitution.

Loops, functions, and complete Bash grammar are outside the project scope. The interpreter is intentionally a subset and should not be described as fully Bash-compatible.

## 13. IPC and synchronization

### 13.1 Process-mode messages

In Process mode, each tab child communicates with the GUI parent over a local Unix-domain socket pair. The message protocol carries command requests, control requests (such as interrupt, stop, and shutdown), output lines, state updates, and exit notifications.

### 13.2 Shared history

Process mode maps a shared history structure into memory shared by the related processes. A process-shared semaphore protects concurrent history access.

### 13.3 Thread-mode coordination

Thread mode uses pthread synchronization for shared state and history access. Worker threads notify/wake the GUI-side coordination path when state changes need attention.

## 14. Build and dependencies

The project is built with GNU Make and GCC. It requires X11 development headers/libraries and POSIX thread support.

```bash
make
./terminator
```

To use Process mode:

```bash
./terminator useProcess=True
```

To remove the generated executable:

```bash
make clean
```

On Ubuntu/Debian, install the build dependencies with:

```bash
sudo apt update
sudo apt install build-essential libx11-dev
```

## 15. Limitations

- The application requires an X11-compatible graphical display.
- The shell implements a project-focused command subset, not every feature of Bash.
- Script loops and functions are not implemented.
- Quoting, command substitution, and shell grammar have limited compatibility.
- The graphical terminal is custom-built with Xlib and is not a complete implementation of every terminal escape sequence.
- External command availability and behaviour depend on the host operating system.

## 16. Project scope

This design document describes the implementation scope for Terminator Tasks 1–11. The `task11-script-interpreter` branch contains the Task 1–11 implementation. The `main` branch remains the earlier Tasks 1–10 version until the final merge is approved.
