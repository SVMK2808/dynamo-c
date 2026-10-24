# POSIX Shell Implementation

A modular, POSIX-compliant interactive shell implemented from scratch in C for Unix-like operating systems. Built as part of Advanced Operating Systems and tailored for deep operating system principles, process lifecycle management, kernel file descriptor manipulation, and virtual filesystem inspection.

---

## Table of Contents
- [Architecture & Directory Structure](#architecture--directory-structure)
- [Compilation & Execution](#compilation--execution)
- [Implemented Features & Specifications](#implemented-features--specifications)
- [Design Decisions & OS Principles](#design-decisions--os-principles)
- [PhD Interview Talking Points](#phd-interview-talking-points)

---

## Architecture & Directory Structure

The project strictly adheres to a modular design separating lexing/parsing, execution/IPC, signal handling, terminal raw mode line editing, and builtin commands:

```
.
├── Makefile                     # Build automation with gcc -Wall -Wextra -g
├── README.md                    # Comprehensive documentation
├── include/                     # Public header files
│   ├── builtin.h                # Builtin dispatcher declarations and dispatch table API
│   ├── executor.h               # Executor interface for pipelines and redirection
│   ├── parser.h                 # Two-phase parser and tokenizer declarations
│   ├── shell.h                  # Core data types (Token, Command, Pipeline) & global state
│   ├── signals.h                # Signal handler prototypes
│   └── terminal.h               # POSIX termios raw mode and interactive line reader API
└── src/                         # Source implementations
    ├── main.c                   # Entry point, REPL, dynamic prompt, semicolon separator
    ├── parser.c                 # Phase 1 (Tokenizer) & Phase 2 (Pipeline AST Builder)
    ├── executor.c               # Process creation (fork/execvp), pipelines, redirections, & jobs
    ├── signals.c                # Signal handlers (SIGINT, SIGTSTP, SIGCHLD)
    ├── terminal.c               # Termios raw mode, Tab autocomplete, and Up-Arrow history
    └── builtins/                # Builtin command modules
        ├── builtin.c            # Builtin dispatch table lookup
        ├── cmd_cd.c             # cd with ~, -, ., .., and argument validation
        ├── cmd_echo.c           # echo command
        ├── cmd_exit.c           # exit shell command
        ├── cmd_history.c        # Persistent 20-slot ring buffer across sessions
        ├── cmd_ls.c             # ls with -a, -l, -la, files, and multi-directory support
        ├── cmd_pinfo.c          # Process status, memory, and path via Linux /proc and macOS APIs
        ├── cmd_pwd.c            # pwd command
        └── cmd_search.c         # Recursive depth-first directory search
```

### Module File Breakdown

| File | Module | Description |
|---|---|---|
| `src/main.c` | **REPL & Shell Core** | Manages the Read-Eval-Print Loop, captures the launch directory (`g_shell_home`), displays the dynamic prompt `<username@hostname:path>`, and splits sequential commands on `;`. |
| `src/parser.c` | **Two-Phase Parser** | Tokenizes input into typed tokens (`TOKEN_WORD`, `TOKEN_PIPE`, `TOKEN_REDIR_IN`, etc.) and builds a `Pipeline` struct of `Command` objects with arguments and I/O targets. |
| `src/executor.c` | **Executor & Process Manager** | Handles process creation via `fork()`, program execution via `execvp()`, arbitrary pipelines with `pipe()` and `dup2()`, I/O redirection, and background execution (`&`). |
| `src/signals.c` | **Signal Handlers** | Installs `sigaction` handlers for `SIGINT` (Ctrl+C), `SIGTSTP` (Ctrl+Z), and asynchronous background zombie process reaping via `SIGCHLD`. |
| `src/terminal.c` | **Terminal Line Editor** | Switches terminal to non-canonical/raw mode via `termios` for single-key interception. Implements Tab autocomplete and Up/Down arrow history browsing. |
| `src/builtins/cmd_cd.c` | **`cd` Builtin** | Changes working directory. Resolves `~` to shell launch directory, `-` to previous directory, `.` to current, and rejects more than one argument. |
| `src/builtins/cmd_ls.c` | **`ls` Builtin** | Implements custom directory traversal (`opendir`/`readdir`) and metadata extraction (`lstat`). Decodes permission bits, hard links, owner/group, 1KB block totals, and timestamps. |
| `src/builtins/cmd_pinfo.c` | **`pinfo` Builtin** | Inspects Process Control Block (PCB) metrics from Linux `/proc/<pid>/stat`, `/proc/<pid>/status`, and `readlink("/proc/<pid>/exe")`. Includes a macOS fallback using `<libproc.h>`. |
| `src/builtins/cmd_search.c` | **`search` Builtin** | Recursively traverses subdirectories using Depth-First Search (DFS) to locate a given file or folder; prints `True` or `False`. |
| `src/builtins/cmd_history.c` | **`history` Builtin** | Maintains an in-memory 20-command rolling buffer with consecutive deduplication, serialized to `.shell_history` for cross-session persistence. |

---

## Compilation & Execution

The project provides a clean Makefile with `-Wall -Wextra -g -I include/`:

```bash
# 1. Compile the shell
make

# 2. Run the interactive shell
./shell

# 3. Clean object files and binaries
make clean
```

---

## Implemented Features & Specifications

### 1. Shell Prompt (`<username@hostname:curr_dir>`)
- Dynamically resolves current user via `getpwuid(getuid())` or `getenv("USER")`.
- Dynamically resolves machine hostname via `gethostname()`.
- The directory where the shell was launched is defined as the home directory (`~`).
- Paths inside the launch directory display as `~` or `~/relative/path`.
- Navigating above/outside the launch directory displays the full absolute path.

### 2. Builtin Commands (`cd`, `pwd`, `echo`, `exit`)
- **`pwd`**: Prints absolute working directory via `getcwd()`.
- **`echo`**: Space-separated argument printing.
- **`cd`**:
  - `cd` or `cd ~`: Returns to the shell launch directory (`g_shell_home`).
  - `cd -`: Navigates to the previous working directory (`g_prev_dir`) and prints it.
  - `cd .` and `cd ..`: Standard current/parent navigation.
  - Multi-argument validation: Prints `Invalid arguments` if more than one argument is supplied.
- **`exit`**: Gracefully terminates the shell session.

### 3. Extended `ls`
- **Flags**: Supports `-a` (all files, including hidden `.` files) and `-l` (long format) in any order and combination (`-a`, `-l`, `-la`, `-al`, `-a -l`).
- **Target Support**: Operates on current directory (`.`), files (`ls Makefile`), subdirectories (`ls src`), multiple targets (`ls src include`), and home (`ls ~`).
- **Long Listing Metadata (`-l`)**:
  - Calculates `total <blocks>` allocated in 1KB blocks (`st_blocks / 2`).
  - 10-character permission string (file type `d`/`l`/`-`, plus user, group, other `rwx` permissions).
  - Hard link count (`st_nlink`).
  - User and group names resolved via `/etc/passwd` (`getpwuid`) and `/etc/group` (`getgrgid`).
  - Byte size (`st_size`).
  - Modification timestamp formatted as `%b %e %H:%M`.
  - Filename (and symlink arrow `name -> target` if a link).

### 4. Foreground & Background Process Management
- **Foreground Execution**: The parent creates a child via `fork()`, invokes `execvp()`, tracks `g_foreground_pid`, and blocks with `waitpid()`.
- **Background Execution (`&`)**: Commands ending with `&` are launched asynchronously. The parent shell prints the child PID immediately and continues taking user commands without blocking.

### 5. Process Information (`pinfo`)
- Syntax: `pinfo` (current shell process) or `pinfo <pid>`.
- Displays:
  ```
  pid -- <PID>
  Process Status -- {R/S/S+/Z/T}
  memory -- <VmSize> {Virtual Memory}
  Executable Path -- ~/shell
  ```
- **Foreground Status (`+`)**: Appends `+` to the status code if `pgrp == tpgid` (the process belongs to the terminal's foreground process group).
- **Executable Path**: Reads `/proc/<pid>/exe` using `readlink()`, replacing `g_shell_home` with `~` if applicable.

### 6. Recursive Search (`search`)
- Recursively traverses all subdirectories starting from `.` to locate a target file or folder.
- Uses `lstat()` to avoid infinite loops from directory symlinks.
- Skips `.` and `..` to prevent infinite recursion.
- Returns `True` or `False`.

### 7. I/O Redirection (`<`, `>`, `>>`)
- `< filename`: Redirects stdin from a file (`open()` with `O_RDONLY`).
- `> filename`: Redirects stdout to a file, overwriting (`open()` with `O_WRONLY | O_CREAT | O_TRUNC, 0644`).
- `>> filename`: Redirects stdout to a file in append mode (`open()` with `O_WRONLY | O_CREAT | O_APPEND, 0644`).
- Supports simultaneous input and output redirection.

### 8. Multi-Stage Command Pipelines (`|`)
- Supports an arbitrary number of piped commands (e.g. `cat file.txt | head -7 | tail -5`).
- Creates $N-1$ pipes via `pipe()`.
- Sets up standard input and output descriptors in child processes using `dup2()`.
- Parent process rigorously closes all unused pipe read/write ends to prevent hangs on EOF.

### 9. Combined Redirection with Pipelines
- Redirection operators inside any pipeline segment are handled cleanly by applying file descriptors locally within each child process.

### 10. Signal Handling
- **`SIGINT` (Ctrl+C)**: Sends `SIGINT` to the running foreground child (`g_foreground_pid`) without killing the parent shell.
- **`SIGTSTP` (Ctrl+Z)**: Sends `SIGTSTP` to suspend the foreground child and push it into the background, returning control to the shell prompt.
- **`SIGCHLD`**: An asynchronous signal handler reaps terminated background child processes using `waitpid(-1, NULL, WNOHANG)` to prevent zombie processes.
- **`Ctrl+D`**: Detects EOF on stdin and exits the shell cleanly.

### 11. Tab Autocomplete
- Pressing `TAB` inspects the current word prefix against all files and subdirectories in `.`.
- **Single match**: Autocompletes the remaining suffix inline (and appends `/` for directories).
- **Multiple matches**: Prints all candidate matches space-separated on a new line and re-renders the current prompt and buffer.

### 12. History & Up-Arrow Navigation
- Stores up to 20 commands in memory, serialized to `.shell_history` in `g_shell_home`.
- **Consecutive Deduplication**: Skips adding commands identical to the most recent command.
- **`history`**: Prints the last 10 commands in chronological order.
- **`history <num>`**: Prints the last `<num>` commands ($\le 20$).
- **Up-Arrow Key**: Traverses backwards through history, interactively rewriting the prompt line with previous commands using `\b \b` erasure.

### 13. Semicolon Command Chaining (`;`)
- Multiple commands or pipelines separated by `;` are split using `strtok_r()` and executed sequentially.

---

## Design Decisions & OS Principles

### 1. Why Builtins Must Run in the Parent Process
Commands like `cd` and `exit` modify the internal state of the calling process (`chdir()` changes the process current working directory; `exit()` terminates the process). If these were executed inside a `fork()`-ed child, the changes would be discarded as soon as the child terminated, leaving the parent shell unchanged.

### 2. Preventing Broken Pipes and EOF Hangs
In multi-stage pipelines (`cmd1 | cmd2 | cmd3`), a reader process only receives EOF when **every** process holding a write descriptor to the pipe has closed it. The parent shell must close both ends of all created pipes after spawning children.

### 3. Asynchronous Zombie Reaping via `SIGCHLD`
When background child processes terminate, the kernel retains their exit status in the process table as **zombies** until the parent calls `wait()` or `waitpid()`. Because signals are not queued in Unix, multiple children exiting simultaneously can collapse into a single `SIGCHLD` delivery. Our handler uses a non-blocking loop:
```c
while (waitpid(-1, NULL, WNOHANG) > 0);
```
This reaps all pending zombie processes without stalling the shell.

### 4. Canonical Mode vs Raw Mode (`termios`)
By default, the terminal driver line discipline operates in canonical mode (`ICANON`), buffering keystrokes until Enter is pressed. To implement Tab autocomplete and Up-Arrow history navigation, the shell switches to raw mode (`raw.c_lflag &= ~(ICANON | ECHO)`). An `atexit(disable_raw_mode)` handler guarantees that terminal settings are safely restored even on unexpected termination.

---

## PhD Interview Talking Points

During OS / Systems screening interviews at institutes like IIT Hyderabad and IIT Madras, the following technical mechanisms are frequently probed:

1. **`fork()` and Copy-on-Write (COW)**:
   - What happens to the page tables across `fork()`? The kernel duplicates the virtual memory mappings, marks pages read-only, and defers physical memory allocation until either process performs a write (triggering a page fault and COW allocation).
2. **`dup2()` Mechanics**:
   - `dup2(oldfd, newfd)` atomically closes `newfd` (if open) and duplicates the file description pointer from index `oldfd` into slot `newfd` in the process's file descriptor table. Both descriptors now share the same file offset and open file status flags in the kernel's open file table.
3. **`lstat()` vs `stat()`**:
   - `stat()` dereferences symbolic links; `lstat()` returns metadata for the symbolic link itself. When scanning directories recursively or implementing `ls -l`, `lstat()` is essential to avoid infinite recursion over circular symlinks and to correctly identify `S_ISLNK`.
4. **Process Groups and `SIGINT`/`SIGTSTP` Delivery**:
   - The terminal driver forwards keyboard interrupt signals (`SIGINT` from Ctrl+C) to the foreground process group (`tpgid`). By managing process groups and restoring default signal actions (`SIG_DFL`) in child processes prior to `execvp()`, external programs handle interrupts naturally without killing the parent shell.
5. **The `/proc` Filesystem (procfs)**:
   - `/proc` is a virtual filesystem generated on-the-fly by the kernel scheduler and memory management subsystem. Reading `/proc/<pid>/stat` extracts Task Control Block (PCB) fields without executing intrusive kernel debugging calls.
