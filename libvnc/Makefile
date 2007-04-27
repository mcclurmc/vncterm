TARGET = libvnc
MAJOR = 1.0
MINOR = 0

OBJS := 
OBJS += d3des.o
OBJS += vnc.o

OBJS_opic = $(patsubst %.o,%.opic,$(OBJS))

XEN_ARCH := x86
XEN_TARGET := xen

CFLAGS  = -I$(shell pwd)/include
# _GNU_SOURCE for asprintf.
CFLAGS += -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_GNU_SOURCE
CFLAGS += -Wall -Werror -g -O1

ifdef LOCAL
CFLAGS	+= -DVNCTERM_BIOS_DIR='"."'
endif

# Get gcc to generate the dependencies for us.
CFLAGS   += -Wp,-MD,$(@D)/.$(@F).d
DEPS     = .*.d

LDFLAGS := -g

SHLIB_CFLAGS = -shared
SONAME_LDFLAG = -soname

#all: $(TARGET).so $(TARGET).a
all: $(TARGET).a

$(TARGET).so: $(TARGET).so.$(MAJOR)
	ln -sf $< $@
$(TARGET).so.$(MAJOR): $(TARGET).so.$(MAJOR).$(MINOR)
	ln -sf $< $@

$(TARGET).so.$(MAJOR).$(MINOR): $(OBJS_opic)
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,$(SONAME_LDFLAG) -Wl,$(TARGET).so.$(MAJOR) $(SHLIB_CFLAGS) -o $@ $^

$(TARGET).a: $(OBJS)
	$(AR) rcs $(TARGET).a $^

%.opic: %.c
	$(CC) $(CPPFLAGS) -DPIC $(CFLAGS) -fPIC -c -o $@ $<

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(OBJS_opic)
	rm -f $(DEPS)
	rm -f $(TARGET).so.$(MAJOR).$(MINOR) $(TARGET).so.$(MAJOR)
	rm -f $(TARGET).so $(TARGET).a
	rm -f TAGS

-include $(DEPS)
