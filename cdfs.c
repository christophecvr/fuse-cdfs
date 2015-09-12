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
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <err.h>
#include <sys/time.h>
#include <assert.h>
#include <syslog.h>
#include <time.h>

#include <inttypes.h>

// required??

#include <ctype.h>

#include <sys/types.h>
#include <pthread.h>

#include <sqlite3.h>


#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#ifndef ENOATTR
#define ENOATTR ENODATA        /* No such attribute */
#endif



#include "logging.h"
#include "cdfs.h"

#include "cdfs-utils.h"

#include "fuse-loop-epoll-mt.h"

#include "entry-management.h"

#include "cdfs-options.h"
#include "cdfs-xattr.h"
#include "cdfs-cache.h"
#include "cdfs-cdromutils.h"



struct cdfs_commandline_options_struct cdfs_commandline_options;
struct cdfs_options_struct cdfs_options;

struct fuse_opt cdfs_help_options[] = {
     CDFS_OPT("--device=%s",			device, 0),
     CDFS_OPT("device=%s",			device, 0),
     CDFS_OPT("--cache-directory=%s",		cache_directory, 0),
     CDFS_OPT("cache-directory=%s",		cache_directory, 0),
     CDFS_OPT("--progressfifo=%s",		progressfifo, 0),
     CDFS_OPT("progressfifo=%s",		progressfifo, 0),
     CDFS_OPT("logging=%i",			logging, 0),
     CDFS_OPT("--logging=%i",			logging, 0),
     CDFS_OPT("--cachebackend=%s",		cachebackend, 0),
     CDFS_OPT("cachebackend=%s",		cachebackend, 0),
     CDFS_OPT("--hashprogram=%s",		hashprogram, 0),
     CDFS_OPT("hashprogram=%s",		        hashprogram, 0),
     CDFS_OPT("--discid=%s",		        discid, 0),
     CDFS_OPT("discid=%s",		        discid, 0),
     CDFS_OPT("--readaheadpolicy=%s",		readaheadpolicy, 0),
     CDFS_OPT("readaheadpolicy=%s",		readaheadpolicy, 0),
     FUSE_OPT_KEY("-V",            		KEY_VERSION),
     FUSE_OPT_KEY("--version",      		KEY_VERSION),
     FUSE_OPT_KEY("-h",             		KEY_HELP),
     FUSE_OPT_KEY("--help",         		KEY_HELP),
     FUSE_OPT_END
};

struct cdfs_device_struct cdfs_device;

static struct fuse_chan *cdfs_chan;
struct cdfs_entry_struct *root_entry;

unsigned long long inoctr = FUSE_ROOT_ID;
unsigned long long nrinodes = 0;

unsigned char loglevel=0;


static void cdfs_lookup(fuse_req_t req, fuse_ino_t parentino, const char *name)
{
    struct fuse_entry_param e;
    struct cdfs_entry_struct *entry;
    struct cdfs_entry_struct *pentry;
    struct cdfs_inode_struct *pinode;
    int nreturn=0;
    unsigned char entrycreated=0;

    logoutput2("LOOKUP, name: %s", name);

    entry=find_entry(parentino, name);

    if ( ! entry ) {

	pinode=find_inode(parentino);

	if ( pinode ) {

	    pentry=pinode->alias;

	    // create the entry, assign the inode only when successfull

	    entry=create_entry(pentry, name, NULL);
	    if ( ! entry ) {

		nreturn=-ENOMEM;
		goto out;

	    }

	} else {

	    nreturn=-ENOENT;
	    goto out;

	}

	entrycreated=1;

    }

    nreturn=stat_track(entry, &(e.attr));

    if ( nreturn==-ENOENT) {

	logoutput2("lookup: entry does not exist (ENOENT)");

	if ( entrycreated==0 ) remove_entry_from_name_hash(entry);

	remove_entry(entry);

	e.ino = 0;
	e.entry_timeout = cdfs_options.negative_timeout;

    } else if ( nreturn<0 ) {

	logoutput1("do_lookup: error (%i)", nreturn);

	// some correction/action here?

    } else {

	// no error

	if ( entrycreated==1 ) {

	    assign_inode(entry);
	    add_to_inode_hash_table(entry->inode);
	    add_to_name_hash_table(entry);

	    if ( entry->type==ENTRY_TYPE_TEMPORARY ) entry->type=ENTRY_TYPE_NORMAL;

	}

	entry->inode->nlookup++;
	e.ino = entry->inode->ino;
	e.attr.st_ino = e.ino;
	e.generation = 0;
	e.attr_timeout = cdfs_options.attr_timeout;
	e.entry_timeout = cdfs_options.entry_timeout;

	logoutput2("lookup return size: %zi", e.attr.st_size);

    }

    out:

    logoutput1("lookup: return %i", nreturn);

    if ( nreturn<0 ) {

	fuse_reply_err(req, -nreturn);

    } else {

        fuse_reply_entry(req, &e);

    }



}


static void cdfs_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup)
{
    struct cdfs_inode_struct *inode;

    inode = find_inode(ino);

    if ( ! inode ) goto out;

    logoutput0("FORGET");

    if ( inode->nlookup < nlookup ) {

	logoutput0("internal error: forget ino=%llu %llu from %llu", (unsigned long long) ino, (unsigned long long) nlookup, (unsigned long long) inode->nlookup);

    }

    inode->nlookup -= nlookup;

    logoutput2("forget, current nlookup value %llu", (unsigned long long) inode->nlookup);

    out:

    fuse_reply_none(req);

}

static void cdfs_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct stat st;
    struct cdfs_entry_struct *entry;
    struct cdfs_inode_struct *inode;
    int nreturn=0;

    //
    // fi not used yet here... 
    // otherwise the fi->fd could be used for getting the attr
    //

    logoutput0("GETATTR");

    if ( fi ) {

	logoutput2("fi not empty..");

    }

    // get the inode and the entry, they have to exist

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=inode->alias;

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    nreturn=stat_track(entry, &st);

    out:

    logoutput1("getattr, return: %i", nreturn);

    if (nreturn < 0) {

	fuse_reply_err(req, -nreturn);

    } else {

	fuse_reply_attr(req, &st, cdfs_options.attr_timeout);

    }

}

