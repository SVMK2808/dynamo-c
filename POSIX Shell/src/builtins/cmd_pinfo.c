#include "../../include/builtin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#ifdef __APPLE__
#include <libproc.h>
#endif

int builtin_pinfo(int argc, char **argv){
    if(argc > 2){
        fprintf(stderr, "pinfo: too many arguments.\n");
        return 1;
    }

    pid_t pid = (argc == 1) ? getpid() : (pid_t)atoi(argv[1]);
    if(pid <= 0){
        fprintf(stderr, "pinfo: invalid pid\n");
        return 1;
    }

    char state = 'R';
    int is_foreground = 0;
    char vmsize[64] = "0";
    char exe_path[PATH_MAX] = "unknown";


    #ifdef __APPLE__
        /* -- macOS Implementation (for local testing on my Mac) */
        struct proc_taskinfo pti;
        if(proc_pidinfo(pid, PROC_PIDTASKINFO, 0 , &pti, sizeof(pti)) <= 0){
            fprintf(stderr, "pinfo: process %d not found\n", pid);
            return 1;
        }

        snprintf(vmsize, sizeof(vmsize), "%lld", (long long)(pti.pti_virtual_size / 1024));

        /* Check if foreground */
        pid_t fg_pgrp = tcgetpgrp(STDIN_FILENO);
        pid_t pgrp = getpgid(pid);
        if(fg_pgrp != -1 && fg_pgrp == pgrp){
            is_foreground = 1;
        }

        if(proc_pidpath(pid, exe_path, sizeof(exe_path)) <= 0){
            strcpy(exe_path, "unknown");
        }
    #else
        /* -- Linux Implememtation (evaluates on Ubuntu /proc) ------- */
        char stat_file[64];
        snprintf(stat_file, sizeof(stat_file), "/proc/%d/stat", pid);
        FILE *f_stat = fopen(stat_file, "r");
        if(!f_stat){
            fprintf(stderr, "pinfo: process %d not found\n", pid);
            return 1;
        }

        char stat_buf[1024];
        if(!fgets(stat_buf, sizeof(stat_buf), f_stat)){
            fclose(f_stat);
            return 1;
        }
        fclose(f_stat);

        // Find closing parenthesis of comm
        char *close_paren = strrchr(stat_buf, ')');
        if(!close_paren) return 1;

        int pgrp = 0, tpgid = 0;
        sscanf(close_paren + 2, "%c %*d %d %*d %*d %d", &state, &pgrp, &tpgid);
        if(pgrp == tpgid){
            is_foreground = 1;
        }

        // Read VmSize from /proc/<pid>/status 
        char status_file[64];
        snprintf(status_file, sizeof(status_file), "/proc/%d/status", pid);
        FILE *f_status = fopen(status_file, "r");
        if(f_status){
            char line[256];
            while(fgets(line, sizeof(line), f_status)){
                if(strncmp(line, "VmSize:", 7) == 0){
                    sscanf(line, "VmSize: %s", vmsize);
                    break;
                }
            }
            fclose(f_status);
        }

        // Read symlink /proc/<pid>/exe
        char exe_link[64];
        snprintf(exe_link, sizeof(exe_link), "/proc/%d/exe", pid);
        ssize_t len = readlink(exe_link, exe_path, sizeof(exe_path) - 1);
        if(len != -1){
            exe_path[len] = '\0';
        }else{
            strcpy(exe_path, "unknown");
        }
    #endif

    //Format executable path relative to g_shell_home
    char display_exe[PATH_MAX];
    size_t home_len = strlen(g_shell_home);
    if(strcmp(exe_path, g_shell_home) == 0){
        strcpy(display_exe, "~");
    }else if(home_len > 0 && strncmp(exe_path, g_shell_home, home_len) == 0
             && exe_path[home_len] == '/'){
                snprintf(display_exe, sizeof(display_exe), "~%s", exe_path + home_len);
             } else {
                strncpy(display_exe, exe_path, sizeof(display_exe) - 1);
                display_exe[sizeof(display_exe) - 1] = '\0';
             }

    printf("pid -- %d\n", pid);
    printf("Process Status -- %c%s\n", state, is_foreground ? "+" : "");
    printf("memory -- %s {Virtual Memory}\n", vmsize);
    printf("Executable Path -- %s\n", display_exe);

    return 0;

}