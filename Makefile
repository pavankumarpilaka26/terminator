CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread
LDFLAGS = -pthread
LIBS = -lX11

TARGET = terminator
SOURCE = src/main.c

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(SOURCE) -o $(TARGET) $(LDFLAGS) $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
