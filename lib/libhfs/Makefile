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
	mkdir -pm755 $(DESTDIR)$(includedir) $(DESTDIR)$(libdir)
	$(INSTALL) -m644 libhfs.h $(DESTDIR)$(includedir)
	$(INSTALL) -m644 $(TARGET) $(DESTDIR)$(libdir)

uninstall:
	rm -f $(DESTDIR)$(includedir)/libhfs.h $(DESTDIR)$(libdir)/$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
