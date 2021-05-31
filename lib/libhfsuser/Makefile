SRCS = $(wildcard *.c)
OBJS = $(SRCS:%.c=%.o)
TARGET = libhfsuser.a

.PHONY: all install uninstall clean

all: $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) $(LOCAL_CFLAGS) $(INCLUDE) $(APP_FLAGS) -c -o $*.o $^

$(TARGET): $(OBJS)
	$(AR) rcs $@ $^
	$(RANLIB) $@

install: $(TARGET)
	$(INSTALL) hfsuser.h $(PREFIX)/include/
	$(INSTALL) $(TARGET) $(PREFIX)/lib/

uninstall:
	rm -f $(PREFIX)/include/hfsuser.h
	rm -f $(PREFIX)/lib/$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
