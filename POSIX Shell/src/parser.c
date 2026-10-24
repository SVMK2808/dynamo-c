/*
 * parser.c — Two-Phase Parser Implementation
 *
 * ═══════════════════════════════════════════════════════════════════
 * THE TWO-PHASE DESIGN
 * ═══════════════════════════════════════════════════════════════════
 *
 * Phase 1 — Tokenization (tokenize)
 *   Input:  "ls -la | grep foo > out.txt &"   (raw string)
 *   Output: [TOKEN_WORD:"ls"] [TOKEN_WORD:"-la"] [TOKEN_PIPE]
 *           [TOKEN_WORD:"grep"] [TOKEN_WORD:"foo"] [TOKEN_REDIR_OUT]
 *           [TOKEN_WORD:"out.txt"] [TOKEN_BACKGROUND] [TOKEN_EOF]
 *
 *   The tokenizer is pure syntax — it doesn't care what "ls" means.
 *   It just recognizes boundaries between tokens.
 *
 * Phase 2 — Parsing (build_pipeline)
 *   Input:  the flat Token array from Phase 1
 *   Output: a Pipeline of Command structs with argc/argv, redirections, flags
 *
 *   The parser gives *meaning* to the token stream. A TOKEN_REDIR_OUT
 *   followed by a TOKEN_WORD means "redirect stdout to that filename."
 *
 * WHY SEPARATE PHASES?
 *   If you mixed tokenizing and parsing into one pass, the code becomes
 *   tangled and hard to debug. Separation of concerns: each phase is
 *   testable independently. This is how real compilers work too
 *   (lexer → parser → AST).
 *
 * ═══════════════════════════════════════════════════════════════════
 * MEMORY MODEL
 * ═══════════════════════════════════════════════════════════════════
 *
 * Everything heap-allocated here must be freed by free_pipeline().
 * The ownership chain:
 *
 *   Pipeline
 *     └── commands[]  (heap-allocated array of Command*)
 *           └── Command  (heap-allocated)
 *                 ├── argv[]  (heap-allocated array of char*)
 *                 │     └── each string  (strdup'd → heap)
 *                 ├── input_file  (strdup'd → heap, or NULL)
 *                 └── output_file (strdup'd → heap, or NULL)
 */

#include "../include/parser.h"
#include <ctype.h>   /* isspace() — returns non-zero if char is whitespace */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Forward declarations of internal helpers ─────────────────────────────── */
/* 'static' means these functions are invisible outside this file.             */
/* They are implementation details, not part of the public API.                */

static Token    *tokenize(char *input, int *count);
static Pipeline *build_pipeline(Token *tokens, int token_count);

