SRCS = $(wildcard *.c)
OBJS = $(SRCS:%.c=%.o)
TARGET = libhfs.a

.PHONY: all install uninstall clean

all: $(TARGET)

$(TARGET): CFLAGS += $(LIBHFS_CFLAGS)
$(TARGET): $(OBJS)
	$(AR) rcs $@ $^
	$(RANLIB) $@

install: $(TARGET)
	mkdir -pm755 $(DESTDIR)$(includedir) $(DESTDIR)$(libdir)
	$(INSTALL) -m644 libhfs.h $(DESTDIR)$(includedir)
	$(INSTALL) -m644 $(TARGET) $(DESTDIR)$(libdir)

uninstall:
	$(RM) $(DESTDIR)$(includedir)/libhfs.h $(DESTDIR)$(libdir)/$(TARGET)

clean:
	$(RM) $(OBJS) $(TARGET)
