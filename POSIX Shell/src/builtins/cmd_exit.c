#include <stdlib.h>

int builtin_exit(int argc, char **argv){
    (void) argc;
    (void) argv;
    exit(0);
}