//
// determine the stat given a direntry
// possibly the corresponding entry does not exist, if so create it here
// return:
// 0 no error, just display the entry
// 1 no error, hide the entry
// negative: error
//

static int get_direntry_stat(struct cdfs_generic_dirp_struct *dirp)
{
    int nreturn=0;
    bool entrycreated=false;
    char *name;


    dirp->st.st_mode = dirp->direntry.d_type << 12;
    name=dirp->direntry.d_name;

    logoutput2("get_direntry_stat, name: %s", name);

    if (strcmp(name, ".") == 0) {

	dirp->st.st_ino = dirp->generic_fh.entry->inode->ino;

    } else if (strcmp(name, "..") == 0) {

	if (dirp->generic_fh.entry->type == ENTRY_TYPE_ROOT ) {

	    dirp->st.st_ino = FUSE_ROOT_ID;

	} else {

	    dirp->st.st_ino = dirp->generic_fh.entry->parent->inode->ino;

	}

    } else  {

	//
	// "normal entry": look there is already an entry for this, a leftover from the previous batch
	//

	if ( dirp->entry ) {

	    // check the current dirp->entry is the right one

	    if ( strcmp(dirp->entry->name, name)!=0 ) dirp->entry=NULL;

	}

	if ( ! dirp->entry ) dirp->entry = find_entry(dirp->generic_fh.entry->inode->ino, name);

	if ( ! dirp->entry) {

	    // entry is not found: create a new one

	    dirp->entry = new_entry(dirp->generic_fh.entry->inode->ino, name);

	    if ( ! dirp->entry ) {

		nreturn=-ENOMEM;
		goto out;

	    }

	    entrycreated=true;

	}

	if ( ! dirp->entry->inode ) {

	    logoutput0("get_direntry_stat, inode not attached");
	    nreturn=-ENOENT;

	}

	dirp->st.st_ino = dirp->entry->inode->ino;

	if ( entrycreated ) {

	    add_to_inode_hash_table(dirp->entry->inode);
	    add_to_name_hash_table(dirp->entry);

	    if ( dirp->entry->type==ENTRY_TYPE_TEMPORARY ) dirp->entry->type=ENTRY_TYPE_NORMAL;

	}

    }

    out:

    logoutput2("get_direntry_stat: return %i", nreturn);

    return nreturn;

}


static inline struct cdfs_generic_dirp_struct *get_dirp(struct fuse_file_info *fi)
{
    return (struct cdfs_generic_dirp_struct *) (uintptr_t) fi->fh;
}

static void free_dirp(struct cdfs_generic_dirp_struct *dirp)
{

    free(dirp);

}

//
// open a directory to read the contents
// here the backend is an audio cdrom, with only one root directory, and
// no subdirectories
//
// so in practice this will be called only for the root
// it's not required to build an extra check we're in the root, the
// VFS will never allow this function to be called on something else than a directory
//

static void cdfs_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct cdfs_generic_dirp_struct *dirp=NULL;
    int nreturn=0;
    struct cdfs_entry_struct *entry;;
    struct cdfs_inode_struct *inode;

    logoutput0("OPENDIR");

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=inode->alias;

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }


    dirp = malloc(sizeof(struct cdfs_generic_dirp_struct));

    if ( ! dirp ) {

	nreturn=-ENOMEM;
	goto out;

    }

    memset(dirp, 0, sizeof(struct cdfs_generic_dirp_struct));

    dirp->entry=NULL;
    dirp->upperfs_offset=0;
    dirp->underfs_offset=1; /* start at 1: first track */

    dirp->generic_fh.entry=entry;

    dirp->direntry.d_name[0]='\0';
    dirp->direntry.d_type=0;

    // assign this object to fi->fh

    fi->fh = (unsigned long) dirp;


    out:

    if ( nreturn<0 ) {

	if ( dirp ) free_dirp(dirp);

	fuse_reply_err(req, -nreturn);

    } else {

	fuse_reply_open(req, fi);

    }

    logoutput1("opendir, nreturn %i", nreturn);

}

//
// reads a "direntry" from the cdrom
// simply look at the underfs_offset to determine the track
// note: the direntry should already be allocated, and part
// of dirp (and thus not to be freed!)
//
// the first 1 <=...<= cdda_tracks(cdd) are just the tracks
// cdda_tracks(cdd)+1 is the .
// cdda_tracks(cdd)+2 is the ..
//
// return: 1 success, 0 end (nothing for error)

static int readdir_cdrom(struct cdfs_generic_dirp_struct *dirp)
{
    int nreturn=0;

    if ( dirp->underfs_offset <= cdfs_device.nrtracks ) {

	dirp->direntry.d_type=DT_REG;
	sprintf(dirp->direntry.d_name, "track-%.2d.wav", dirp->underfs_offset);

	nreturn=1;

    } else if ( dirp->underfs_offset==cdfs_device.nrtracks+1 ) {

	dirp->direntry.d_type=DT_DIR;
	sprintf(dirp->direntry.d_name, ".");

	nreturn=1;

    } else if ( dirp->underfs_offset==cdfs_device.nrtracks+2 ) {

	dirp->direntry.d_type=DT_DIR;
	sprintf(dirp->direntry.d_name, "..");

	nreturn=1;

    }

    dirp->underfs_offset+=nreturn;

    return nreturn;

}


