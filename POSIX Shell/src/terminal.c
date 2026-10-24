#include "../include/terminal.h"
#include "../include/shell.h"
#include "../include/builtin.h"
#include <termios.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/stat.h>

static struct termios orig_termios;
static int raw_active = 0;

static void disable_raw_mode(void){
    if(raw_active){
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_active = 0;
    }
}

static void enable_raw_mode(void){
    if(!isatty(STDIN_FILENO)) return;
    if(!raw_active){
        tcgetattr(STDIN_FILENO, &orig_termios);
        atexit(disable_raw_mode);
    }

    struct termios raw = orig_termios;
    /* Disable cannonical mode and auto - echo; keep signal generation (ISIG) active */
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_active = 1;
}

/* Helper for Tab autocomplete */
static void handle_tab(char *buffer, int *len_ptr, size_t max_len){
    int len = *len_ptr;

    /* Find the start of the word being completed (after last space) */
    int word_start = len;
    while(word_start > 0 && buffer[word_start - 1] != ' '){
        word_start--;
    }

    const char *prefix = buffer + word_start;
    size_t prefix_len = len - word_start;

    /*2. Read entries from current directory */
    DIR *d = opendir(".");
    if(!d) return;

    char matches[MAX_ARGS][256];
    int match_count = 0;

    struct dirent *de;
    while((de = readdir(d)) != NULL){
        if(de -> d_name[0] == '.' && prefix[0] != '.') continue;

        if(strncmp(de -> d_name, prefix, prefix_len) == 0){
            if(match_count < MAX_ARGS){
                strncpy(matches[match_count++], de -> d_name, sizeof(matches[0]) - 1);
            }
        }
    }
    closedir(d);

    /*3. Single match: autocomplete the suffix */
    if(match_count == 1){
        const char *suffix = matches[0] + prefix_len;
        size_t suffix_len = strlen(suffix);

        /* Append '/' if it is a directory */
        char extra[2] = "";
        struct stat st;
        if(stat(matches[0], &st) == 0 && S_ISDIR(st.st_mode)){
            extra[0] = '/';
            extra[1] = '\0';
        }

        if(len + suffix_len + strlen(extra) < max_len - 1){
            strcat(buffer, suffix);
            strcat(buffer, extra);
            len += suffix_len + strlen(extra);
            *len_ptr = len;
            printf("%s%s", suffix, extra);
            fflush(stdout);
        }
    }else if(match_count > 1){
        /* Multiple matches: print them space-separated, then redraw prompt */
        printf("\n");
        for(int i = 0; i < match_count; i++){
            printf("%s%s", matches[i], (i == match_count - 1) ? "\n" : " ");
        }
        print_prompt();
        printf("%s", buffer);
        fflush(stdout);
    }
}

int read_line_raw(char *buffer, size_t max_len){
    /* If input is piped (e.g. scripts or test suites), use standard fgets */
    if(!isatty(STDIN_FILENO)){
        if(fgets(buffer, max_len, stdin) == NULL){
            return -1;
        }
        buffer[strcspn(buffer, "\r\n")] = '\0';
        return (int)strlen(buffer);
    }

    enable_raw_mode();

    int len = 0;
    buffer[0] = '\0';
    int history_index = history_get_count();

    while(1){
        char c;
        if(read(STDIN_FILENO, &c, 1) <= 0){
            if(len == 0){
                disable_raw_mode();
                return -1;
            }
            break;
        }

        /* 1. Enter key */
        if(c == '\n' || c == '\r'){
            buffer[len] = '\0';
            printf("\n");
            fflush(stdout);
            break;
        }

        /* 2. Ctrl + D (EOF on empty prompt) */
        if(c == 4){
            if(len == 0){
                disable_raw_mode();
                return -1;
            }
            continue;
        }

        /* 3. Backspace */
        if(c == 127 || c == 8){
            if(len > 0){
                len--;
                buffer[len] = '\0';
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }

        /* 4. Tab key: Autocomplete */
        if(c == '\t'){
            handle_tab(buffer, &len, max_len);
            continue;
        }

        /* 5. Escape sequence (arrow keys) */
        if(c == '\033'){
            char seq[2];
            if(read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1){
                if(seq[0] == '['){
                    if(seq[1] == 'A'){
                        /* UP ARROW: Previous command from history */
                        int h_count = history_get_count();
                        if(h_count > 0 && history_index > 0){
                            history_index--;
                            const char *cmd = history_get(history_index);
                            if(cmd){
                                while(len > 0){
                                    printf("\b \b");
                                    len--;
                                }

                                len = (int)strlen(cmd);
                                if(len >= (int)max_len) len = max_len - 1;
                                strncpy(buffer, cmd, len);
                                buffer[len] = '\0';
                                printf("%s", buffer);
                                fflush(stdout);
                            }
                        }
                    }else if(seq[1] == 'B'){
                        /* DOWN ARROW: Next command from history */
                        int h_count = history_get_count();
                        if(history_index < h_count - 1){
                            history_index++;
                            const char *cmd = history_get(history_index);
                            if(cmd){
                                while(len > 0){
                                    printf("\b \b");
                                    len--;
                                }

                                len = (int)strlen(cmd);
                                if(len >= (int)max_len) len = max_len - 1;
                                strncpy(buffer, cmd, len);
                                buffer[len] = '\0';
                                printf("%s", buffer);
                                fflush(stdout);
                            }
                        } else if(history_index == h_count - 1){
                            history_index++;
                            while(len > 0){
                                printf("\b \b");
                                len--;
                            }

                            buffer[0] = '\0';
                            fflush(stdout);
                        }
                    }
                }
            }
            continue;
        }

        if(c >= 32 && c < 127){
            if(len < (int) max_len - 1){
                buffer[len++] = c;
                buffer[len] = '\0';
                putchar(c);
                fflush(stdout);
            }
        }
    }

    disable_raw_mode();
    return len;
}