# Makefile for claude-c

CC ?= gcc
BASE_CFLAGS = -Wall -Wextra -O2 -std=c99 -D_POSIX_C_SOURCE=200809L
BASE_LDFLAGS =

# Auto-detect libcurl using pkg-config, with fallbacks
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LDFLAGS := $(shell pkg-config --libs libcurl 2>/dev/null)

# Fallback if pkg-config not available or fails
ifeq ($(CURL_LDFLAGS),)
    CURL_LDFLAGS = -lcurl
    # Check common include paths
    ifneq ($(wildcard /usr/include/curl/curl.h),)
        CURL_CFLAGS = -I/usr/include
    else ifneq ($(wildcard /usr/local/include/curl/curl.h),)
        CURL_CFLAGS = -I/usr/local/include
        CURL_LDFLAGS += -L/usr/local/lib
    else ifneq ($(wildcard /opt/homebrew/include/curl/curl.h),)
        # macOS Homebrew
        CURL_CFLAGS = -I/opt/homebrew/include
        CURL_LDFLAGS = -L/opt/homebrew/lib -lcurl
    endif
endif

# Auto-detect OpenSSL using pkg-config
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LDFLAGS := $(shell pkg-config --libs openssl 2>/dev/null)

# Fallback if pkg-config not available
ifeq ($(OPENSSL_LDFLAGS),)
    OPENSSL_LDFLAGS = -lssl -lcrypto
endif

CFLAGS = $(BASE_CFLAGS) $(CURL_CFLAGS) $(OPENSSL_CFLAGS)
LDFLAGS = $(BASE_LDFLAGS) $(CURL_LDFLAGS) $(OPENSSL_LDFLAGS)

SRCS = main.c api.c auth.c oauth.c stream.c json.c state.c image.c
OBJS = $(SRCS:.c=.o)
TARGET = claude-c

# Default target
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Dependencies
main.o: main.c api.h json.h image.h oauth.h
api.o: api.c api.h auth.h json.h stream.h state.h
auth.o: auth.c auth.h json.h
oauth.o: oauth.c oauth.h auth.h json.h
stream.o: stream.c stream.h json.h
json.o: json.c json.h
state.o: state.c state.h json.h
image.o: image.c image.h

# Debug build
debug: CFLAGS += -g -DDEBUG
debug: clean $(TARGET)

# Install to /usr/local/bin
install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

# Uninstall
uninstall:
	rm -f /usr/local/bin/$(TARGET)

# Clean build artifacts
clean:
	rm -f $(OBJS) $(TARGET)

# Run a quick test
test: $(TARGET)
	@echo "Testing with simple query..."
	@echo "Hello" | ./$(TARGET) -p -s "Reply with exactly: OK"

.PHONY: all clean debug install uninstall test
