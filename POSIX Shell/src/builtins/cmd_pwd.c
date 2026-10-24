#include "../../include/builtin.h"
#include <stdio.h>
#include <unistd.h>
#include <limits.h>

int builtin_pwd(int argc, char **argv){
    (void) argc; (void) argv;
    char buf[PATH_MAX];
    if(getcwd(buf, sizeof(buf)) == NULL){
        perror("pwd");
        return 1;
    }

    printf("%s\n", buf);
    return 0;
}