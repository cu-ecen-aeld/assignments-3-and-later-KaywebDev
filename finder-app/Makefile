# Makefile for the “writer” application

CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Wextra -O2

SRC = writer.c
OBJS = $(SRC:.c=.o)
TARGET = writer

.PHONY: all clean

all: $(TARGET)
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^
clean:
	rm -f $(TARGET) $(OBJS)