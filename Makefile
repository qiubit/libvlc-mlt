CFLAGS += -I../..

LDFLAGS += -L../../framework -lmlt -lpthread

include ../../../config.mak

TARGET = ../libmltlibvlc$(LIBSUF)

OBJS = factory.o \
	   producer_libvlc.o \
	   consumer_libvlc.o \
	   frame_cache.o \
	   buffer_queue.o

CFLAGS += $(shell pkg-config libvlc --cflags)

LDFLAGS += $(shell pkg-config libvlc --libs)

SRCS := $(OBJS:.o=.c)

all: 	$(TARGET)

$(TARGET): $(OBJS)
		$(CC) $(SHFLAGS) -o $@ $(OBJS) $(LDFLAGS)

depend:	$(SRCS)
		$(CC) -MM $(CFLAGS) $^ 1>.depend

distclean:	clean
		rm -f .depend

clean:
		rm -f $(OBJS) $(TARGET)

install: all
	install -m 755 $(TARGET) "$(DESTDIR)$(moduledir)"
	install -d "$(DESTDIR)$(mltdatadir)/libvlc"

ifneq ($(wildcard .depend),)
include .depend
endif
