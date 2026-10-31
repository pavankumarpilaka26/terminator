CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2

TARGET = terminator
SOURCE = src/main.c
LIBS = -lX11

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(SOURCE) -o $(TARGET) $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean