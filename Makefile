CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c99
TARGET = touchpad-monitor
SRC = main.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

debug: CFLAGS += -g -O0
debug: clean all

release: CFLAGS += -O2 -DNDEBUG
release: clean all

clean:
	rm -f $(TARGET) *.o

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

.PHONY: all clean debug release install