static int do_readdir_localhost(fuse_req_t req, char *buf, size_t size, off_t upperfs_offset, struct cdfs_generic_dirp_struct *dirp)
{
    size_t bufpos = 0;
    int res, nreturn=0;
    size_t entsize;
    bool validentryfound=false;
    bool direntryfrompreviousbatch=false;

    logoutput0("DO_READDIR, offset: %"PRId64, upperfs_offset);

    dirp->upperfs_offset=upperfs_offset;
    if ( dirp->upperfs_offset==0 ) dirp->upperfs_offset=1;

    if ( strlen(dirp->direntry.d_name)>0 ) direntryfrompreviousbatch=true;


    while (bufpos < size ) {

	//
	// no valid entry found yet (the purpose here is to find valid entries right?)
	// (or there is still on attached to dirp)

	// search a new direntry
	validentryfound=false;


	// start a "search" through the directory stream to the first valid next entry
	while ( ! validentryfound ) {

	    // read next entry (only when not one attached to dirp)

	    if ( strlen(dirp->direntry.d_name)==0 ) {

		// read a direntry from the cdrom

		direntryfrompreviousbatch=false;

		res=readdir_cdrom(dirp);

		if ( res==0 ) {

		    // no direntry from readdir, look at what's causing this

		    nreturn=0;
		    goto out;

		}

	    }


	    if ( ! dirp->entry ) {

		// translate it into a entry

		res=get_direntry_stat(dirp);

		if ( res<0 ) {

		    nreturn=res;
		    goto out;

		}

	    }

	    validentryfound=true;

	    if ( dirp->entry ) {

		// an entry is attached only when it's not . and ..
		// but these will never be hidden, so these are always valid

		if ( dirp->entry->hide==1 && cdfs_options.global_nohide==0 ) {

		    logoutput3("hide entry: %s", dirp->entry->name);

		    validentryfound=false;

		}

	    }

	}

	// store the next offset (of course of the underlying fs)
	// this is after the readdir, so is pointing to the next direntry

	if ( ! direntryfrompreviousbatch ) {

	    // a valid direntry not from a previous batch is here, so the offset has to be increased

	    dirp->upperfs_offset+=1;

	}



	logoutput2("adding %s to dir buffer", dirp->direntry.d_name);

	entsize = fuse_add_direntry(req, buf + bufpos, size - bufpos, dirp->direntry.d_name, &dirp->st, dirp->upperfs_offset);

	// break when buffer is not large enough
	// function fuse_add_direntry has not added it when buffer is too small to hold direntry, 
	// only returns the requested size

	if (entsize > size - bufpos) {

	    // the direntry does not fit in buffer
	    // (keep the current direntry and entry attached)
	    break;

	}

	logoutput2("increasing buffer with %zi", entsize);

	bufpos += entsize;

	dirp->direntry.d_name[0]='\0';
	dirp->direntry.d_type=0;

    }

    out:

    if (nreturn<0) {

	logoutput1("do_readdir, return: %i", nreturn);

	// if a real error: return that
	return nreturn;

    } else {

	logoutput1("do_readdir, return: %zu", bufpos);

	return bufpos;

    }

}


static void cdfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t offset, struct fuse_file_info *fi)
{
    struct cdfs_generic_dirp_struct *dirp = get_dirp(fi);
    char *buf;
    int nreturn=0;

    logoutput0("READDIR");

    // look what readdir has to be called

    buf = malloc(size);

    if (buf == NULL) {

	nreturn=-ENOMEM;
	goto out;

    }

    nreturn=do_readdir_localhost(req, buf, size, offset, dirp);

    out:

    if (nreturn < 0 ) {

	fuse_reply_err(req, -nreturn);

    } else {

	fuse_reply_buf(req, buf, nreturn);

    }

    logoutput1("readdir, nreturn %i", nreturn);
    if ( buf ) free(buf);

}


static void cdfs_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct cdfs_generic_dirp_struct *dirp = get_dirp(fi);

    (void) ino;

    logoutput0("RELEASEDIR");

    fuse_reply_err(req, 0);

    free_dirp(dirp);
    fi->fh=0;

}


static void cdfs_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    int flags = (fi->flags & O_ACCMODE);
    int nreturn=0, fd=0, res;
    struct cdfs_entry_struct *entry=NULL;
    struct cdfs_inode_struct *inode=NULL;
    int tracknr;
    struct caching_data_struct *caching_data=NULL;
    struct track_info_struct *track_info;

    logoutput0("OPEN");

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=inode->alias;

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }


    fi->direct_io=0;


    //
    // here look the buffer is present in the cache: if yes take that one
    //
    // if not what happens next:
    //
    // - whole buffer has to be cached -> command to the cdrom reader to read the whole buffer and wait for it to finish ( condition message...)
    //
    // - part(s) are already cached, just split in different commands to the cdrom reader
    //
    // what to do ??
    // if buffer is from a till c
    // and a to b ( a<b<c ) is cached and b to c not
    // just reply with a - b and send a command to the cdrom reader to read b - c. (and let it do a cond broadcast.....)
    //

    tracknr=get_tracknr(entry->name);

    if ( tracknr==0 ) {

	nreturn=-ENOENT;
	goto out;

    }

    // check and/or create the file in the cache
    // get an fd of this file
    // write the header to this file
    // and create a initial cached_block for this

    caching_data = ( struct caching_data_struct *) entry->data;

    if ( ! caching_data ) {

	// not found: probably the first time here

	caching_data=create_caching_data();

	if ( ! caching_data ) {

	    nreturn=-ENOMEM;
	    goto out;

	}

	// the cachefile is stored in the cachedirectory with the same name as the entry,,,, very simple

	snprintf(caching_data->path, PATH_MAX, "%s/%s", cdfs_options.cache_directory, entry->name);

	entry->data=(void *) caching_data;

        caching_data->tracknr=tracknr;

        track_info = (struct track_info_struct *) (cdfs_device.track_info + ( tracknr - 1 ) * sizeof(struct track_info_struct));

	caching_data->size=get_size_track(tracknr);

	caching_data->startsector=track_info->firstsector_lsn;
	caching_data->endsector=track_info->lastsector;


        nreturn=create_cache_file(caching_data, entry->name);

	if ( nreturn<0 ) {

            /* possible error creating file in cache */

            goto out;

        } else if ( nreturn==0 ) {

            // existing file found

            if ( cdfs_options.cachebackend ==  CDFS_CACHE_ADMIN_BACKEND_SQLITE ) {

                nreturn=get_intervals_from_sqlite(caching_data);

            }


        } else if ( nreturn==1 ) {

            //
            // file created, did not exist... 
	    // the first block should be the header
	    //

	    char *header=malloc(SIZE_RIFFHEADER);

	    if ( header ) {

	        write_wavheader(header, caching_data->size);

	        res=write_to_cached_file(caching_data, header, 0, SIZE_RIFFHEADER);

	        if ( res<0 ) {

		    nreturn=res;
		    goto out;

	        }

            }

            res=remove_all_intervals_sqlite(tracknr);

        }

    }

    fd=open(caching_data->path, O_RDONLY);

    if ( fd==-1 ) {

	nreturn=-errno;
	goto out;

    }


    fi->fh=fd;
    fi->keep_cache=1;
    fi->nonseekable=0;

    out:

    if (nreturn < 0) {

	fuse_reply_err(req, -nreturn);

    } else {

	fuse_reply_open(req, fi);

    }

    logoutput1("open nreturn: %i", nreturn);

}


