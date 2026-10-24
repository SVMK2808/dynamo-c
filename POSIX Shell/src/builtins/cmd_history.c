#include "../../include/builtin.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#define MAX_HISTORY 20
static char s_history[MAX_HISTORY][SHELL_MAX_INPUT];
static int s_history_count = 0;

// Helpers for implementing Up - Arrow command
int history_get_count(void){
    return s_history_count;
}

const char *history_get(int index){
    if(index >= 0 && index < s_history_count){
        return s_history[index];
    }
    return NULL;
}

static void get_history_file_path(char *buf, size_t size){
    snprintf(buf, size, "%s/.shell_history", g_shell_home);
}

void history_init(){
    char path[PATH_MAX];
    get_history_file_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if(!f) return;

    s_history_count = 0;
    char line[SHELL_MAX_INPUT];
    while(fgets(line, sizeof(line), f) && s_history_count < MAX_HISTORY){
        line[strcspn(line, "\n")] = '\0';
        if(line[0] != '\0'){
            strncpy(s_history[s_history_count++], line, SHELL_MAX_INPUT - 1);
        }
    }
    fclose(f);
}

static void save_history_to_file(void){
    char path[PATH_MAX];
    get_history_file_path(path, sizeof(path));

    FILE *f = fopen(path, "w");
    if(!f) return;
    
    for(int i = 0; i < s_history_count; i++){
        fprintf(f, "%s\n", s_history[i]);
    }
    fclose(f);
}

void history_add(const char *cmd){
    if(!cmd || cmd[0] == '\0') return;

    if(s_history_count > 0 && strcmp(s_history[s_history_count - 1], cmd) == 0){
        return;
    }

    if(s_history_count == MAX_HISTORY){
        for(int i = 0; i < MAX_HISTORY - 1; i++){
            strncpy(s_history[i], s_history[i + 1], SHELL_MAX_INPUT);
        }
        s_history_count = MAX_HISTORY - 1;
    }

    strncpy(s_history[s_history_count++], cmd, SHELL_MAX_INPUT - 1);
    s_history[s_history_count - 1][SHELL_MAX_INPUT - 1] = '\0';

    save_history_to_file();
}



int builtin_history(int argc, char **argv){
    if(argc > 2){
        fprintf(stderr, "history: too many arguments.\n");
        return 1;
    }

    int num = 10;
    if(argc == 2){
        num = atoi(argv[1]);
        if(num <= 0){
            fprintf(stderr, "history: invalid number\n");
            return 1;
        }
        if(num > MAX_HISTORY){
            num = MAX_HISTORY;
        }
    }

    if(num > s_history_count){
        num = s_history_count;
    }

    int start = s_history_count - num;
    for(int i = start; i < s_history_count; i++){
        printf("%s\n", s_history[i]);
    }
    return 0;
}