CC      = gcc
CFLAGS  = -Wall -Wextra -std=c99 -pedantic -g
TARGET  = mysh

all: $(TARGET)

$(TARGET): mysh.c
	$(CC) $(CFLAGS) -o $(TARGET) mysh.c

clean:
	rm -f $(TARGET)

test: $(TARGET)
	@echo "=== Running tests ==="
	@bash test.sh

.PHONY: all clean test