static void cdfs_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi)
{
    int nreturn=0, res;
    struct cdfs_entry_struct *entry=NULL;
    struct cdfs_inode_struct *inode=NULL;
    struct caching_data_struct *caching_data=NULL;
    struct cached_block_struct *cached_block=NULL;
    off_t off_exheader;
    size_t headerlen=0, size_exheader;
    int startsector, endsector, tmpsector;
    char *buffer=NULL;
    unsigned char buffalloc=0, cachereadlock=0;
    bool notfound;
    struct read_call_struct *read_call=NULL;

    if ( size>0 ) {

	logoutput0("READ, offset %"PRIu64" and size (>0) %zi", off, size);

    } else {

	logoutput0("READ, offset %"PRIu64" and size zero", off);

    }

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=inode->alias;

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    caching_data = ( struct caching_data_struct *) entry->data;

    if ( ! caching_data ) {

	nreturn=-EIO;
	goto out;

    }

    buffer=malloc(size);

    if ( ! buffer ) {

	nreturn=-ENOMEM;
	goto out;

    }

    buffalloc=1;


    if ( off + size < SIZE_RIFFHEADER ) {

	// bytes requested totally from header
	// this header is already present in the cache file

	res=pread(fi->fh, buffer, size, off);

	if ( res<0 ) {

	    nreturn=-errno;

	} else {

	    nreturn=res;

	}


    } else if ( caching_data->ready==1 ) {

        // every available in cache: there is no need to investigate and possibly
        // wait for sectors to become available ( and also not to read ahead)

	res=pread(fi->fh, buffer, size, off);

	if ( res<0 ) {

	    nreturn=-errno;

	} else {

	    nreturn=res;

	}


    } else {

	// check for part in header

	off_exheader=off;
	size_exheader=size;

	if ( off < SIZE_RIFFHEADER ) {

            /* piece in header : correct the size and offset */

	    headerlen=SIZE_RIFFHEADER - off; /* number of bytes in the header */
	    size_exheader=size-headerlen; /* size minus the bytes in the header */
	    off_exheader=0; /* the offset corrected: the offset in the file on the cd */

	} else {

            off_exheader-=SIZE_RIFFHEADER; /* the offset corrected: the offset in the file on the cd */

        }

	// find out the start and end sector of the requested read 

	startsector=get_sector_from_position(caching_data->tracknr, off_exheader);
	endsector=get_sector_from_position(caching_data->tracknr, off_exheader + size_exheader);

        // look the sectors are in the cache
        // and what read_commands to send to the cdrom reader

	logoutput2("read: looking for block from %i to %i", startsector, endsector);

        notfound=true;
        tmpsector=startsector;

        read_call=get_read_call();

        if ( ! read_call ) {

            nreturn=-ENOMEM;
            goto out;

        }

        read_call->fuse_cdfs_thread_id=pthread_self();
        read_call->tracknr=caching_data->tracknr;
        read_call->startsector=startsector;
        read_call->endsector=endsector;
        read_call->nrsectorstoread=0;
        read_call->nrsectorsread=0;
        read_call->complete=0;

        read_call->nrsectorstoread=0;

        pthread_mutex_init(&read_call->lockmutex, NULL);
        pthread_cond_init(&read_call->lockcond, NULL);

        read_call->lock=0;
        cached_block=NULL;

        //
        // get a read lock on the cache blocks
        //

        nreturn=get_readlock_caching_data(caching_data);

        if ( nreturn<0 ) {

	    logoutput2("error!! not able to get read lock caching_data for track %i!! serious io error", read_call->tracknr);

            // what to do here??? try again?

        } else {

            cachereadlock=(unsigned char) nreturn;

        }

        //
        // look in the cache if any of the required bytes is already there
        //

        while (notfound) {


            if ( cached_block) {

                cached_block=cached_block->next;

            } else {

                // get the first cached block...

                cached_block=get_first_cached_block_internal(caching_data, startsector, endsector);


            }

            //
            // there are three cases: 
            // a. no cached block found, send the reader request (tmpsector, endsector) to cdromreader
            // b. there is a cached block found, it's startsector <= tmpsector
            // c. there is a cached block found, it's startsector > tmpsector
            //
            // in case a ready and leave
            //
            //

	    if ( ! cached_block ) {

	        // no data found: send a command to the cdromreader 

                read_call->nrsectorstoread += endsector - tmpsector + 1;

                nreturn=send_read_command(read_call, caching_data, tmpsector, endsector, 0, 0);
                if ( nreturn<0 ) goto out;

                break;

            } else if ( cached_block->startsector<=tmpsector ) {

                // a cached block found, with startsector left of the desired sector
                // check the endsector

                if ( cached_block->endsector >= endsector ) {

                    // (tmpsector,endsector) is in cache

                    break;

                } else {

                    // check the next interval right of cached_block->endsector

                    tmpsector=cached_block->endsector+1;
                    continue;

                }

            } else {

                // cached_block->startsector>tmpsector

                if ( cached_block->startsector>endsector ) {

                    // the cached block found doesn't offer anything
                    // so the interval has to be read

                    read_call->nrsectorstoread += endsector - tmpsector + 1;

                    nreturn=send_read_command(read_call, caching_data, tmpsector, endsector, 0, 0);
                    if ( nreturn<0 ) goto out;

	            break;

                } else {

                    // cached_block->startsector<=endsector

                    // cached block does offer something, but left of still uncached
                    // a read command is still required

                    read_call->nrsectorstoread += cached_block->startsector - tmpsector;

                    nreturn=send_read_command(read_call, caching_data, tmpsector, cached_block->startsector-1, 0, 0);
                    if ( nreturn<0 ) goto out;

                    if ( cached_block->endsector<endsector ) {

                        // right of cached block is still uncached

                        tmpsector=cached_block->endsector+1;
                        continue;

                    }

                    break;

                }

            }

        }


        if ( cachereadlock>0 ) {

            // a read lock has been set, release it

            nreturn=release_readlock_caching_data(caching_data);

        }


        // read ahead
        //
        // send a read command
        // do this only when the cache is not complete and
        // the readahead policy is set 
        //

        if ( caching_data->ready==0 ) {

            if ( cdfs_options.readaheadpolicy==READAHEAD_POLICY_PIECE || cdfs_options.readaheadpolicy==READAHEAD_POLICY_WHOLE ) {

                logoutput2("read: readahead");

                if ( endsector<caching_data->endsector ) {

                    // create only when there is some space left over
                    // to readahead

                    if ( cdfs_options.readaheadpolicy==READAHEAD_POLICY_PIECE ) {

                        nreturn=send_read_command(NULL, caching_data, endsector+1, endsector+READAHEAD_POLICY_PIECE_SECTORS, READAHEAD_POLICY_PIECE, 2);

                    } else if ( cdfs_options.readaheadpolicy==READAHEAD_POLICY_WHOLE ) {

                        // do here something to fallback to readahead piece when reading the begin sectors
                        // make the begin (250) configurable

                        if ( endsector - caching_data->startsector > CDFS_TRACK_BEGIN_SIZE ) {

                            nreturn=send_read_command(NULL, caching_data, endsector+1, endsector+READAHEAD_POLICY_WHOLE_SECTORS, READAHEAD_POLICY_WHOLE, 0);

                        } else {

                            // when in the begin of a track fall back to policy "piece"
                            // this is because lots of graphical environment read the first X bytes
                            // to get information, and thus it's not required
                            // to read the whole file

                            nreturn=send_read_command(NULL, caching_data, endsector+1, endsector+READAHEAD_POLICY_PIECE_SECTORS, READAHEAD_POLICY_PIECE, 2);

                        }

                    }

                }

            }

        }


        //
        // when there are commands send to the cdromreader wait for these to be read
        //


        if ( read_call->nrsectorstoread>0 ) {


            // here totally different: wait for an event on read_call->lockcond
            // and then compare nrsectorstoread with nrsectorsread


            logoutput2("read: read commands send, waiting for %i sectors to be read", read_call->nrsectorstoread);


            if ( read_call->nrsectorsread < read_call->nrsectorstoread ) {

                // the read commands send are not completed yet

                struct timespec expiretime;

                // no error checking here....

                res=clock_gettime(CLOCK_REALTIME, &expiretime);

                expiretime.tv_sec+=15; /* todo: make this global configurable var */

                res=pthread_mutex_lock(&(read_call->lockmutex));

                res=0;


                while ( read_call->nrsectorsread < read_call->nrsectorstoread ) {

                    logoutput2("read: inside wait loop for %i sectors, already done %i, expire: %li", read_call->nrsectorstoread, read_call->nrsectorsread, expiretime.tv_sec);

                    res=pthread_cond_timedwait(&(read_call->lockcond), &(read_call->lockmutex), &expiretime);

                    if ( res!=0 ) {

                        nreturn=-abs(res);
                        break;

                    }

                    logoutput2("read: caught signal already read: %i (total: %i )", read_call->nrsectorsread, read_call->nrsectorstoread);

                }


                res=pthread_mutex_unlock(&(read_call->lockmutex));

                // check errors

                if ( nreturn!=0 ) {

                    if ( nreturn==-ETIMEDOUT ) {

                        logoutput2("read call: waiting for read results to finish timed out");

                    } else {

                        logoutput2("read call: waiting for read results to finish unknown error %i", nreturn);

                    }


                    // handle error here???


                    goto out;

                }

            }

        }


	logoutput2("reading %zi bytes from %"PRIu64, size, off);

	res=pread(fi->fh, buffer, size, off);

	if ( res<0 ) {

	    nreturn=-errno;

	} else {

	    nreturn=res;

	}

    }

    out:

    if ( nreturn<0 ) {

	logoutput2("read, error %i", nreturn);

	fuse_reply_err(req, -nreturn);

    } else {

	logoutput2("read, %i bytes read", nreturn);

	fuse_reply_buf(req, buffer, nreturn);

    }

    if ( buffalloc==1 ) free(buffer);

    if ( read_call ) move_read_call_to_unused_list(read_call);

}



