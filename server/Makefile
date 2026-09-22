CC ?= gcc
CFLAGS ?= -g -Wall -Werror -Wextra -pthread
LDFLAGS ?= -pthread

CROSS_COMPILE ?=
CC := $(CROSS_COMPILE)$(CC)

TARGET := aesdsocket
SRCS := aesdsocket.c
OBJS := $(SRCS:.c=.o)

.PHONY: all clean default

all: default

default: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
