#include "../../include/builtin.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>

static int search_recursive(const char *dir_path, const char *target){
    DIR *dir = opendir(dir_path);
    if(!dir){
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(dir))){
        if(strcmp(entry -> d_name, ".") == 0 || strcmp(entry -> d_name, "..") == 0){
            continue;
        }

        if(strcmp(entry -> d_name, target) == 0){
            closedir(dir);
            return 1;
        }

        char child_path[PATH_MAX];
        snprintf(child_path, sizeof(child_path), "%s/%s", dir_path, entry -> d_name);

        struct stat st;
        if(lstat(child_path, &st) == 0 && S_ISDIR(st.st_mode)){
            if(search_recursive(child_path, target)){
                closedir(dir);
                return 1; // Found in subtree! Propogate success up the call stack
            }
        }
    }
    closedir(dir);
    return 0; // Not found in this directory or its subdirectories
}

int builtin_search(int argc, char **argv){
    if(argc != 2){
        fprintf(stderr, "search: expected exactly 1 argument\n");
        return 1;
    }

    int found = search_recursive(".", argv[1]);
    if(found){
        printf("True\n");
    }else{
        printf("False\n");
    }

    return 0;
}