static void cdfs_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    int nreturn=0;

    logoutput0("RELEASE");

    close(fi->fh);

    fi->fh=0;

    out:

    fuse_reply_err(req, abs(nreturn));

}



static void cdfs_statfs(fuse_req_t req, fuse_ino_t ino)
{
    struct statvfs st;
    int nreturn=0;
    struct cdfs_entry_struct *entry; 
    struct cdfs_inode_struct *inode;

    logoutput0("STATFS");

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=inode->alias;

    if ( ! entry ){

	nreturn=-ENOENT;
	goto out;

    }



    memset(&st, 0, sizeof(statvfs));

    // take some values from the default

    st.f_bsize=CDIO_CD_FRAMESIZE_RAW;
    st.f_frsize=st.f_bsize; /* no fragmentation on this fs */
    st.f_blocks=cdfs_device.totalblocks;

    st.f_bfree=1; /* readonly fs */
    st.f_bavail=1; /* readonly fs */

    // does this work (conflicting types...)?

    st.f_files=inoctr;
    // st.f_ffree=UINT32_MAX - inoctr; /* inodes are of unsigned long int, 4 bytes:32 */
    st.f_ffree=4096 - inoctr;
    st.f_favail=st.f_ffree;

    // do not know what to put here...

    st.f_fsid=0;
    st.f_flag=0;
    st.f_namemax=255;

    out:

    if (nreturn==0) {

	fuse_reply_statfs(req, &st);

    } else {

        fuse_reply_err(req, nreturn);

    }

    logoutput1("statfs, B, nreturn: %i", nreturn);

}

