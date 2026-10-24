#ifndef SHELL_H
#define SHELL_H

/*
 * shell.h — The single shared header for the entire project.
 *
 * Every .c file includes this. It defines the core data types, constants,
 * and global variable declarations that cross module boundaries.
 *
 * WHY A SINGLE SHARED HEADER?
 * ────────────────────────────
 * The parser, executor, builtins, and signal handlers all speak the same
 * language (Token, Command, Pipeline). Rather than re-defining structs in
 * every file or having circular includes, one central header is cleaner.
 * This is a common pattern in C systems programming (e.g., Linux kernel's
 * types.h, libc's unistd.h).
 *
 * INCLUDE GUARD (#ifndef SHELL_H / #define SHELL_H / #endif)
 * ────────────────────────────────────────────────────────────
 * Prevents the header from being processed more than once if multiple .c
 * files include it. Without this, you'd get "duplicate definition" errors
 * at link time. This is called an include guard.
 */

/* ── Standard library includes ────────────────────────────────────────────── */

#include <stdio.h>    /* printf, fprintf, perror, fgets, FILE */
#include <stdlib.h>   /* malloc, free, exit, realloc */
#include <string.h>   /* strcpy, strcmp, strdup, strlen, strtok */
#include <unistd.h>   /* fork, execvp, pipe, dup2, chdir, getcwd, getuid
                         read, write, close — POSIX syscall wrappers */
#include <sys/types.h>/* pid_t, mode_t, off_t — POSIX type definitions */
#include <sys/wait.h> /* waitpid, WIFEXITED, WEXITSTATUS, WNOHANG */
#include <sys/stat.h> /* stat(), struct stat — file metadata */
#include <dirent.h>   /* opendir, readdir, closedir, struct dirent */
#include <fcntl.h>    /* open(), O_RDONLY, O_WRONLY, O_CREAT, O_TRUNC,
                         O_APPEND — file descriptor flags */
#include <signal.h>   /* sigaction, sigemptyset, kill, SIGINT, SIGCHLD,
                         SIGTSTP — signal handling */
#include <errno.h>    /* errno global, strerror() — error reporting */
#include <limits.h>   /* PATH_MAX — maximum length of a file path */
#include <pwd.h>      /* getpwuid, struct passwd — for reading $HOME */

/* ── Constants ────────────────────────────────────────────────────────────── */

/*
 * SHELL_MAX_INPUT: maximum number of characters we'll read per input line.
 * 4096 bytes = 4 KB. This is also the typical pipe buffer size on Linux.
 * If a line exceeds this, fgets() will silently truncate it.
 *
 * NOTE: We prefix with SHELL_ because macOS's <sys/syslimits.h> already
 * defines MAX_INPUT as 1024 (terminal input limit). Name collisions like
 * this are a real hazard in C — always namespace your macros.
 */
#define SHELL_MAX_INPUT    4096

/*
 * MAX_ARGS: maximum arguments in a single command.
 * argv is a NULL-terminated array, so the actual usable slots = MAX_ARGS - 1.
 * 256 is generous; real shells have no fixed limit (they grow dynamically).
 */
#define MAX_ARGS     256

/*
 * MAX_PIPELINE: maximum number of commands in a single pipeline.
 * e.g., cmd1 | cmd2 | ... | cmd32 — we allow up to 32 stages.
 */
#define MAX_PIPELINE 32

/* ── TokenType Enum ───────────────────────────────────────────────────────── */

/*
 * TokenType — Tags each token with what kind of thing it is.
 *
 * WHY AN ENUM AND NOT PLAIN INTS?
 * ─────────────────────────────────
 * Enums give names to integer values. TOKEN_PIPE reads better than '3'.
 * The compiler also catches typos (TOKEN_PPIPE would be an error).
 * An enum is just an int under the hood — no runtime overhead.
 *
 * These are all the syntactically significant tokens in our shell grammar:
 */
typedef enum {
    TOKEN_WORD,          /* a normal word: command name or argument, e.g. "ls", "-la" */
    TOKEN_PIPE,          /* '|' — connects stdout of one command to stdin of next     */
    TOKEN_REDIR_IN,      /* '<' — redirect stdin from a file                           */
    TOKEN_REDIR_OUT,     /* '>' — redirect stdout to a file (overwrite)               */
    TOKEN_REDIR_APPEND,  /* '>>' — redirect stdout to a file (append mode)            */
    TOKEN_BACKGROUND,    /* '&' — run the whole pipeline in the background             */
    TOKEN_EOF            /* sentinel: marks the end of the token stream               */
} TokenType;

/* ── Token struct ─────────────────────────────────────────────────────────── */

/*
 * Token — The smallest unit of parsed input.
 *
 * When the user types:   ls -la | grep foo > out.txt &
 * The tokenizer produces these tokens:
 *   { "ls",     TOKEN_WORD        }
 *   { "-la",    TOKEN_WORD        }
 *   { "|",      TOKEN_PIPE        }    ← value stored but rarely used
 *   { "grep",   TOKEN_WORD        }
 *   { "foo",    TOKEN_WORD        }
 *   { ">",      TOKEN_REDIR_OUT   }    ← value stored but rarely used
 *   { "out.txt",TOKEN_WORD        }
 *   { "&",      TOKEN_BACKGROUND  }    ← value stored but rarely used
 *   { NULL,     TOKEN_EOF         }    ← sentinel
 *
 * MEMORY NOTE:
 *   The 'value' field must be heap-allocated (use strdup), because the
 *   original input buffer will be freed or reused. Each token owns its
 *   string. free_pipeline() is responsible for freeing them.
 *
 * FIELDS — already provided (parse_line() needs to access them):
 *   value  — heap-allocated string: the token's text, e.g. "ls", ">", "foo"
 *   type   — what kind of token this is (one of the TokenType values above)
 */
