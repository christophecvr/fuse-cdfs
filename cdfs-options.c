/*
 
  2010, 2011 Stef Bon <stefbon@gmail.com>

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "global-defines.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>

#include <inttypes.h>
#include <ctype.h>

#include <sys/stat.h>
#include <sys/param.h>

#include <fuse/fuse_lowlevel.h>

#include "cdfs-options.h"

static void usage(const char *progname, FILE *f)
{
	fprintf(f, "usage: %s [opts] -o cache-directory=DIR\n",
		"             device=DEVICE\n",
		"             progressfifo=FILE\n",
	        "             [logging=NR,]\n",
	        "             --cachebackend=none[default],sqlite\n",
	        "             --readaheadpolicy=none/piece/whole\n",
	        "             --hashprogram=[prog]\n",
	        "             --discid=FILE\n",
		progname);
}


int cdfs_options_output_proc(void *data, const char *arg, int key, struct fuse_args *outargs)
{
	(void) arg;
	(void) data;

	switch (key) {
	case KEY_HELP:
		usage(outargs->argv[0], stdout);
		printf(
		"General options:\n"
		"    -o opt,[opt...]        mount options\n"
		"    -h   --help            print help\n"
		"    -V   --version         print version\n"
		"    -d   -o debug          enable debug output (implies -f)\n"
		"    -f                     foreground operation\n"
		"\n"
		"cdfs options:\n"
		"    -o device=DEVICE                      	device to use (like /dev/sr0)\n"
		"    -o cache-directory=DIR                     directory to store cached files\n"
		"    -o progressfifo=FILE                       fifo ro write cdrom read progress to\n"
		"    -o logging=NUMBER                          set loglevel (0=no logging)\n"
		"    -o cachebackend=none[default],sqlite       backend to store cache info\n"
		"    -o readaheadpolicy=none/piece/whole        policy for readahead\n"
		"    -o hashprogram=md5sum/sha1sum/..           program to compute hash, default md5sum\n"
		"    -o discid                                  path write the discid to, default cache-directory\n"
		"\n"
		"FUSE options:\n");
		fflush(stdout);
		dup2(1, 2);
		fuse_opt_add_arg(outargs, "--help");
		fuse_mount(NULL, outargs);
		fuse_lowlevel_new(outargs, NULL, 0, NULL);
		exit(0);
	case KEY_VERSION:
		printf("xmpfs version %s\n", PACKAGE_VERSION);
		fflush(stdout);
		dup2(1, 2);
		fuse_opt_add_arg(outargs, "--version");
		fuse_parse_cmdline(outargs, NULL, NULL, NULL);
		fuse_lowlevel_new(outargs, NULL, 0, NULL);
		exit(0);
	}
	return 1;
}