static void cdfs_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name, const char *value, size_t size, int flags)
{
    int nreturn=0;
    int openflags = O_RDWR;
    struct cdfs_entry_struct *entry;
    struct cdfs_inode_struct *inode;
    char *basexattr=NULL;

    logoutput0("SETXATTR");

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=inode->alias;

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }


    // make this global....

    basexattr=malloc(strlen("system.") + strlen(XATTR_SYSTEM_NAME) + 3); /* plus _ and terminator */

    if ( ! basexattr ) {

        nreturn=-ENOMEM;
        goto out;

    }

    memset(basexattr, '\0', strlen("system.") + strlen(XATTR_SYSTEM_NAME) + 3);

    sprintf(basexattr, "system.%s_", XATTR_SYSTEM_NAME);

    // intercept the xattr used by the fs here and jump to the end

    if ( strlen(name) > strlen(basexattr) && strncmp(name, basexattr, strlen(basexattr))==0 ) {

	nreturn=setxattr4workspace(entry, name + strlen(basexattr), value);

    } else {

	nreturn=-ENOATTR;

    }

    out:

    if (nreturn<0) {

	fuse_reply_err(req, -nreturn);

    } else {

	fuse_reply_err(req, 0);

    }

    logoutput1("setxattr, nreturn %i", nreturn);

    if ( basexattr ) free(basexattr);


}


static void cdfs_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name, size_t size)
{
    int nreturn=0, nlen=0;
    struct cdfs_entry_struct *entry;
    struct cdfs_inode_struct *inode;
    void *value=NULL;
    int openflags = O_RDONLY;
    struct xattr_workspace_struct *xattr_workspace;
    char *basexattr=NULL;

    logoutput0("GETXATTR, name: %s, size: %i", name, size);


    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=inode->alias;

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }

    // make this global: this is always the same

    basexattr=malloc(strlen("system.") + strlen(XATTR_SYSTEM_NAME) + 3); /* plus _ and terminator */

    if ( ! basexattr ) {

        nreturn=-ENOMEM;
        goto out;

    }

    memset(basexattr, '\0', strlen("system.") + strlen(XATTR_SYSTEM_NAME) + 3);

    sprintf(basexattr, "system.%s_", XATTR_SYSTEM_NAME);


    if ( strlen(name) > strlen(basexattr) && strncmp(name, basexattr, strlen(basexattr))==0 ) {

	    // workspace related xattrs 
	    // (they begin with system. and follow the value of XATTR_SYSTEM_NAME and the a _)

	    xattr_workspace=malloc(sizeof(struct xattr_workspace_struct));

	    if ( ! xattr_workspace ) {

		nreturn=-ENOMEM;
		goto out;

	    }

	    memset(xattr_workspace, 0, sizeof(struct xattr_workspace_struct));

            // here pass only the relevant part? 

	    xattr_workspace->name=NULL;
	    xattr_workspace->size=size;
	    xattr_workspace->nerror=0;
	    xattr_workspace->value=NULL;
	    xattr_workspace->nlen=0;

	    getxattr4workspace(entry, name + strlen(basexattr), xattr_workspace);

	    if ( xattr_workspace->nerror<0 ) {

		nreturn=xattr_workspace->nerror;
		if ( xattr_workspace->value) free(xattr_workspace->value);

	    } else {

		nlen=xattr_workspace->nlen;
		if ( xattr_workspace->value ) value=xattr_workspace->value;

	    }

	    // free the tmp struct xattr_workspace
	    // note this will not free value, which is just a good thing
	    // it is used as reply overall

	    free(xattr_workspace);

	    goto out;


    } else {

	nreturn=-ENOATTR;

    }

    out:

    if ( nreturn < 0 ) { 

	fuse_reply_err(req, -nreturn);

    } else {

	if ( size == 0 ) {

	    // reply with the requested bytes

	    fuse_reply_xattr(req, nlen);

            logoutput1("getxattr, fuse_reply_xattr %i", nlen);

	} else if ( nlen > size ) {

	    fuse_reply_err(req, ERANGE);

            logoutput1("getxattr, fuse_reply_err ERANGE");

	} else {

	    // reply with the value

	    fuse_reply_buf(req, value, strlen(value));

            logoutput1("getxattr, fuse_reply_buf value %s", value);

	}

    }

    logoutput1("getxattr, nreturn: %i, nlen: %i", nreturn, nlen);

    if ( value ) free(value);
    if ( basexattr ) free(basexattr);

}


static void cdfs_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
    ssize_t nlenlist, nlenlist_workspace;
    int nreturn=0;
    char *list;
    int openflags = O_RDONLY;
    struct cdfs_entry_struct *entry;
    struct cdfs_inode_struct *inode;

    logoutput0("LISTXATTR");

    inode=find_inode(ino);

    if ( ! inode ) {

	nreturn=-ENOENT;
	goto out;

    }

    entry=inode->alias;

    if ( ! entry ) {

	nreturn=-ENOENT;
	goto out;

    }


    if ( nreturn==0 ) {

	if ( size>0 ) {

	    // just create a list with the overall size

	    list=malloc(size);

	    if ( ! list ) {

		nreturn=-ENOMEM;
		goto out;

	    }

	}

	nlenlist=listxattr4workspace(entry, list, size);

	if ( nlenlist<0 ) {

	    // some error
	    nreturn=nlenlist;
	    goto out;

	}

    }

    out:

    if ( nreturn != 0) {

	fuse_reply_err(req, -nreturn);

    } else {

	if ( size == 0 ) {

	    // reply with the requested size

	    fuse_reply_xattr(req, nlenlist);

	} else if ( nlenlist > size ) {

	    // reply does not fit: return ERANGE

	    if ( list ) free(list);

	    fuse_reply_err(req, ERANGE);

	} else {

	    fuse_reply_buf(req, list, size);

	}

    }

    // if ( list ) free(list);


    logoutput1("listxattr, nreturn: %i, nlenlist: %i", nreturn, nlenlist);

}


