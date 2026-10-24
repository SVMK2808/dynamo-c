/*
 * main.c — POSIX Shell entry point.
 *
 * The REPL (Read-Eval-Print Loop):
 *   1. Print a prompt
 *   2. Read a line from the user
 *   3. Parse it into a Pipeline
 *   4. Execute the pipeline
 *   5. Repeat until Ctrl+D (EOF)
 */

#include "../include/builtin.h"
#include "shell.h"
#include "parser.h"
#include "executor.h"
#include "signals.h"
#include "terminal.h"
#include <stdio.h>
#include <string.h>

/* ── Global definitions ───────────────────────────────────────────────────── */
/* g_foreground_pid: owned here; read by the SIGINT handler (signals.c) */
volatile pid_t g_foreground_pid = 0;
/* g_prev_dir: defined in cmd_cd.c — declared extern in shell.h */
char           g_shell_home[PATH_MAX] = {0};

/* ── Prompt ───────────────────────────────────────────────────────────────── */
void print_prompt(void) {
    /* 1. Username */
    struct passwd *pw = getpwuid(getuid());
    const char *user = pw ? pw->pw_name : getenv("USER");
    if (!user || user[0] == '\0') {
        user = "user";
    }

    /* 2. System hostname */
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        strcpy(hostname, "localhost");
    } else {
        hostname[sizeof(hostname) - 1] = '\0';
    }

    /* 3. Current working directory */
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        strcpy(cwd, "?");
    }

    /* 4. Format directory relative to shell launch directory (~) */
    char display_path[PATH_MAX];
    size_t home_len = strlen(g_shell_home);

    if (strcmp(cwd, g_shell_home) == 0) {
        strcpy(display_path, "~");
    } else if (home_len > 0 && strncmp(cwd, g_shell_home, home_len) == 0 && cwd[home_len] == '/') {
        snprintf(display_path, sizeof(display_path), "~%s", cwd + home_len);
    } else {
        strncpy(display_path, cwd, sizeof(display_path) - 1);
        display_path[sizeof(display_path) - 1] = '\0';
    }

    /* 5. Print prompt: <username@hostname:display_path> */
    printf("<%s@%s:%s> ", user, hostname, display_path);
    fflush(stdout);
}

/* ── Main ─────────────────────────────────────────────────────────────────── */
int main(void) {
    char input[SHELL_MAX_INPUT];

    /* Record the shell launch directory as home (~) */
    if (getcwd(g_shell_home, sizeof(g_shell_home)) == NULL) {
        perror("getcwd");
        strcpy(g_shell_home, "/");
    }

    setup_signals();
    history_init();

    while (1) {
        print_prompt();

        /* Read interactive line with Tab autocomplete and Up - Arrow history */
        if (read_line_raw(input, sizeof(input)) < 0) {
            printf("\n");   /* move off the prompt line */
            break;
        }

        /* Skip blank lines */
        if (input[0] == '\0') continue;

        /* Add to history before strtok_r modifies the string */
        history_add(input);

        /* Split the line by ';' into sequential commands */
        char *saveptr = NULL;
        char *cmd_chunk = strtok_r(input, ";", &saveptr);

        while (cmd_chunk != NULL) {
            Pipeline *p = parse_line(cmd_chunk);
            if (p != NULL) {
                execute_pipeline(p);
            }
            cmd_chunk = strtok_r(NULL, ";", &saveptr);
        }
    }

    return 0;
}
