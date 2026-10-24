#ifndef BUILTIN_H
#define BUILTIN_H

/*
 * builtin.h — Public API for the builtin command module.
 *
 * Any file that needs to dispatch or call builtins just does:
 *   #include "builtin.h"
 *
 * HOW BUILTINS DIFFER FROM EXTERNAL COMMANDS
 * ─────────────────────────────────────────────
 * External commands (e.g. /usr/bin/grep) are run by fork()-ing a child
 * and calling execvp() inside the child. The shell just waits.
 *
 * Builtins (pwd, echo, cd, ls) must run INSIDE the shell process because:
 *   - cd: changes the working directory of the CURRENT process. A forked
 *         child's chdir() would have no effect on the shell's own cwd.
 *   - echo, pwd, ls: could be external, but implementing them as builtins
 *         avoids a fork/exec overhead and lets us control exact behavior.
 *
 * HOW DISPATCH WORKS
 * ──────────────────
 * The executor calls find_builtin(argv[0]) before deciding to fork.
 * find_builtin() does a linear search through the BUILTINS[] table
 * (defined in builtin.c). If a match is found, the executor calls
 * the function pointer directly — no fork, no exec.
 *
 * ADDING A NEW BUILTIN
 * ─────────────────────
 * 1. Write  int builtin_<name>(int argc, char **argv)  in a new .c file.
 * 2. Declare it below.
 * 3. Add a { "<name>", builtin_<name> } row to BUILTINS[] in builtin.c.
 * 4. Add the .c file to the Makefile SRCS list.
 * That's it — no other files need to change.
 */

#include "shell.h"  /* for builtin_fn, BuiltinEntry */

/* ── Dispatcher ───────────────────────────────────────────────────────────── */

/*
 * find_builtin() — Look up a command name in the builtin dispatch table.
 *
 * Parameters:
 *   name — the command name to look up (e.g. "ls", "cd"). Compared with
 *           strcmp(), so it must be a null-terminated C string.
 *
 * Returns:
 *   A pointer to the matching builtin_fn if found, NULL otherwise.
 *
 * Usage in the executor:
 *   builtin_fn fn = find_builtin(cmd->argv[0]);
 *   if (fn) {
 *       int exit_code = fn(cmd->argc, cmd->argv);
 *   } else {
 *       // fork() + execvp() path
 *   }
 */
builtin_fn find_builtin(const char *name);

/* ── Individual builtin declarations ─────────────────────────────────────── */

/*
 * Every builtin follows the same signature:
 *   int builtin_<name>(int argc, char **argv)
 *
 * argc  — number of arguments (argv[0] is the command name itself)
 * argv  — NULL-terminated argument array, e.g. {"echo", "-n", "hi", NULL}
 *
 * Return value conventions (mirror shell exit codes):
 *   0  → success
 *   1  → general error (bad arguments, syscall failure, etc.)
 *   2  → misuse / wrong number of arguments
 */

/*
 * builtin_pwd — Print the current working directory.
 *
 * Calls getcwd() and prints the result followed by a newline.
 * Extra arguments are ignored (consistent with /bin/pwd behavior).
 *
 * Example: pwd
 *   Output: /home/user/project
 */
int builtin_pwd(int argc, char **argv);

/*
 * builtin_echo — Print arguments to stdout.
 *
 * Flags:
 *   -n  suppress the trailing newline
 *
 * Arguments are printed space-separated. No other flags are supported
 * in this implementation (no -e escape sequences).
 *
 * Examples:
 *   echo hello world     → "hello world\n"
 *   echo -n hello        → "hello"  (no newline)
 */
int builtin_echo(int argc, char **argv);

/*
 * builtin_cd — Change the current working directory.
 *
 * Supported forms:
 *   cd            → change to $HOME (read from getenv("HOME"))
 *   cd <path>     → change to <path> using chdir()
 *   cd -          → change to previous directory (stored in g_prev_dir)
 *
 * On success, updates g_prev_dir to the directory we came from.
 * On failure, prints an error with perror() and returns 1.
 */
int builtin_cd(int argc, char **argv);

/*
 * builtin_ls — List directory contents.
 *
 * Uses opendir() / readdir() / stat() — no execvp("/bin/ls").
 *
 * Behavior:
 *   ls            → list current directory (.)
 *   ls <path>     → list specified directory
 *
 * Output: one filename per line, skipping hidden files (names starting
 * with '.') unless -a flag is given.
 *
 * Note: This is a simplified implementation. It does NOT support
 * column formatting or color output.
 */
int builtin_ls(int argc, char **argv);
int builtin_exit(int argc, char **argv);
int builtin_search(int argc, char **argv);
int builtin_pinfo(int argc, char **argv);

//History command helpers and main function
int         builtin_history(int argc, char **argv);
void        history_init(void);
void        history_add(const char *cmd);
int         history_get_count(void);
const char* history_get(int index);

// Dynamo Distributed Key-Value Store Builtins
int         builtin_connect(int argc, char **argv);
int         builtin_ping(int argc, char **argv);
int         builtin_put(int argc, char **argv);
int         builtin_get(int argc, char **argv);

#endif /* BUILTIN_H */
