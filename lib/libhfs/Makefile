SRCS = $(wildcard *.c)
OBJS = $(SRCS:%.c=%.o)
DEPS = $(SRCS:%.c=%.d)
TARGET = libhfs.a

CPPFLAGS += -MMD -MP

.PHONY: all install uninstall clean

all: $(TARGET)

%.o: CPPFLAGS += "-D__KERNEL_RCSID(sec,string)=const char hfs_rcsid_$*[]=string"

# from NetBSD syssrc usr/src/sys/conf/Makefile.kern.inc v 1.300
LIBHFS_UPSTREAM_CFLAGS = -Wall -Wextra -Wno-unknown-warning-option\
    -Wpointer-arith -Wstrict-prototypes -Wold-style-definition -Wswitch -Wshadow -Wcast-qual -Wwrite-strings -Walloca -Wold-style-definition -Wmissing-prototypes -Wstack-usage=3584\
    -Wno-unused-parameter -Wno-sign-compare -Wno-conversion -Wno-self-assign\
    -fno-common -fno-delete-null-pointer-checks -fno-strict-aliasing\
    -std=gnu99 -O2

$(TARGET): CFLAGS := $(LIBHFS_UPSTREAM_CFLAGS) $(CFLAGS) $(LIBHFS_CFLAGS)
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
	$(RM) $(OBJS) $(DEPS) $(TARGET)

-include $(DEPS)
