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
#include <sys/types.h>
#include <sys/stat.h>

#include <pthread.h>

#ifndef ENOATTR
#define ENOATTR ENODATA        /* No such attribute */
#endif

#include <sqlite3.h>

#include <fuse/fuse_lowlevel.h>

#include "logging.h"
#include "cdfs.h"
#include "entry-management.h"

#ifdef LOGGING
extern unsigned char loglevel;
#endif

pthread_mutex_t inodectrmutex=PTHREAD_MUTEX_INITIALIZER;

#ifdef SIZE_INODE_HASHTABLE
static size_t id_table_size=SIZE_INODE_HASHTABLE;
#else
static size_t id_table_size=32768;
#endif

#ifdef SIZE_NAME_HASHTABLE
static size_t name_table_size=SIZE_NAME_HASHTABLE;
#else
static size_t name_table_size=32768;
#endif

struct cdfs_inode_struct **inode_hash_table;
struct cdfs_entry_struct **name_hash_table;

extern long long inoctr;

//
// basic functions for adding and removing inodes and entries
//

int init_hashtables()
{
    int nreturn=0;

    inode_hash_table = calloc(id_table_size, sizeof(struct cdfs_entry_struct *));

    if ( ! inode_hash_table ) {

	nreturn=-ENOMEM;

    } else {

	name_hash_table = calloc(name_table_size, sizeof(struct cdfs_entry_struct *));

	if ( ! name_hash_table ) {

	    nreturn=-ENOMEM;
	    free(inode_hash_table);

	}

    }

    return nreturn;

}

static size_t inode_2_hash(fuse_ino_t inode)
{
	return inode % id_table_size;
}

static size_t name_2_hash(fuse_ino_t parent_inode, const char *name)
{
	uint64_t hash = parent_inode;

	for (; *name; name++) hash = hash * 31 + (unsigned char) *name;

	return hash % name_table_size;
}

void add_to_inode_hash_table(struct cdfs_inode_struct *inode)
{
	size_t idh = inode_2_hash(inode->ino);

	inode->id_next = inode_hash_table[idh];
	inode_hash_table[idh] = inode;
}


void add_to_name_hash_table(struct cdfs_entry_struct *entry)
{
	size_t tmphash = name_2_hash(entry->parent->inode->ino, entry->name);
	struct cdfs_entry_struct *next_entry = name_hash_table[tmphash];

	if (next_entry) {

	    next_entry->name_prev = entry;

	    entry->name_next = next_entry;
	    entry->name_prev = NULL;


	} else {

	    // there is no first, so this becomes the first, a circular list of one element

	    entry->name_next=NULL;
	    entry->name_prev=NULL;

	}

	// point to the new inserted entry.. well the pointer to it

	name_hash_table[tmphash] = entry;
	entry->namehash=tmphash;

}

void remove_entry_from_name_hash(struct cdfs_entry_struct *entry)
{
    struct cdfs_entry_struct *next_entry = entry->name_next;
    struct cdfs_entry_struct *prev_entry = entry->name_prev;

    if ( next_entry == NULL && prev_entry == NULL ) {

	// entry is the last one...no next and no prev

	name_hash_table[entry->namehash]=NULL;

    } else {

	// if removing the first one, shift the list pointer 

	if (name_hash_table[entry->namehash]==entry) {

	    if (next_entry) {

		name_hash_table[entry->namehash]=next_entry;

	    } else {

		name_hash_table[entry->namehash]=prev_entry;

	    }

	}

	if (next_entry) next_entry->name_prev=prev_entry;
	if (prev_entry) prev_entry->name_next=next_entry;

    }

    entry->name_prev = NULL;
    entry->name_next = NULL;
    entry->namehash = 0;

}


struct cdfs_inode_struct *find_inode(fuse_ino_t inode)
{
    struct cdfs_inode_struct *tmpinode = inode_hash_table[inode_2_hash(inode)];

    while (tmpinode && tmpinode->ino != inode) tmpinode = tmpinode->id_next;

    return tmpinode;

}


struct cdfs_entry_struct *find_entry(fuse_ino_t parent, const char *name)
{
    struct cdfs_entry_struct *tmpentry = name_hash_table[name_2_hash(parent, name)];

    while (tmpentry) {

	if (tmpentry->parent && tmpentry->parent->inode->ino == parent && strcmp(tmpentry->name, name) == 0) break;

	tmpentry = tmpentry->name_next;

    }

    return tmpentry;
}


static struct cdfs_inode_struct *create_inode()
{
    struct cdfs_inode_struct *inode=NULL;

    // make threadsafe

    pthread_mutex_lock(&inodectrmutex);

    inode = malloc(sizeof(struct cdfs_inode_struct));

    if (inode) {

	memset(inode, 0, sizeof(struct cdfs_inode_struct));
	inode->ino = inoctr;
	inode->nlookup = 0;

	inoctr++;

    }

    pthread_mutex_unlock(&inodectrmutex);

    return inode;

}

static void init_entry(struct cdfs_entry_struct *entry)
{
    entry->inode=NULL;
    entry->name=NULL;

    entry->parent=NULL;

    // hide an entry or not
    entry->hide=0;

    // to maintain a table overall
    entry->name_next=NULL;
    entry->name_prev=NULL;

    entry->type=ENTRY_TYPE_TEMPORARY;

}


struct cdfs_entry_struct *create_entry(struct cdfs_entry_struct *parent, const char *name, struct cdfs_inode_struct *inode)
{
    struct cdfs_entry_struct *entry;

    entry = malloc(sizeof(struct cdfs_entry_struct));

    if (entry) {

	memset(entry, 0, sizeof(struct cdfs_entry_struct));
	init_entry(entry);

	entry->name = strdup(name);

	if (!entry->name) {

	    free(entry);
	    entry = NULL;

	} else {

	    entry->parent = parent;

	    if (inode != NULL) {

		entry->inode = inode;
		inode->alias=entry;

	    }

	}

    }

    return entry;

}

void remove_entry(struct cdfs_entry_struct *entry)
{

    free(entry->name);

    // if there is a cached entry

    if ( entry->inode ) {

	entry->inode->alias=NULL;

    }

    free(entry);

}

void assign_inode(struct cdfs_entry_struct *entry)
{

    entry->inode=create_inode();

    if ( entry->inode ) {

	entry->inode->alias=entry;

    }

}

struct cdfs_entry_struct *new_entry(fuse_ino_t parent, const char *name)
{
    struct cdfs_entry_struct *entry;

    entry = create_entry(find_inode(parent)->alias, name, NULL);

    if ( entry ) {

	assign_inode(entry);

	if ( ! entry->inode ) {

	    remove_entry(entry);
	    entry=NULL;

	}

    }

    return entry;
}
