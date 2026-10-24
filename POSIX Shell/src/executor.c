#include "../include/executor.h"
#include "../include/builtin.h"
#include "../include/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <string.h>

/*
 * move_fd - dup2 old onto new, then close the now-redundant old
 * Does nothing if old == new (already in the right place)
 */
static void move_fd(int src, int dst) {
    if (src == dst) return;
    dup2(src, dst);
    close(src);
}

static void apply_redirection(Command *cmd){
    if(cmd -> input_file){
        int fd = open(cmd -> input_file, O_RDONLY);
        if(fd == -1) {
            perror("Error opening the file for read");
            exit(1);
        }

        move_fd(fd, STDIN_FILENO);
    }

    if(cmd -> output_file){
        int flags = O_WRONLY | O_CREAT | (cmd -> append ? O_APPEND : O_TRUNC); 
        int fd = open(cmd ->output_file, flags, 0644);
        if(fd == -1){
            perror("Error in opening the file for write");
            exit(1);
        }

        move_fd(fd, STDOUT_FILENO);
    }
}

static int run_single(Command *cmd){
    builtin_fn fn = find_builtin(cmd -> argv[0]);
    if(fn){
        int saved_stdout = -1;
        int saved_stdin = -1;
        if (cmd->output_file) saved_stdout = dup(STDOUT_FILENO);
        if (cmd->input_file)  saved_stdin = dup(STDIN_FILENO);

        apply_redirection(cmd);
        int rc = fn(cmd -> argc, cmd -> argv);

        if (saved_stdout != -1) {
            dup2(saved_stdout, STDOUT_FILENO);
            close(saved_stdout);
        }
        if (saved_stdin != -1) {
            dup2(saved_stdin, STDIN_FILENO);
            close(saved_stdin);
        }
        return rc;
    }

    // External command
    pid_t pid = fork();
    if(pid == -1) {
        perror("fork failed");
        return -1;
    }

    if(pid == 0){
        //Child: restore default signal behaviour before execvp
        signal(SIGINT,  SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        apply_redirection(cmd);
        execvp(cmd->argv[0], cmd->argv);
        perror(cmd->argv[0]);
        exit(1);
    }else{
        //Parent
        if(cmd->background){
            printf("%d\n", pid);   /* print PID of background process */
            return 0;
        }
        int status;
        g_foreground_pid = pid;
        waitpid(pid, &status, WUNTRACED);   /* return on stop OR exit */
        g_foreground_pid = 0;
        if (WIFSTOPPED(status))             /* child was paused by Ctrl+Z */
            return 0;
        return WEXITSTATUS(status);
    }
}

int execute_pipeline(Pipeline *p){
    if(p -> count == 1){
        int result = run_single(p -> commands[0]);
        free_pipeline(p);
        return result;
    }

    //Multi - command pipeline: eg. ls | grep foo | wc -l

    /* For each adjacent pair, create a pipe. 
     * We need (count - 1) pipes total
     */
     int n = p -> count;
     int pipes[n - 1][2];
     pid_t pids[n];
    
     // Create all pipes first
     for(int i = 0 ; i < n - 1; i++){
        pipe(pipes[i]);
     }

     // Fork one child per command
     for(int i = 0; i < n; i++){
        pids[i] = fork();

        if (pids[i] == -1) {

            perror("fork");
            exit(1);
        }

        if(pids[i] == 0){
            //Child: restore default signal behaviour before execvp
            signal(SIGINT,  SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            if(i != 0) move_fd(pipes[i - 1][0], STDIN_FILENO);
            if(i != n - 1) move_fd(pipes[i][1], STDOUT_FILENO);

            for(int j = 0; j < n - 1; j++){
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            // close All pipe ends in the child 
            apply_redirection(p -> commands[i]);
            builtin_fn fn = find_builtin(p -> commands[i] -> argv[0]);
            if (fn) {
                int rc = fn(p -> commands[i] -> argc, p -> commands[i] -> argv);
                exit(rc);
            }
            execvp(p -> commands[i] -> argv[0], p -> commands[i] -> argv);
            perror(p -> commands[i] -> argv[0]);
            exit(1);
        }
       
     }

     // Parent: close ALL pipe ends
     for(int i = 0; i < n - 1; i++){
        close(pipes[i][0]);
        close(pipes[i][1]);
     }

     int status, last = 0;
     for(int i = 0; i < n; i++){
        waitpid(pids[i], &status, 0);
        if(i == n - 1) last = WEXITSTATUS(status);
     }

     free_pipeline(p);
     return last;
}