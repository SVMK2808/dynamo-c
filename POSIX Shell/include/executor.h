#ifndef EXECUTOR_H
#define EXECUTOR_H

/*
 * executor.h — Public API for the executor module.
 *
 * Any file that needs to run a parsed Pipeline just does:
 *   #include "executor.h"
 *
 * HOW THE EXECUTOR FITS INTO THE SHELL
 * ──────────────────────────────────────
 * The flow for every command the user types is:
 *
 *   main() → parse_line(input) → Pipeline
 *                              ↓
 *                    execute_pipeline(Pipeline)
 *                              ↓
 *              ┌───────────────┴────────────────┐
 *              │                                │
 *     builtin? (find_builtin)          external command?
 *     call fn() directly               fork() + execvp()
 *     (no fork needed)                 parent: waitpid()
 *
 * PIPELINE EXECUTION (2+ commands: ls | grep foo)
 * ────────────────────────────────────────────────
 * For each adjacent pair of commands, a pipe() is created.
 * Each command is run in its own child process.
 * The children wire their stdin/stdout to the pipe ends using dup2().
 * The parent closes all pipe ends and waits for all children.
 *
 * I/O REDIRECTION
 * ────────────────
 * Handled inside each child, before execvp():
 *   cmd->input_file  → open() + dup2() onto STDIN_FILENO
 *   cmd->output_file → open() + dup2() onto STDOUT_FILENO
 *                      (O_TRUNC if append==0, O_APPEND if append==1)
 *
 * BACKGROUND PROCESSES
 * ─────────────────────
 * If cmd->background == 1, the parent does NOT waitpid() immediately.
 * The child is reaped later by the SIGCHLD handler in signals.c.
 */

#include "shell.h"

/*
 * execute_pipeline() — Run a fully-parsed Pipeline.
 *
 * This is the single entry point into the executor. It handles all cases:
 *   - Single builtin command (no fork)
 *   - Single external command (fork + execvp)
 *   - Multi-command pipeline (pipe + fork per command)
 *   - I/O redirection (< > >>)
 *   - Background execution (&)
 *
 * Parameters:
 *   p — the Pipeline returned by parse_line(). Must not be NULL.
 *
 * Ownership:
 *   execute_pipeline() calls free_pipeline(p) before returning.
 *   The caller must NOT use p after this call.
 *
 * Return value:
 *   The exit status of the last command in the pipeline.
 *   Returns -1 on internal error (fork failure, etc.).
 */
int execute_pipeline(Pipeline *p);

#endif /* EXECUTOR_H */
