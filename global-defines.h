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

#define FUSE_USE_VERSION 26
#define _REENTRANT
#define _GNU_SOURCE
#define _XOPEN_SOURCE 500

#ifdef HAVE_CONFIG_H
#include <config.h>
#else
#define PACKAGE_VERSION "1.0"
#define HAVE_SETXATTR
#endif

#define MODEMASK 07777

#define SIZE_INODE_HASHTABLE 4096
#define SIZE_NAME_HASHTABLE 4096

#define SIZE_RIFFHEADER         44

#ifndef FUSE_SET_ATTR_ATIME_NOW
#define FUSE_SET_ATTR_ATIME_NOW (1 << 7)
#endif

#ifndef FUSE_SET_ATTR_MTIME_NOW
#define FUSE_SET_ATTR_MTIME_NOW (1 << 8)
#endif

#ifndef SIZE_INODE_HASHTABLE
#define SIZE_INODE_HASHTABLE 32768;
#endif

#ifndef SIZE_NAME_HASHTABLE
#define SIZE_NAME_HASHTABLE 32768;
#endif

#define SMALL_PATH_MAX  	64
#define LINE_MAXLEN 		64

#define ENTRY_TYPE_NORMAL			0
#define ENTRY_TYPE_ROOT				1
#define ENTRY_TYPE_LINK				2
#define ENTRY_TYPE_TEMPORARY			6
#define ENTRY_TYPE_SYSTEM			7


#define LOGGING


#define READAHEAD_POLICY_NONE                   0
#define READAHEAD_POLICY_PIECE                  1
#define READAHEAD_POLICY_WHOLE                  2

#define READAHEAD_POLICY_PIECE_SECTORS          750
#define READAHEAD_POLICY_WHOLE_SECTORS          250

#define CDFS_TRACK_BEGIN_SIZE                   250


#define CDFS_CACHE_ADMIN_BACKEND_INTERNAL                   0
#define CDFS_CACHE_ADMIN_BACKEND_SQLITE                     1

#define CDFS_CACHE_ADMIN_BACKEND_NONE                       CDFS_CACHE_ADMIN_BACKEND_INTERNAL