static int is_special(char c){
    return c == '&' || c == '>' 
                  || c == '<' || c == '|';
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* PUBLIC API                                                                  */
/* ═══════════════════════════════════════════════════════════════════════════ */

/*
 * parse_line() — Orchestrates Phase 1 + Phase 2.
 *
 * This function is already implemented — study it to understand how the
 * two phases connect. Notice:
 *   - It validates input before doing any work.
 *   - It owns the temporary Token array and frees it before returning.
 *   - The Pipeline it returns outlives the Token array because all strings
 *     were strdup'd (copied) into the Command structs.
 */
Pipeline *parse_line(char *input) {
    /* Guard: nothing to parse */
    if (input == NULL || input[0] == '\0')
        return NULL;

    /* Phase 1: tokenize the raw string into a flat Token array */
    int    token_count = 0;
    Token *tokens      = tokenize(input, &token_count);

    if (tokens == NULL || token_count == 0)
        return NULL;

    /* Phase 2: interpret the token stream into a structured Pipeline */
    Pipeline *pipeline = build_pipeline(tokens, token_count);

    /*
     * Free the temporary token array.
     * The token values (strings) were strdup'd into Command.argv and
     * Command.input_file/output_file during build_pipeline, so freeing
     * the tokens here does NOT free the data the Pipeline points to.
     */
    for (int i = 0; i < token_count; i++)
        free(tokens[i].value);
    free(tokens);

    return pipeline;  /* caller must call free_pipeline() on this */
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/* YOUR IMPLEMENTATIONS GO BELOW                                               */
/* ═══════════════════════════════════════════════════════════════════════════ */

/*
 * ── FUNCTION 1: tokenize() ────────────────────────────────────────────────
 *
 * PURPOSE
 *   Walk the raw input character-by-character and produce a flat array of
 *   Token structs. Each token carries a string value and a type tag.
 *
 * PARAMETERS
 *   input  — the raw input line (you may modify it in place, but working
 *             on the original is fine since we process char by char)
 *   count  — OUTPUT parameter: you must set *count to the number of tokens
 *             you produced (excluding the TOKEN_EOF sentinel)
 *
 * RETURN VALUE
 *   Heap-allocated Token array. The caller (parse_line) will free it.
 *   Return NULL on allocation failure.
 *
 * ALGORITHM — implement exactly this logic:
 * ─────────────────────────────────────────
 *   Allocate a Token array of size MAX_ARGS (generous upper bound).
 *   Maintain an index i into input[], starting at 0.
 *   Maintain an index n = 0 into your token array.
 *
 *   while (input[i] != '\0'):
 *
 *     STEP A — Skip whitespace:
 *       while (isspace(input[i])) i++
 *       if input[i] == '\0': break   ← nothing left after whitespace
 *
 *     STEP B — Recognize special single/double characters:
 *       if input[i] == '|':
 *           emit token: { strdup("|"), TOKEN_PIPE }, i++
 *
 *       elif input[i] == '&':
 *           emit token: { strdup("&"), TOKEN_BACKGROUND }, i++
 *
 *       elif input[i] == '<':
 *           emit token: { strdup("<"), TOKEN_REDIR_IN }, i++
 *
 *       elif input[i] == '>':
 *           if input[i+1] == '>':                     ← peek ahead
 *               emit token: { strdup(">>"), TOKEN_REDIR_APPEND }, i += 2
 *           else:
 *               emit token: { strdup(">"), TOKEN_REDIR_OUT }, i++
 *
 *     STEP C — Otherwise, collect a word:
 *       else:
 *           Mark start = i
 *           while input[i] is not '\0', space, '|', '&', '<', '>': i++
 *           The word is input[start .. i-1]
 *           Use strndup(input + start, i - start) to copy it
 *           emit token: { that_string, TOKEN_WORD }
 *
 *   After the loop:
 *     Append a sentinel: { NULL, TOKEN_EOF }
 *     *count = n   (do NOT count the EOF sentinel in *count)
 *     return the token array
 *
 * KEY FUNCTIONS TO USE
 *   isspace(c)           — <ctype.h>: returns non-zero if c is space/tab/newline
 *   strdup(s)            — allocates a heap copy of string s
 *   strndup(s, n)        — allocates a heap copy of the first n bytes of s
 *   malloc(n * sizeof(Token)) — allocate the token array
 *
 * HINT: isspecial(c) — you can write a tiny inline helper:
 *   static int is_special(char c) {
 *       return c == '|' || c == '&' || c == '<' || c == '>';
 *   }
 */
static Token *tokenize(char *input, int *count) {
    /* TODO: implement following the algorithm above */
    *count = 0;

    Token *token = (Token*) malloc(MAX_ARGS * sizeof(Token));
    int i = 0;
    int n = 0;

    while(input[i] != '\0'){
        while(isspace(input[i])) i++;
        if(input[i] == '\0') break; 

        else if(input[i] == '|'){
            token[n++] = (Token) { strdup("|"), TOKEN_PIPE};
            i++;
        }

        else if(input[i] == '&'){
            token[n++] = (Token) { strdup("&"), TOKEN_BACKGROUND};
            i++;
        } else if (input[i] == '<') {
            token[n++] = (Token) { strdup("<"), TOKEN_REDIR_IN};
            i++;
       } 

        else if(input[i] == '>'){
            if(input[i + 1] == '>'){
                token[n++] = (Token) {strdup(">>"), TOKEN_REDIR_APPEND};
                i += 2;
            }else{
                token[n++] = (Token) {strdup(">"), TOKEN_REDIR_OUT};
                i++;
            }
        } else if (input[i] == '"' || input[i] == '\'') {
            char quote = input[i++];
            int start = i;
            while (input[i] != '\0' && input[i] != quote) i++;
            token[n++] = (Token) { strndup(input + start, i - start), TOKEN_WORD };
            if (input[i] == quote) i++;
        } else {
            int start = i;
            while(input[i] != '\0' && !isspace(input[i]) && !is_special(input[i])) i++;
            token[n++] = (Token) { strndup(input + start, i - start), TOKEN_WORD};
        }


    }

    token[n] = (Token){NULL, TOKEN_EOF};
    *count = n;
    return token;
}

/*
 * ── FUNCTION 2: build_pipeline() ─────────────────────────────────────────
 *
 * PURPOSE
 *   Walk the Token array produced by tokenize() and build a Pipeline
 *   (an ordered collection of Command structs connected by pipes).
 *
 * PARAMETERS
 *   tokens      — the Token array from tokenize()
 *   token_count — number of tokens (not counting TOKEN_EOF)
 *
 * RETURN VALUE
 *   Heap-allocated Pipeline. The caller (parse_line) takes ownership.
 *   Return NULL on allocation failure.
 *
 * ALGORITHM — implement exactly this logic:
 * ─────────────────────────────────────────
 *   Allocate a Pipeline struct.
 *   Allocate pipeline->commands as an array of MAX_PIPELINE Command pointers.
 *   pipeline->count = 0.
 *
 *   Allocate the FIRST Command struct (heap), zero-initialize it.
 *   Set cmd = that Command.
 *   Allocate cmd->argv as an array of MAX_ARGS char pointers, zero them.
 *   cmd->argc = 0.
 *
 *   Walk tokens[i] for i = 0 .. token_count - 1:
 *
 *     TOKEN_WORD:
 *       cmd->argv[cmd->argc] = strdup(tokens[i].value)
 *       cmd->argc++
 *
 *     TOKEN_PIPE:
 *       NULL-terminate current argv:  cmd->argv[cmd->argc] = NULL
 *       Push cmd into pipeline->commands[pipeline->count++]
 *       Allocate a NEW Command (heap, zeroed), set cmd = new command
 *       Allocate new cmd->argv (heap, zeroed)
 *
 *     TOKEN_REDIR_IN:
 *       i++  ← advance to the filename token
 *       cmd->input_file = strdup(tokens[i].value)
 *
 *     TOKEN_REDIR_OUT:
 *       i++  ← advance to the filename token
 *       cmd->output_file = strdup(tokens[i].value)
 *       cmd->append = 0
 *
 *     TOKEN_REDIR_APPEND:
 *       i++  ← advance to the filename token
 *       cmd->output_file = strdup(tokens[i].value)
 *       cmd->append = 1
 *
 *     TOKEN_BACKGROUND:
 *       cmd->background = 1
 *       (Note: background applies to the whole pipeline. You may also
 *        store it on the Pipeline struct if you prefer — either works.)
 *
 *     TOKEN_EOF:
 *       break
 *
 *   After the loop:
 *     NULL-terminate the last command's argv:  cmd->argv[cmd->argc] = NULL
 *     Push the last cmd into the pipeline:
 *       pipeline->commands[pipeline->count++] = cmd
 *
 *   Return the pipeline.
 *
 * IMPORTANT EDGE CASE
 *   If the pipeline has no commands (e.g., input was just whitespace that
 *   tokenize emitted as TOKEN_EOF immediately), return NULL or a pipeline
 *   with count = 0, and the executor must handle that gracefully.
 *
 * KEY FUNCTIONS TO USE
 *   malloc / calloc   — allocate structs and arrays
 *   strdup            — copy token strings into Command fields
 *   memset            — zero-initialize a struct (or use calloc)
 */
static Pipeline *build_pipeline(Token *tokens, int token_count) {
    /* TODO: implement following the algorithm above */
    
    Pipeline *pipeline = (Pipeline *) malloc(sizeof(Pipeline));
    pipeline -> count = 0;
    pipeline -> commands = malloc(MAX_PIPELINE * sizeof(Command *));



    Command *cmd = calloc(1, sizeof(Command));
    cmd -> argv = malloc(MAX_ARGS * sizeof(char *));
    cmd -> argc = 0;

    for(int i = 0; i < token_count; i++){
        switch(tokens[i].type){
            case TOKEN_WORD : 
                cmd -> argv[cmd -> argc++] = strdup(tokens[i].value);
                break;

            case TOKEN_PIPE:
                cmd -> argv[cmd -> argc] = NULL;
                pipeline -> commands[pipeline -> count++] = cmd;
                cmd = calloc(1, sizeof(Command));
                cmd -> argv = malloc(MAX_ARGS * sizeof(char *));
                cmd -> argc = 0;
                break;

            case TOKEN_REDIR_IN:
                i++;
                cmd -> input_file = strdup(tokens[i].value);
                break;

            case TOKEN_REDIR_OUT:
                i++;
                cmd -> output_file = strdup(tokens[i].value);
                cmd -> append = 0;
                break;

            case TOKEN_REDIR_APPEND:
                i++;
                cmd -> output_file = strdup(tokens[i].value);
                cmd -> append = 1;
                break;

            case TOKEN_BACKGROUND:
                cmd -> background = 1;
                break;

            case TOKEN_EOF:
            default:
                break;
        }
    }


    cmd -> argv[cmd -> argc] = NULL;
    pipeline -> commands[pipeline -> count++] = cmd;
    return pipeline;
}

/*
 * ── FUNCTION 3: free_pipeline() ──────────────────────────────────────────
 *
 * PURPOSE
 *   Release every byte of heap memory owned by a Pipeline.
 *   Must be called after execute_pipeline() finishes with the Pipeline.
 *
 * ALGORITHM — implement exactly this logic:
 * ─────────────────────────────────────────
 *   if (pipeline == NULL) return;   ← always safe to call with NULL
 *
 *   for i = 0 .. pipeline->count - 1:
 *       cmd = pipeline->commands[i]
 *       if cmd == NULL: continue
 *
 *       Free each argument string:
 *           for j = 0 .. cmd->argc - 1:
 *               free(cmd->argv[j])
 *
 *       free(cmd->argv)          ← free the argv array itself
 *       free(cmd->input_file)    ← free() is a no-op on NULL, so no NULL check needed
 *       free(cmd->output_file)   ← same
 *       free(cmd)                ← free the Command struct
 *
 *   free(pipeline->commands)    ← free the array of Command pointers
 *   free(pipeline)              ← free the Pipeline struct
 *
 * WHY IS THIS ORDERING IMPORTANT?
 *   You must free the contents of a struct BEFORE freeing the struct itself.
 *   Once you call free(cmd), the memory is returned to the heap — accessing
 *   cmd->argv after that is undefined behavior (a "use-after-free" bug).
 *
 * NOTE ON free(NULL)
 *   The C standard guarantees that free(NULL) is a no-op — it does nothing
 *   and does not crash. So you don't need to NULL-check input_file or
 *   output_file before freeing them.
 */
void free_pipeline(Pipeline *pipeline) {
    /* TODO: implement following the algorithm above */
    if(!pipeline) return;

    for(int i = 0; i < pipeline -> count; i++){
        Command *cmd = pipeline->commands[i];
        if(!cmd) continue;
        for(int j = 0; j < cmd -> argc; j++)
            free(cmd -> argv[j]);
        free(cmd -> argv);
        free(cmd -> input_file);
        free(cmd -> output_file);
        free(cmd);
    }

    free(pipeline -> commands);
    free(pipeline);
}
