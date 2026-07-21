# Cross-compilation default set up
CROSS_COMPILE ?=
CC = $(CROSS_COMPILE)gcc

# Source and object file dsefinitions
SRCS = writer.c
OBJS = $(SRCS:.c=.o)
TARGET = writer

# Default target: build the app
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $(TARGET) $(OBJS)

%.o: %.c
	$(CC) -c $< -o $@

# clean target: remove binary and object files
clean:
	rm -f $(TARGET) $(OBJS)
