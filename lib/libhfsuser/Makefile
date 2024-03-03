SRCS = $(wildcard *.c)
OBJS = $(SRCS:%.c=%.o)
TARGET = libhfsuser.a

.PHONY: all install uninstall clean

all: $(TARGET)

$(TARGET): CPPFLAGS += $(LOCAL_CFLAGS) $(INCLUDE) $(APP_FLAGS)
$(TARGET): $(OBJS)
	$(AR) rcs $@ $^
	$(RANLIB) $@

install: $(TARGET)
	mkdir -pm755 $(DESTDIR)$(includedir) $(DESTDIR)$(libdir)
	$(INSTALL) -m644 hfsuser.h $(DESTDIR)$(includedir)
	$(INSTALL) -m644 $(TARGET) $(DESTDIR)$(libdir)

uninstall:
	$(RM) $(DESTDIR)$(includedir)/hfsuser.h $(DESTDIR)$(libdir)/$(TARGET)

clean:
	$(RM) $(OBJS) $(TARGET)