void create_pid_file()
{
int fd=0,res;
pathstring path;
char *buf, *tmpchar;
struct stat st;

tmpchar=getenv("TMPDIR");

if ( tmpchar ) {

    snprintf(path, sizeof(pathstring), "%s/fuse-cdfs.pid", tmpchar);

    buf=malloc(20);

    if ( buf ) {

	memset(buf, '\0', 20);

	sprintf(buf, "%d", getpid()); 

	logoutput1("storing pid: %s in %s", buf, path);

	res=stat(path, &st);


	if ( S_ISREG(st.st_mode) ) {

	    fd = open(path, O_RDWR | O_EXCL | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

	} else {

	    fd = open(path, O_RDWR | O_EXCL | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

	}


	if ( fd>0 ) {

	    res=write(fd, buf, strlen(buf));

	    close(fd);

	}

	free(buf);

    }

  }

}


void remove_pid_file()
{
    int res;
    pathstring path;
    char *tmpchar;
    struct stat st;

    tmpchar=getenv("TMPDIR");

    if ( tmpchar ) {

	snprintf(path, sizeof(pathstring), "%s/fuse-cdfs.pid", tmpchar);

	res=stat(path, &st);

	if ( res!=-1 && S_ISREG(st.st_mode) ) {

	    logoutput("Pid file %s found, removing it.", path);

	    res=unlink(path);

	}

    }

}



static void cdfs_init (void *userdata, struct fuse_conn_info *conn)
{

    // create a pid file

    create_pid_file();

}


static void cdfs_destroy (void *userdata)
{

    // remove pid file

    remove_pid_file();

}

static struct fuse_lowlevel_ops cdfs_oper = {
	.init		= cdfs_init,
	.destroy	= cdfs_destroy,
	.lookup		= cdfs_lookup,
	.forget		= cdfs_forget,
	.getattr	= cdfs_getattr,
	.open		= cdfs_open,
	.read		= cdfs_read,
	.release	= cdfs_release,
	.opendir	= cdfs_opendir,
	.readdir	= cdfs_readdir,
	.releasedir	= cdfs_releasedir,
	.statfs		= cdfs_statfs,
	.setxattr	= cdfs_setxattr,
	.getxattr	= cdfs_getxattr,
	.listxattr	= cdfs_listxattr,
};


int main(int argc, char *argv[])
{
    struct fuse_args cdfs_args = FUSE_ARGS_INIT(argc, argv);
    struct fuse_session *cdfs_session;
    char *cdfs_mountpoint;
    int foreground=0;
    int res;
    struct stat st;
    pthread_t pthreadid_cdrom_reader;
    pthread_t pthreadid_cache_manager;
    pthread_t pthreadid_create_hash;

    umask(0);

    // set logging

    openlog("fuse-cdfs", 0,0); 

    // clear commandline options

    cdfs_commandline_options.cache_directory=NULL;
    cdfs_commandline_options.progressfifo=NULL;
    cdfs_commandline_options.logging=0;
    cdfs_commandline_options.cachebackend=NULL;
    cdfs_commandline_options.hashprogram=NULL;
    cdfs_commandline_options.discid=NULL;
    cdfs_commandline_options.device=NULL;


    // set defaults

    cdfs_options.cache_directory=NULL;
    cdfs_options.logging=0;
    cdfs_options.progressfifo=NULL;
    cdfs_options.cachebackend=0;
    cdfs_options.discid=NULL;
    cdfs_options.caching=1;
    cdfs_options.device=NULL;

    // read commandline options

    res = fuse_opt_parse(&cdfs_args, &cdfs_commandline_options, cdfs_help_options, cdfs_options_output_proc);

    if (res == -1) {

	fprintf(stderr, "Error parsing options.\n");
	exit(1);

    }

    res = fuse_opt_insert_arg(&cdfs_args, 1, "-oallow_other,ro,default_permissions,nonempty,big_writes,nodev,nosuid");


    // get the device

    if ( cdfs_commandline_options.device ) {

        cdfs_options.device=cdfs_commandline_options.device;

        logoutput("call to create_track_info");

        if ( create_track_info() < 0 ) {

            fprintf(stderr, "Error opening the device...\n");
            exit(1);

        }

    } else {

        fprintf(stderr, "No device as parameter, cannot continue... (no default).\n");
        exit(1);

    }


    // cache directory

    if ( cdfs_options.caching>0 ) {

        if ( cdfs_commandline_options.cache_directory ) {

	    res=stat(cdfs_commandline_options.cache_directory, &st);

	    if ( res==-1 ) {

	        fprintf(stderr, "Cache directory %s does not exist.\n", cdfs_commandline_options.cache_directory);
	        exit(1);

	    } else if ( ! S_ISDIR(st.st_mode) ) {

	        fprintf(stderr, "%s exist but is not a directory.\n", cdfs_commandline_options.cache_directory);
	        exit(1);

	    }

	    unslash(cdfs_commandline_options.cache_directory);
	    cdfs_options.cache_directory=cdfs_commandline_options.cache_directory;

	    fprintf(stdout, "Taking cache directory %s.\n", cdfs_options.cache_directory);

        }


        if ( ! cdfs_options.cache_directory ) {

            fprintf(stdout, "Cache directory not set. Cannot continue.\n");
            exit(1);

        }

    }


    // fifo to write progress to

    if ( cdfs_commandline_options.progressfifo ) {

	res=stat(cdfs_commandline_options.progressfifo, &st);

	if ( res==-1 ) {

	    fprintf(stderr, "Fifo %s does not exist.\n", cdfs_commandline_options.progressfifo);
	    exit(1);

	} else if ( ! S_ISFIFO(st.st_mode) ) {

	    fprintf(stderr, "%s exist but is not a fifo.\n", cdfs_commandline_options.progressfifo);
	    exit(1);

	}

	unslash(cdfs_commandline_options.progressfifo);
	cdfs_options.progressfifo=cdfs_commandline_options.progressfifo;

	fprintf(stdout, "Taking progress fifo%s.\n", cdfs_options.progressfifo);

    }

    // hash program

    if ( cdfs_commandline_options.hashprogram ) {

	res=stat(cdfs_commandline_options.hashprogram, &st);

	if ( res==-1 ) {

	    fprintf(stderr, "Hashprogram %s does not exist.\n", cdfs_commandline_options.hashprogram);
	    exit(1);

	} else if ( ! S_ISREG(st.st_mode) ) {

	    fprintf(stderr, "%s exist but is not a fifo.\n", cdfs_commandline_options.hashprogram);
	    exit(1);

	}

	unslash(cdfs_commandline_options.hashprogram);
	cdfs_options.hashprogram=cdfs_commandline_options.hashprogram;

	fprintf(stdout, "Taking hashprogram %s.\n", cdfs_options.hashprogram);

    }


    cdfs_options.cachebackend=CDFS_CACHE_ADMIN_BACKEND_INTERNAL;

    if ( cdfs_commandline_options.cachebackend ) {

        if ( strcmp(cdfs_commandline_options.cachebackend, "sqlite")==0 ) {

            cdfs_options.cachebackend=CDFS_CACHE_ADMIN_BACKEND_SQLITE;

        }

    }

    // for now: readahead set here


    cdfs_options.readaheadpolicy=READAHEAD_POLICY_WHOLE; /* default */


    if ( cdfs_commandline_options.readaheadpolicy ) {

        if ( strcmp(cdfs_commandline_options.readaheadpolicy, "none")==0 ) {

            cdfs_options.readaheadpolicy=READAHEAD_POLICY_NONE;

        } else if ( strcmp(cdfs_commandline_options.readaheadpolicy, "piece")==0 ) {

            cdfs_options.readaheadpolicy=READAHEAD_POLICY_PIECE;

        } else if ( strcmp(cdfs_commandline_options.readaheadpolicy, "whole")==0 ) {

            cdfs_options.readaheadpolicy=READAHEAD_POLICY_WHOLE;

        }

    }

    //
    // init the name and inode hashtables
    //

    res=init_hashtables();

    if ( res<0 ) {

	fprintf(stderr, "Error, cannot intialize the hashtables (error: %i).\n", abs(res));
	exit(1);

    }

    //
    // create the root inode and entry
    // (and the root directory data)
    //

    root_entry=create_entry(NULL,".",NULL);

    if ( ! root_entry ) {

	fprintf(stderr, "Error, failed to create the root entry.\n");
	exit(1);

    }

    assign_inode(root_entry);

    if ( ! root_entry->inode ) {

	fprintf(stderr, "Error, failed to create the root inode.\n");
	exit(1);

    }


    add_to_inode_hash_table(root_entry->inode);
    root_entry->type=ENTRY_TYPE_ROOT;


    //
    // set default options
    //

    cdfs_options.global_nohide=0;

    cdfs_options.logging=cdfs_commandline_options.logging;

    loglevel=cdfs_options.logging;

    cdfs_options.attr_timeout=1.0;
    cdfs_options.entry_timeout=1.0;
    cdfs_options.negative_timeout=1.0;

    cdfs_device.initready=0;

    cdfs_options.secondswaitforread=15; /* a commandline option for this ??*/


    res = -1;

    if (fuse_parse_cmdline(&cdfs_args, &cdfs_mountpoint, NULL, &foreground) != -1 ) {

	if ( (cdfs_chan = fuse_mount(cdfs_mountpoint, &cdfs_args)) != NULL) {

	    cdfs_session=fuse_lowlevel_new(&cdfs_args, &cdfs_oper, sizeof(cdfs_oper), NULL);

	    if ( cdfs_session != NULL ) {

		res = fuse_daemonize(foreground);

		if (res==0) {

                    //
                    // start different helper threads
                    //

                    if ( cdfs_options.caching==1 ) {

                        logoutput("Starting create cache hash thread...");

                        res=start_do_init_in_background_thread(&pthreadid_create_hash);

                        if ( res != 0 ) {

                            logoutput("Error creating cache hash: %i.", res);

                        }

                    }

                    logoutput("Starting cdrom reader thread...");

		    res=start_cdrom_reader_thread(&pthreadid_cdrom_reader);

                    logoutput("Starting cache manager thread...");

                    res=start_cache_manager_thread(&pthreadid_cache_manager);


                    //
                    // begin fuse
                    //

		    fuse_session_add_chan(cdfs_session, cdfs_chan);

		    logoutput("Channel created, starting fuse_session_loop_epoll_mt.");

		    res=fuse_session_loop_epoll_mt(cdfs_session, loglevel);

		    fuse_session_remove_chan(cdfs_chan);

                    pthread_cancel(pthreadid_cdrom_reader);
                    pthread_cancel(pthreadid_cache_manager);

		}

		fuse_session_destroy(cdfs_session);

	    } else {

		logoutput("Error starting a new session.\n");

	    }

	    fuse_unmount(cdfs_mountpoint, cdfs_chan);

	} else {

	    logoutput("Error mounting and setting up a channel.\n");

	}

    } else {

	logoutput("Error parsing options.\n");

    }

    if ( cdfs_device.cddevice ) {

        cdio_cddap_close_no_free_cdio(cdfs_device.cddevice);

    }

    cdio_destroy(cdfs_device.p_cdio);

    // close(cdfs_device.fd); /* required ?? */

    if ( cdfs_options.cachebackend==CDFS_CACHE_ADMIN_BACKEND_SQLITE ) {

        // create the sqlite db

        res=write_all_intervals_to_sqlitedb();

        if ( res<0 ) {

            fprintf(stderr, "Error, writing to sqlite db (error: %i).\n", abs(res));

        }

    }

    out:

    fuse_opt_free_args(&cdfs_args);

    closelog();

    return res ? 1 : 0;
}
