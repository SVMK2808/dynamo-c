#ifndef TERMINAL_H
#define TERMINAL_H

#include <stddef.h>

/* Reads a line in raw mode aupporting Up-Arrow and Tab autocomplete */
int read_line_raw(char *buffer, size_t max_len);

#endif /* TERMINAL_H */