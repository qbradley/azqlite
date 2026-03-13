#
# Makefile for Azure Blob Storage REST API Proof of Concept
#
# Build: make
# Run:   ./azure_poc
# Clean: make clean
#
# Dependencies:
#   macOS:  brew install openssl curl  (or use system libcurl)
#   Linux:  apt-get install libcurl4-openssl-dev libssl-dev
#

CC = cc
CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -g -O0

# OpenSSL — prefer Homebrew on macOS (system LibreSSL lacks some features)
OPENSSL_PREFIX ?= $(shell brew --prefix openssl 2>/dev/null || echo /usr)
CFLAGS  += -I$(OPENSSL_PREFIX)/include
LDFLAGS += -L$(OPENSSL_PREFIX)/lib

# libcurl
CURL_PREFIX ?= $(shell brew --prefix curl 2>/dev/null || echo /usr)
CFLAGS  += -I$(CURL_PREFIX)/include
LDFLAGS += -L$(CURL_PREFIX)/lib

LDLIBS = -lcurl -lssl -lcrypto

# Source files
SRCS = azure_auth.c azure_blob.c azure_error.c azure_poc_main.c
OBJS = $(SRCS:.c=.o)
TARGET = azure_poc

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c azure_blob.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

# Run the PoC
run: $(TARGET)
	./$(TARGET)
