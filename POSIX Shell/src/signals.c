#include "../include/signals.h"
#include <signal.h>

#include <sys/wait.h>
#include <stdio.h>

static void handle_sigint(int sig){
    (void) sig;
    if(g_foreground_pid != 0)
        kill(g_foreground_pid, SIGINT);
}

static void handle_sigtstp(int sig){
    (void)sig;
    if(g_foreground_pid != 0){
        kill(g_foreground_pid, SIGTSTP);
    }
}

static void handle_sigchld(int sig){
    (void) sig;
    while(waitpid(-1, NULL, WNOHANG) > 0);
}

void setup_signals(void){
    struct sigaction sa;
    sa.sa_flags = SA_RESTART;

    sigemptyset(&sa.sa_mask);

    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    sa.sa_handler = handle_sigchld;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = handle_sigtstp;
    sigaction(SIGTSTP, &sa, NULL);
    
}