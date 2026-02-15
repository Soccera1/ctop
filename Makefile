# ctop Makefile
# A simple system resource monitor in C

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c99
LDFLAGS = 
TARGET = ctop
SRC = ctop.c

# Detect OS for potential platform-specific flags
UNAME_S := $(shell uname -s)

# Installation prefix (default: /usr/local)
PREFIX ?= /usr/local
INSTALL_DIR = $(PREFIX)/bin

# Linux-specific flags
ifeq ($(UNAME_S),Linux)
    LDFLAGS += -lm
endif

# macOS-specific flags
ifeq ($(UNAME_S),Darwin)
    CFLAGS += -D_DARWIN_C_SOURCE
endif

.PHONY: all clean install debug

all: $(TARGET)

$(TARGET): $(SRC) termbox2.h
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

debug: CFLAGS += -g -DDEBUG
debug: $(TARGET)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -d $(INSTALL_DIR)
	install -m 755 $(TARGET) $(INSTALL_DIR)/

uninstall:
	rm -f $(INSTALL_DIR)/$(TARGET)
