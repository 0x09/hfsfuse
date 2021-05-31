SRCS = $(wildcard *.c)
OBJS = $(SRCS:%.c=%.o)
TARGET = libhfs.a

.PHONY: all install uninstall clean

all: $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) $(LIBHFS_CFLAGS) -c -o $*.o $^

$(TARGET): $(OBJS)
	$(AR) rcs $@ $^
	$(RANLIB) $@

install: $(TARGET)
	$(INSTALL) libhfs.h $(PREFIX)/include/
	$(INSTALL) $(TARGET) $(PREFIX)/lib/

uninstall:
	rm -f $(PREFIX)/include/libhfs.h
	rm -f $(PREFIX)/lib/$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
