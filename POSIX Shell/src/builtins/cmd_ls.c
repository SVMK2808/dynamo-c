#include "../../include/builtin.h"
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>

static void print_file_details(const char* full_path, const char *name){
    struct stat st;
    if(lstat(full_path, &st) != 0){
        perror("ls : lstat");
        return;
    }

    char perms[11];
    perms[0] = S_ISDIR(st.st_mode) ? 'd' : (S_ISLNK(st.st_mode) ? 'l' : '-');
    perms[1] = (st.st_mode & S_IRUSR) ? 'r' : '-';
    perms[2] = (st.st_mode & S_IWUSR) ? 'w' : '-';
    perms[3] = (st.st_mode & S_IXUSR) ? 'x' : '-';
    perms[4] = (st.st_mode & S_IRGRP) ? 'r' : '-';
    perms[5] = (st.st_mode & S_IWGRP) ? 'w' : '-';
    perms[6] = (st.st_mode & S_IXGRP) ? 'x' : '-';
    perms[7] = (st.st_mode & S_IROTH) ? 'r' : '-';
    perms[8] = (st.st_mode & S_IWOTH) ? 'w' : '-';
    perms[9] = (st.st_mode & S_IXOTH) ? 'x' : '-';
    perms[10] = '\0';

    struct passwd *pw = getpwuid(st.st_uid);
    struct group  *gr = getgrgid(st.st_gid);
    const char *user  = pw ? pw -> pw_name : "unknown";
    const char *group = gr ? gr -> gr_name : "unknown";

    char time_str[64];
    struct tm *tm_info = localtime(&st.st_mtime);
    strftime(time_str, sizeof(time_str), "%b %e %H:%M", tm_info);

    printf("%s %2ld %s %s %8lld %s %s\n",
            perms,
            (long)st.st_nlink,
            user, 
            group, 
            (long long)st.st_size,
            time_str,
            name);

}

int builtin_ls(int argc, char **argv){

    int show_all = 0;
    int long_format = 0;
    char *targets[MAX_ARGS];
    int target_count = 0;

    for(int i = 1; i < argc; i++){
        if(argv[i][0] == '-' && argv[i][1] != '\0'){
            for(int j = 1; argv[i][j] != '\0'; j++){
                if(argv[i][j] ==  'a'){
                    show_all = 1;
                }
                else if(argv[i][j] == 'l'){
                    long_format = 1;
                }else{
                    fprintf(stderr, "ls: invalid option -- '%c'\n", argv[i][j]);
                    return 1;
                }
            }
        } else {
            targets[target_count++] = argv[i];
        }
    }

    if(target_count == 0){
        targets[target_count++] = ".";
    }
    
    
    for(int t = 0; t < target_count; t++){
        const char *target = targets[t];
        char path[PATH_MAX];

        if(strcmp(target, "~") == 0){
            strcpy(path, g_shell_home);
        }else if(strncmp(target, "~/", 2) == 0){
            snprintf(path, sizeof(path), "%s%s", g_shell_home, target + 1);
        }else{
            strncpy(path, target, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }

        if(target_count > 1){
        if(t > 0) printf("\n");
        printf("%s:\n", target);
    }

        struct stat st;
        if(lstat(path, &st) != 0){
            perror("ls");
            continue;
        }

        if(!S_ISDIR(st.st_mode)){
            if(long_format){
                print_file_details(path, target);
            }else{
                printf("%s\n", target);
            }
            continue;
        }

        DIR *dir = opendir(path);
        if(!dir){
            perror("ls");
            continue;
        }

        if(long_format){
            long long total_blocks = 0;
            struct dirent *entry;
            while((entry = readdir(dir)) != NULL){
                if(!show_all && entry -> d_name[0] == '.') continue;
                char full_path[PATH_MAX];
                snprintf(full_path, sizeof(full_path), "%s/%s", path, entry ->d_name);
                struct stat file_st;
                if(lstat(full_path, &file_st) == 0){
                    total_blocks += file_st.st_blocks;
                }
            }

            printf("total %lld\n", total_blocks / 2);
            rewinddir(dir);
        }

        struct dirent *entry;
        while((entry = readdir(dir)) != NULL){
            if(!show_all && entry -> d_name[0] == '.') continue; //skipping hidden files
            
            char full_path[PATH_MAX];
            snprintf(full_path, sizeof(full_path), "%s/%s", path, entry ->d_name);

            if(long_format){
                print_file_details(full_path, entry -> d_name);
            }else{
                printf("%s\n", entry -> d_name);
            }
        }

    closedir(dir);
    

    }

    
    return 0;

}