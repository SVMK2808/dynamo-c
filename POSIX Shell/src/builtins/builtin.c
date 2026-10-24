#include "../../include/builtin.h"
#include <string.h>

static const BuiltinEntry BUILTINS[] = {
    {"pwd", builtin_pwd},
    {"echo" , builtin_echo},
    {"cd", builtin_cd},
    {"ls", builtin_ls},
    {"exit", builtin_exit},
    {"search", builtin_search},
    {"pinfo", builtin_pinfo},
    {"history", builtin_history},
    {"connect", builtin_connect},
    {"ping", builtin_ping},
    {"put", builtin_put},
    {"get", builtin_get}
};

#define BUILTIN_COUNT (sizeof(BUILTINS)/sizeof(BUILTINS[0]))

builtin_fn find_builtin(const char *name){
    for(size_t i = 0; i < BUILTIN_COUNT; i++){
        if(strcmp(BUILTINS[i].name, name) == 0)
            return BUILTINS[i].fn; 
    }

    return NULL;
}