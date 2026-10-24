#ifndef PARSER_H
#define PARSER_H

/*
 * parser.h — Public API for the parser module.
 *
 * Any file that needs to call parse_line() or free_pipeline()
 * just does: #include "parser.h"
 *
 * The internal helpers (tokenize, build_pipeline) are declared static
 * inside parser.c — they're implementation details, not public API.
 */

#include "shell.h"

/*
 * parse_line() — Convert a raw input string into a Pipeline.
 *
 * This is the single entry point into the parser. It orchestrates
 * two phases internally:
 *   Phase 1 → tokenize(input)      produces a flat Token array
 *   Phase 2 → build_pipeline(tokens) produces the Pipeline struct
 *
 * Parameters:
 *   input — the raw line read from the user (may be modified in-place
 *            by the tokenizer, so pass a mutable buffer, not a literal)
 *
 * Returns:
 *   A heap-allocated Pipeline on success.
 *   NULL if the input is empty, whitespace-only, or parsing fails.
 *
 * Ownership:
 *   The caller is responsible for calling free_pipeline() on the result.
 *   Failing to do so causes a memory leak on every command the user runs.
 */
Pipeline *parse_line(char *input);

/*
 * free_pipeline() — Deep-free a Pipeline and all memory it owns.
 *
 * Walks the entire data structure and calls free() on everything
 * that was heap-allocated during parsing:
 *   - Each string in each Command's argv[]
 *   - Each Command's argv array itself
 *   - Each Command's input_file and output_file strings
 *   - Each Command struct
 *   - The Pipeline's commands[] array
 *   - The Pipeline struct itself
 *
 * Parameters:
 *   pipeline — the Pipeline returned by parse_line(). Safe to pass NULL.
 */
void free_pipeline(Pipeline *pipeline);

#endif /* PARSER_H */
