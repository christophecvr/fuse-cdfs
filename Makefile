OBJS = cdfs-utils.o cdfs-cdromutils.o cdfs-options.o cdfs-xattr.o cdfs-cache.o fuse-loop-epoll-mt.o entry-management.o cdfs.o
EXECUTABLE = fuse-cdfs

CC=gcc
CFLAGS = -Wall -std=gnu99 -O3 -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse -lpthread -lfuse -lrt -ldl -lcdio_paranoia -lcdio_cdda -lcdio -lsqlite3

LDFLAGS = $(CFLAGS)
COMPILE = $(CC) $(CFLAGS) -c
LINKCC = $(CC)


all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJS)
	$(LINKCC) $(LDFLAGS) -o $(EXECUTABLE) $(OBJS)

fuse-loop-epoll-mt.o: fuse-loop-epoll-mt.h logging.h
cdfs-utils.o: cdfs-utils.h logging.h
cdfs-cdromutils.o: cdfs-cdromutils.h logging.h
cdfs.o: cdfs-utils.h cdfs-options.h cdfs-xattr.h fuse-loop-epoll-mt.h entry-management.h logging.h cdfs.h global-defines.h
cdfs-options.o: cdfs-options.h logging.h global-defines.h
cdfs-xattr.o: cdfs-xattr.h logging.h cdfs.h global-defines.h
cdfs-cache.o: cdfs-cache.h logging.h cdfs.h global-defines.h

entry-management.o: entry-management.h logging.h global-defines.h

%.o: %.c 
	$(COMPILE) -o $@ $<


clean:
	rm -f $(OBJS) $(EXECUTABLE) *~
