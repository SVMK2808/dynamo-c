#include "../../include/builtin.h"
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>


char g_prev_dir[PATH_MAX] = {0};

int builtin_cd(int argc, char **argv) {
    if (argc > 2) {
        fprintf(stderr, "Invalid arguments\n");
        return 1;
    }

    char current[PATH_MAX];
    if (getcwd(current, sizeof(current)) == NULL) {
        perror("cd: getcwd");
        return 1;
    }

    char target_path[PATH_MAX];
    const char *target;

    if (argc == 1 || strcmp(argv[1], "~") == 0) {
        target = g_shell_home;
    } else if (strncmp(argv[1], "~/", 2) == 0) {
        snprintf(target_path, sizeof(target_path), "%s%s", g_shell_home, argv[1] + 1);
        target = target_path;
    } else if (strcmp(argv[1], "-") == 0) {
        if (g_prev_dir[0] == '\0') {
            fprintf(stderr, "cd: no previous directory\n");
            return 1;
        }
        target = g_prev_dir;
        printf("%s\n", target);
    } else {
        target = argv[1];
    }

    if (chdir(target) != 0) {
        perror("cd");
        return 1;
    }

    strncpy(g_prev_dir, current, PATH_MAX - 1);
    g_prev_dir[PATH_MAX - 1] = '\0';

    return 0;
}