typedef struct {
    char      *value;   /* heap-allocated text content of the token            */
    TokenType  type;    /* one of the TOKEN_* values from the enum above        */
} Token;

/* ── Command struct ───────────────────────────────────────────────────────── */

/*
 * Command — One fully-parsed command with all its metadata.
 *
 * A Command maps to one segment of a pipeline. For:
 *   grep -i "hello" < input.txt > output.txt
 *
 * The Command struct would hold:
 *   argv        = { "grep", "-i", "hello", NULL }
 *   argc        = 3
 *   input_file  = "input.txt"
 *   output_file = "output.txt"
 *   append      = 0  (overwrite mode, not >>)
 *   background  = 0  (foreground)
 *
 * WHY NULL-TERMINATE argv?
 *   execvp(argv[0], argv) requires a NULL sentinel at the end of the array.
 *   We mirror this convention even for builtins, so all code handles argv
 *   the same way regardless of how the command is dispatched.
 *
 * YOUR TASK — add six fields:
 *   1. argv        : NULL-terminated array of argument strings (char **)
 *   2. argc        : count of arguments (int), not counting the NULL
 *   3. input_file  : filename for '<' redirection, or NULL (char *)
 *   4. output_file : filename for '>' or '>>' redirection, or NULL (char *)
 *   5. append      : 1 if '>>' (append), 0 if '>' (overwrite) (int)
 *   6. background  : 1 if '&' was present, 0 otherwise (int)
 */
typedef struct {
    /* YOUR FIELDS HERE */
    char        **argv;
    int         argc;
    char        *input_file;
    char        *output_file;
    int         append;
    int         background;
} Command;

/* ── Pipeline struct ──────────────────────────────────────────────────────── */

/*
 * Pipeline — A sequence of Commands connected by '|'.
 *
 * For:   ls -la | grep foo | wc -l
 *
 * The Pipeline would hold:
 *   commands[0] → Command { argv={"ls","-la",NULL}, ... }
 *   commands[1] → Command { argv={"grep","foo",NULL}, ... }
 *   commands[2] → Command { argv={"wc","-l",NULL}, ... }
 *   count = 3
 *
 * A single command (no pipes) is just a Pipeline with count = 1.
 * This uniform representation lets the executor always work with a
 * Pipeline, regardless of whether there's one or many commands.
 *
 * YOUR TASK — add two fields:
 *   1. commands : heap-allocated array of Command pointers (Command **)
 *   2. count    : number of Command entries in the array (int)
 */
typedef struct {
    /* YOUR FIELDS HERE */
    Command **commands;
    int count;
} Pipeline;

/* ── Builtin function pointer type ───────────────────────────────────────── */

/*
 * builtin_fn — A function pointer type for builtin command handlers.
 *
 * Every builtin (pwd, echo, ls, cd) must have exactly this signature:
 *   int builtin_xyz(int argc, char **argv)
 *
 * WHY THIS SIGNATURE?
 *   It mirrors the signature of main(). argc = argument count, argv =
 *   argument vector. This lets builtins behave exactly like standalone
 *   programs, making them easy to dispatch through a lookup table.
 *
 * A function pointer stores the *address* of a function, not its return
 * value. This lets us call different functions through a single variable.
 */
typedef int (*builtin_fn)(int argc, char **argv);

/* ── Builtin lookup table entry ──────────────────────────────────────────── */

/*
 * BuiltinEntry — One row in the builtin dispatch table.
 *
 * The executor holds an array of these. When it sees a command name,
 * it linearly searches this table. If found, it calls .fn directly
 * (no fork needed). If not found, it forks and calls execvp().
 *
 * Example table:
 *   { "pwd",  builtin_pwd  }
 *   { "echo", builtin_echo }
 *   { "ls",   builtin_ls   }
 *   { "cd",   builtin_cd   }
 *
 * This is the "dispatch table" pattern — an alternative to a long
 * if/else chain. Adding a new builtin is O(1): just add a row.
 */
typedef struct {
    const char *name;  /* command name to match against, e.g. "ls"    */
    builtin_fn  fn;    /* pointer to the handler function              */
} BuiltinEntry;

/* ── Global variable declarations ────────────────────────────────────────── */

/*
 * 'extern' means: "this variable is defined somewhere else (in a .c file);
 * just let me refer to it here." Without extern, every .c that includes
 * shell.h would create its own copy — causing linker 'multiple definition' errors.
 *
 * g_foreground_pid: PID of the currently running foreground child.
 *   Set to 0 when no child is running.
 *   The SIGINT handler reads this to know which process to interrupt.
 *   'volatile' tells the compiler: this variable can change outside the
 *   normal flow of the program (i.e., in a signal handler). Without volatile,
 *   the compiler might cache it in a register and miss signal-handler updates.
 *
 * g_prev_dir: stores the previous working directory for 'cd -'.
 *   Defined in cmd_cd.c, used only there, but declared here so any
 *   module can read it if needed.
 */
extern volatile pid_t g_foreground_pid;
extern char           g_prev_dir[PATH_MAX];
extern char           g_shell_home[PATH_MAX];

void print_prompt(void);

#endif /* SHELL_H */
