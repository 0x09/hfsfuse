AR ?= ar
RANLIB ?= ranlib

SRCS = $(wildcard *.c)
OBJS = $(SRCS:%.c=%.o)
TARGET = libhfs.a

.PHONY: all amalgamtion clean

all: $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $*.o $^

$(TARGET): $(OBJS)
	$(AR) rcs $@ $^
	$(RANLIB) $@

clean:
	rm -f $(OBJS) $(TARGET)
