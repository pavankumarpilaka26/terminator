CC := gcc
TARGET := terminator
SOURCE := src/main.c

CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread
LDFLAGS := -pthread
LDLIBS := -lX11

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(SOURCE) -o $@ $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGET)
