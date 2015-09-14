AC_PREREQ([2.69])
AC_INIT([fuse-cdfs], [0.01])
AC_CONFIG_SRCDIR([cdfs-utils.h])
AC_CONFIG_HEADERS([config.h])

AM_MAINTAINER_MODE

AM_INIT_AUTOMAKE([foreign])

AC_SUBST(ACLOCAL_AMFLAGS, "-I m4")

# Checks for programs.
AC_PROG_CC
AC_PROG_LIBTOOL

# Checks for libraries.

# Checks for header files.
AC_CHECK_HEADERS([fcntl.h inttypes.h stddef.h stdlib.h string.h sys/param.h sys/time.h syslog.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_CHECK_HEADER_STDBOOL
AC_C_INLINE
AC_TYPE_OFF_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_CHECK_MEMBERS([struct stat.st_blksize])
AC_STRUCT_ST_BLOCKS
AC_TYPE_UINT64_T

# Checks for library functions.
AC_FUNC_MALLOC
AC_CHECK_FUNCS([clock_gettime dup2 memset mkdir strchr strdup])

MORE_CFLAGS="-Wall -std=gnu99 -D_FILE_OFFSET_BITS=64"
MORE_LIBS="-lpthread -lfuse -lrt -ldl -lcdio_cdda -lcdio_paranoia -lcdio -lsqlite3"

AC_SUBST(MORE_CFLAGS)
AC_SUBST(MORE_LIBS)

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
