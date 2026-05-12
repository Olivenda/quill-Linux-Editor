CC      := gcc
CFLAGS  := -std=c99 -Wall -Wextra -pedantic \
           -fstack-protector-strong -D_FORTIFY_SOURCE=2
LDFLAGS := -lncurses

SRC_DIR := src
BIN     := quill
OBJS    := $(SRC_DIR)/editor.o

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(BIN)

.PHONY: all clean
