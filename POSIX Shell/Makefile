# ─────────────────────────────────────────────────────────────────────────────
# Makefile — POSIX Shell Project
# ─────────────────────────────────────────────────────────────────────────────
#
# VARIABLE GLOSSARY
# ─────────────────
# CC      : the compiler binary to invoke
# CFLAGS  : compiler flags
#             -Wall    → enable all standard warnings (catch common bugs)
#             -Wextra  → enable extra warnings beyond -Wall
#             -g       → embed debug symbols (lets gdb show line numbers)
#             -I include/ → add include/ to the header search path so that
#                           #include "shell.h" works from any .c file
# TARGET  : name of the final linked executable
# SRC_DIR : directory containing .c source files
# SRCS    : explicit list of every .c file in the project
# OBJS    : object file list — derived from SRCS by replacing .c → .o
#           $(SRCS:.c=.o) is Make's substitution reference syntax
# ─────────────────────────────────────────────────────────────────────────────

CC      = gcc
CFLAGS  = -Wall -Wextra -g -I include/ -I ../include/ -pthread
TARGET  = shell

SRC_DIR     = src
BUILTIN_DIR = src/builtins

# Add new .c files here as you create them
SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/parser.c \
       $(SRC_DIR)/executor.c \
       $(SRC_DIR)/signals.c \
       $(SRC_DIR)/terminal.c \
       $(BUILTIN_DIR)/cmd_pwd.c \
       $(BUILTIN_DIR)/cmd_echo.c \
       $(BUILTIN_DIR)/cmd_cd.c \
       $(BUILTIN_DIR)/cmd_ls.c \
       $(BUILTIN_DIR)/cmd_exit.c \
       $(BUILTIN_DIR)/cmd_search.c \
       $(BUILTIN_DIR)/cmd_pinfo.c \
       $(BUILTIN_DIR)/cmd_history.c \
       $(BUILTIN_DIR)/cmd_dynamo.c \
       $(BUILTIN_DIR)/builtin.c \
       ../src/network.c \
       ../src/protocol.c \
       ../src/vector_clock.c

# We'll uncomment these lines as we build each module:
#      $(SRC_DIR)/executor.c \
#      $(SRC_DIR)/signals.c \
#      $(BUILTIN_DIR)/cmd_echo.c \
#      $(BUILTIN_DIR)/cmd_ls.c \
#      $(BUILTIN_DIR)/cmd_cd.c

OBJS = $(SRCS:.c=.o)

# ── Default target ────────────────────────────────────────────────────────────
# 'make' with no arguments builds this first target.
all: $(TARGET)

# ── Link step ─────────────────────────────────────────────────────────────────
# Combines all .o files into the final executable.
# $@ = the target name (shell), $^ = all prerequisites (all .o files)
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^
	@rm -f $(OBJS)
	@echo "Build complete → $(TARGET)  (.o files removed)"

# ── Compile step (pattern rule) ───────────────────────────────────────────────
# For ANY .c file anywhere in the tree, compile it to a .o file beside it.
# $< = the first (and only) prerequisite (the .c file)
# $@ = the target (the .o file)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ── Clean ─────────────────────────────────────────────────────────────────────
# Remove all build artifacts. Run 'make clean' to start fresh.
clean:
	rm -f $(OBJS) $(TARGET)

# .PHONY tells Make that 'all' and 'clean' are not real files.
# Without this, if a file named 'clean' ever existed, Make would skip the rule.
.PHONY: all clean
