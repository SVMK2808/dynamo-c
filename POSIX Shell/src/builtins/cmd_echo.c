#include "../../include/builtin.h"
#include <stdio.h>
#include <string.h>

int builtin_echo(int argc, char **argv){
    int start = 1;
    int no_newline = 0;
    if(argc > 1 && strcmp(argv[1], "-n") == 0){
        no_newline = 1;
        start = 2;
    }

    for(int i = start; i < argc; i++){
        if(i == argc - 1) 
            printf("%s", argv[i]);
        else 
            printf("%s ", argv[i]);
         
    }
    if(no_newline == 0) printf("\n");
    return 0;

}