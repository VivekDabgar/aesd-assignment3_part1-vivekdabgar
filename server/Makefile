# Makefile for AESD Assignment 5 - aesdsocket
#
# Native build:
#     make
# Cross build (example, aarch64 Buildroot toolchain):
#     make CROSS_COMPILE=aarch64-none-linux-gnu-
# Clean:
#     make clean

CROSS_COMPILE ?=
# CC predefined variable
CC             = $(CROSS_COMPILE)gcc
CFLAGS         = -Wall -Werror -g
LDFLAGS        =

TARGET := aesdsocket
SRCS   := aesdsocket.c
OBJS   := $(SRCS:.c=.o)

.PHONY: all default clean

all: $(TARGET)

default: all

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	-rm -f $(TARGET) $(OBJS)