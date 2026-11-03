# Terminator – Design Document

## 1. Overview

Terminator is a C-based graphical terminal emulator and project-focused shell. The GUI is built with Xlib. The program supports two execution modes: a default single-process/thread mode and an optional process mode. The implementation is concentrated in `src/main.c`.

This document describes the architecture visible in the implementation. It is not a claim that Terminator is a full POSIX terminal emulator or a complete Bash shell.

## 2. X11 GUI and event loop

The application opens an X11 display and creates its own window. X11 events are consumed by the GUI event loop. Keyboard events are translated into key symbols/characters and update the active tab's input state. The display is redrawn from the tab's stored terminal buffer and current input.

Each tab maintains independent state, including:
- terminal output buffer;
- editable input and cursor position;
- current working directory;
- command and job state;
- scroll position;
- history-navigation and reverse-search state;
- filename-completion state.

The X11 interface is handled by the GUI thread; worker threads do not draw directly into the X11 window. The event loop also handles tab actions, window close events and keyboard shortcuts.

## 3. Tabs and execution modes

### 3.1 Thread mode

The default invocation is:

```bash
./terminator
```

Thread mode uses one application process. A worker thread is associated with each tab for command/job servicing. Tab-specific state is held in the tab structure. Shared history is protected by a `pthread_mutex_t`.

### 3.2 Process mode

Process mode can be started with:

```bash
./terminator useProcess=True
```

The implementation also accepts `--process`. The GUI remains in the parent process and tab work is delegated to child processes. The GUI and tab processes exchange structured messages over IPC file descriptors (socket-pair based communication in the implementation).

The process-mode history is placed in shared memory and protected by a process-shared POSIX semaphore. This allows history updates to be visible across tab processes.

### 3.3 Isolation intent

Each tab maintains its own current working directory and command/job state. A `cd` in one tab should not change another tab's directory. Commands and signals are associated with the relevant tab's process/job state.

## 4. Command execution and process management

External commands are started using `fork()` followed by `execvp()`. The parent tracks child state and collects output through pipes. Child signal dispositions are reset before executing external programs so that commands receive normal interrupt/stop behaviour.

Built-in commands are handled by Terminator because they affect shell state or project features. These include `cd`, `exit`, `history`, `multiWatch`, `jobs`, `fg`, `bg` and `source`.

Background jobs are tracked in per-tab job structures. Foreground and background process state is serviced by the command/job servicing routines.

## 5. IPC, pipes and redirection

### 5.1 Command output

A pipe is created for command output. The child redirects standard output and/or standard error to the pipe using `dup2()`. The parent reads available output and appends it to the tab's terminal buffer.

### 5.2 Input/output redirection

Input redirection opens the requested file and duplicates it onto standard input. Output redirection opens/creates the destination and duplicates it onto standard output. File descriptors are closed after duplication where appropriate.

### 5.3 Pipelines

Pipelines connect the standard output of one process to the standard input of the next using POSIX pipes. Redirection can be used with command execution as supported by the parser.

### 5.4 Process-mode IPC

The process-mode GUI/tab communication uses socket-pair file descriptors and message structures carrying command, state, status, working-directory and output information.

## 6. multiWatch

`multiWatch` accepts a list of commands and starts them independently. Each command has its own process/output-pipe state. The application monitors the read ends using `poll()` rather than continuously spinning on each pipe. Completed commands are scheduled to run again after the configured interval. Ctrl+C stops the multiWatch activity and its children while leaving the terminal application running.

## 7. History

History uses a fixed-size circular buffer:
- capacity: `MAX_HISTORY` (10,000 commands);
- `count` tracks the number of stored entries;
- `next` identifies the next insertion slot.

The history is persisted in `~/.terminator_history`. The `history` built-in displays up to 1,000 recent commands. Ctrl+R invokes reverse search over stored commands.

Synchronization differs by execution mode:
- thread mode: `pthread_mutex_t`;
- process mode: shared memory plus process-shared `sem_t`.

## 8. Filename auto-completion

Each tab stores the completion state and candidate names. Completion scans the tab's current directory, handles directory prefixes, and records matching filenames. A unique match can be completed directly. For multiple matches, the implementation can extend the common prefix and retain a candidate list for selection.

## 9. Keyboard shortcuts and clipboard

The application defines default key bindings for line editing and clipboard operations. User overrides are read from `~/.terminatorrc`; an example configuration is provided in `config/terminatorrc.example`. The GUI handles modifier masks and key symbols to implement cursor movement, history navigation, line deletion, screen clearing, EOF, copy and paste.

Clipboard interaction uses X11 selection/atom mechanisms.

## 10. Signals and job control

The GUI receives keyboard events and translates Ctrl+C/Ctrl+Z into actions on the active tab's foreground process/job. Signals are delivered to child process groups using `kill()` rather than allowing the GUI process itself to be terminated or suspended. Background job commands (`jobs`, `fg`, `bg`) operate on the active tab's tracked jobs.

Child processes reset relevant signal dispositions before `execvp()`.

## 11. Script interpreter (Task 11)

Task 11 adds a bounded interpreter implemented in C. It loads a script into a fixed-size line array, checks conditional-block structure before execution, and then interprets supported statements.

The interpreter maintains a script context containing:
- a bounded variable table;
- script argument pointers and argument count;
- an exit-request flag and exit status.

Variable expansion handles the supported forms `$NAME`, `${NAME}`, `$1`–`$9`, `$#`, `$?` and `$$`. Conditional blocks support nested `if` statements and `elif`/`else` branches. Test operators include numeric comparisons, string equality/inequality, and file/directory existence tests.

Ordinary commands are sent through the existing Terminator command engine, which provides the existing command, pipeline and redirection functionality. The interpreter waits for asynchronous command completion before proceeding. Script `exit` ends script interpretation without closing the interactive Terminator process.

### 11.1 Deliberate scope

The interpreter is a subset implementation, not Bash. Loops and functions are not implemented. Advanced Bash parsing, expansions, quoting and compound-command grammar are not guaranteed. Command substitution is limited; scripts should use the supported project subset.

## 12. Important implementation limits

- Fixed-size tab, buffer, command, history and script-context limits are defined in `src/main.c`.
- The application depends on X11 and POSIX process/thread facilities.
- External utilities are not bundled.
- The script interpreter supports only the subset required for the term project.
- The design aims to keep GUI/X11 work in the GUI thread and command execution in child processes or tab workers.

## 13. Build and execution

Install build dependencies on Ubuntu/Debian:

```bash
sudo apt update
sudo apt install build-essential libx11-dev
```

Build:

```bash
make
```

Run thread mode:

```bash
./terminator
```

Run process mode:

```bash
./terminator useProcess=True
```
