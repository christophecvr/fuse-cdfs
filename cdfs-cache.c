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
#include <sys/param.h>
#include <fcntl.h>

#include <pthread.h>
#include <sqlite3.h>

#ifndef ENOATTR
#define ENOATTR ENODATA        /* No such attribute */
#endif

#include <fuse/fuse_lowlevel.h>

#include "logging.h"
#include "cdfs.h"

#include "entry-management.h"
#include "cdfs-cache.h"
#include "cdfs-cdromutils.h"

extern struct cdfs_options_struct cdfs_options;
extern struct cdfs_device_struct cdfs_device;

struct read_result_struct *head_queue_read_results=NULL;
struct read_result_struct *tail_queue_read_results=NULL;
struct read_result_struct *unused_read_results=NULL;

struct cached_block_struct *unused_cached_blocks=NULL;
struct caching_data_struct *list_caching_data=NULL;

unsigned char results_lock=0;
pthread_mutex_t results_lockmutex;
pthread_cond_t  results_lockcond;



//
// create a new read_result
//
// if there is an unused one, take that one
// otherwise create one
//
//

static struct read_result_struct *get_read_result()
{
    struct read_result_struct *read_result;

    if ( ! unused_read_results ) {

	// no unused ones... : create a new one

	read_result=malloc(sizeof(struct read_result_struct));

    } else {

	// take from list

	read_result=unused_read_results;
	unused_read_results=read_result->next;
	if (unused_read_results) unused_read_results->prev=NULL;

    }

    if ( read_result ) {

	read_result->startsector=0;
	read_result->endsector=0;
	read_result->read_call=NULL;
	read_result->next=NULL;
	read_result->prev=NULL;

    }

    return read_result;

}

static void move_read_result_to_unused_list(struct read_result_struct *read_result)
{
    // remove first from the active list

    if ( read_result->next ) read_result->next->prev=read_result->prev;
    if ( read_result->prev ) read_result->prev->next=read_result->next;


    // insert in unused list at beginning

    if ( unused_read_results ) {

	read_result->next=unused_read_results;

	if ( read_result->next ) read_result->next->prev=read_result;

	read_result->prev=NULL;

    }

    unused_read_results=read_result;

}


struct caching_data_struct *create_caching_data()
{
    struct caching_data_struct *caching_data;
    int nreturn=0;

    caching_data=malloc(sizeof(struct caching_data_struct));

    if ( caching_data ) {

        caching_data->fd=0;
        caching_data->startsector=0;
        caching_data->endsector=0;
        caching_data->sectorsread=0;

        caching_data->tracknr=0;

        caching_data->size=0;

	pthread_mutex_init(&(caching_data->cachelockmutex), NULL);
	pthread_cond_init(&(caching_data->cachelockcond), NULL);

        caching_data->cachelock=0;
        caching_data->nrreads=0;
        caching_data->writelock=0;

        caching_data->cached_block=NULL;


	// insert in list
	caching_data->next=list_caching_data;
	if ( list_caching_data ) list_caching_data->prev=caching_data;
	list_caching_data=caching_data;

	caching_data->prev=NULL;

    }

    return caching_data;

}


//
// todo: lock the list
//

struct caching_data_struct *find_caching_data_by_tracknr(unsigned char tracknr)
{
    struct caching_data_struct *caching_data=list_caching_data;

    while ( caching_data ) {

	if ( caching_data->tracknr == tracknr ) break;

	caching_data=caching_data->next;

    }

    return caching_data;

}

//
//
// create a file in the cache
//
// do that by checking it exist already. If that's the case check it correct
// if it does not exist, create it
//
// additionally create list of "already decoded blocks"
//
//
// possibly start with a decoded "head" of 4096 (or any other default size) bytes standard
// return:
// <0 : error
// =0 : file does exist already
// =1 : file created

int create_cache_file(struct caching_data_struct *caching_data, const char *name)
{
    struct stat st;
    int res, nreturn=0;

    // the cachefile is stored in the cachedirectory with the same name as the entry,,,, very simple

    // here a wait loop for the hashcache to be set and ready

    if ( cdfs_device.initready==0 ) {

        // hash is not set....

        nreturn=pthread_mutex_lock(&(cdfs_device.initmutex));

        while ( cdfs_device.initready==0 ) {

            nreturn=pthread_cond_wait(&(cdfs_device.initcond), &(cdfs_device.initmutex));

            // if a timeout is used: handle ETIMEDOUT here

            if ( nreturn!=0 ) {

                nreturn=-abs(nreturn);
                goto out;

            }

        }

        nreturn=pthread_mutex_unlock(&(cdfs_device.initmutex));

    }



    snprintf(caching_data->path, PATH_MAX, "%s/%s/%s", cdfs_options.cache_directory, cdfs_options.cachehash, name);


    res=stat(caching_data->path, &st);


    if ( res==-1 ) {

	// does exists already

        res=mknod(caching_data->path, S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH, 0);

        if (res==-1) {

	    nreturn=-errno;
	    goto out;

        }

        logoutput2("set size to %zi", caching_data->size);

        res=truncate(caching_data->path, caching_data->size);

        if (res<0 ) {

	    nreturn=-errno;
	    goto out;

        }

        nreturn=1; /* file created */

    }

    out:

    return nreturn;

}

//
// copy nbytes from buffer to offset on cached file
//
// admin a cached block list
//

int write_to_cached_file(struct caching_data_struct *caching_data, char *buffer, off_t offset, size_t size)
{
    int nreturn=0;
    int fd=0;
    unsigned char openhere=0;

    if ( ! buffer || size<=0 ) {

	return 0;

    } else if ( caching_data->fd==0 ) {

	logoutput2("write_to_cached file: fd is zero");
	fd=open(caching_data->path, O_WRONLY);

	if ( fd<=0 ) goto out;
	openhere=1;

    } else {

	fd=caching_data->fd;

    }

    logoutput2("write_to_cached file: writing %zi bytes to %"PRIu64" in file %s", size, offset, caching_data->path);

    nreturn = pwrite(fd, buffer, size, offset);

    if ( nreturn==-1 ) {

        logoutput("error writing to file: %i", errno);

    } else {

        logoutput("write_to_cached file: %i bytes written", nreturn);

    }

    out:

    if (openhere==1) close(fd);

    return nreturn;

}





//
// create a new "cached" interval
//
// if there is an unused one, take that one
// otherwise create one
//
//

static struct cached_block_struct *get_cached_block()
{
    struct cached_block_struct *cached_block;

    if ( ! unused_cached_blocks ) {

	// no unused ones... : create a new one

	cached_block=malloc(sizeof(struct cached_block_struct));

    } else {

	// take from list

	cached_block=unused_cached_blocks;
	unused_cached_blocks=cached_block->next;
	if (cached_block->next) unused_cached_blocks->prev=NULL;

    }

    if ( cached_block ) {

	cached_block->startsector=0;
	cached_block->endsector=0;
	cached_block->tracknr=0;
	cached_block->next=NULL;
	cached_block->prev=NULL;

    }

    return cached_block;

}

static void move_cached_block_to_unused_list(struct cached_block_struct *cached_block)
{
    // remove first from the active list

    if ( cached_block->next ) cached_block->next->prev=cached_block->prev;
    if ( cached_block->prev ) cached_block->prev->next=cached_block->next;

    // insert in unused list at beginning

    if ( unused_cached_blocks ) {

	cached_block->next=unused_cached_blocks;

	if ( cached_block->next ) cached_block->next->prev=cached_block;

	cached_block->prev=NULL;

    }

    unused_cached_blocks=cached_block;

}



//
//
// get a read lock on caching data
//
// if the mutex is locked increase the number of reads just like a semaphore
//
//

int get_readlock_caching_data(struct caching_data_struct *caching_data)
{
    int nreturn=0;
    int nerror=0;

    logoutput2("get_readlock_caching_data");

    nreturn=pthread_mutex_lock(&(caching_data->cachelockmutex));

    if ( nreturn!=0 ) {

        nerror=-abs(nreturn);
        goto out;

    }

    if ( caching_data->writelock>0 ) {

        // wait for the writelock to become zero
        // no timeout here: just wait (forever?)

        while ( caching_data->writelock>0 ) {

            nreturn=pthread_cond_wait(&(caching_data->cachelockcond), &(caching_data->cachelockmutex));

            // if a timeout is used: handle ETIMEDOUT here

            if ( nreturn!=0 ) {

                nerror=-abs(nreturn);
                goto out;

            }

        }

    }

    // here: got lock

    // increase the number of reads

    caching_data->nrreads++;
    nreturn=1;

    nreturn=pthread_mutex_unlock(&(caching_data->cachelockmutex));

    if ( nreturn!=0 ) {

        nerror=-abs(nreturn);
        goto out;

    }

    out:

    logoutput2("get_readlock_caching_data: error: %i return %i", nerror, nreturn);

    return (nerror!=0 ) ? nerror : 1;

}


//
// release read lock
//
// just like a semaphore decrease the number of reads
//

int release_readlock_caching_data(struct caching_data_struct *caching_data)
{
    int nreturn=0;
    int nerror=0;


    logoutput2("release_readlock_caching_data_internal");


    nreturn=pthread_mutex_lock(&(caching_data->cachelockmutex));

    if ( nreturn!=0 ) {

        nerror=-abs(nreturn);
        goto out;

    }

    // here: got lock

    // decrease the number of reads

    caching_data->nrreads--;

    if ( caching_data->writelock>0 ) {

        if ( caching_data->nrreads==0 ) {

            // signal waiting threads which want a writelock
            // do this only when there is one (writelock>0 ) and there are no reads anymore...
            // (this has been the latest)

            nreturn=pthread_cond_broadcast(&(caching_data->cachelockcond));

            if ( nreturn!=0 ) {

                nerror=-abs(nreturn);
                goto out;

            }

        }

    }


    // unlock

    nreturn=pthread_mutex_unlock(&(caching_data->cachelockmutex));

    if ( nreturn!=0 ) {

        nerror=-abs(nreturn);
        goto out;

    }

    out:

    logoutput2("release_readlock_caching_data: error: %i return %i", nerror, nreturn);

    return (nerror!=0 ) ? nerror : nreturn;

}

//
// get a write lock on caching data
//
// TODO: some stepping down from another write lock ?
//

int get_writelock_caching_data(struct caching_data_struct *caching_data)
{
    int nreturn=0;
    int nerror=0;

    logoutput2("get_writelock_caching_data");

    nreturn=pthread_mutex_lock(&(caching_data->cachelockmutex));

    if ( nreturn!=0 ) {

        nerror=-abs(nreturn);
        goto out;

    }


    // wait for the writelock to become zero or one
    // no timeout here: just wait (forever?)

    if ( caching_data->writelock==2 ) {

        while ( caching_data->writelock==2 ) {

            nreturn=pthread_cond_wait(&(caching_data->cachelockcond), &(caching_data->cachelockmutex));

            // if a timeout is used: handle ETIMEDOUT here

            if ( nreturn!=0 ) {

                nerror=-abs(nreturn);
                goto out;

            } else if ( caching_data->writelock==1 || caching_data->writelock==0 ) {

                break;

            }

        }

    }


    //
    // here: got the mutex lock
    //

    if ( caching_data->writelock==1 ) {

        // got it from another write lock which is releasing

        caching_data->writelock=2;

    } else {

        // set it to 1 to prevent new reads
        caching_data->writelock=1;

        // wait for read locks to be released

        if ( caching_data->nrreads>0 ) {

            while ( caching_data->nrreads>0 ) {

                nreturn=pthread_cond_wait(&(caching_data->cachelockcond), &(caching_data->cachelockmutex));

            }

        }

        caching_data->writelock=2;

    }


    nreturn=pthread_mutex_unlock(&(caching_data->cachelockmutex));

    if ( nreturn!=0 ) {

        nerror=-abs(nreturn);
        goto out;

    }

    out:

    logoutput2("get_writelock_caching_data: error: %i return %i", nerror, nreturn);

    return (nerror!=0 ) ? nerror : 1;

}


//
// release write lock
//

int release_writelock_caching_data(struct caching_data_struct *caching_data)
{
    int nreturn=0;
    int nerror=0;

    logoutput2("release_writelock_caching_data");

    nreturn=pthread_mutex_lock(&(caching_data->cachelockmutex));

    if ( nreturn!=0 ) {

        nerror=-abs(nreturn);
        goto out;

    }

    // here: got mutex lock
    // two steps: first get down to 1 to enable other waiting threads for a write lock
    // to get this lock

    //
    // first step down to 1
    //

    caching_data->writelock=1;

    nreturn=pthread_cond_broadcast(&(caching_data->cachelockcond));

    if ( nreturn!=0 ) {

        nerror=-abs(nreturn);
        goto out;

    }

    // unlock

    nreturn=pthread_mutex_unlock(&(caching_data->cachelockmutex));

    if ( nreturn!=0 ) {

        nerror=-abs(nreturn);
        goto out;

    }


    //
    // second step down to 0
    //


    nreturn=pthread_mutex_lock(&(caching_data->cachelockmutex));

    if ( nreturn!=0 ) {

        nerror=-abs(nreturn);
        goto out;

    }

    if ( caching_data->writelock ) {

        caching_data->writelock=0;

        nreturn=pthread_cond_broadcast(&(caching_data->cachelockcond));

        if ( nreturn!=0 ) {

            nerror=-abs(nreturn);
            goto out;

        }

    }

    // unlock

    nreturn=pthread_mutex_unlock(&(caching_data->cachelockmutex));

    if ( nreturn!=0 ) {

        nerror=-abs(nreturn);
        goto out;

    }

    out:

    logoutput2("release_writelock_caching_data: error: %i return %i", nerror, nreturn);

    return (nerror!=0 ) ? nerror : nreturn;

}


//
//
// insert a cached interval/block in the list of existing cached blocks
//
// merging with existing intervals is possible
//
// called from the special cache thread, that takes care of the setting of locks
//

int insert_cached_block_internal(struct caching_data_struct *caching_data, unsigned int startsector, unsigned int endsector)
{
    struct cached_block_struct *cached_block=NULL;
    struct cached_block_struct *cached_block_found=NULL;
    struct cached_block_struct *next_cached_block=NULL;
    struct cached_block_struct *before_cached_block=NULL;
    struct cached_block_struct *after_cached_block=NULL;
    unsigned int nrsectors = endsector - startsector + 1;
    unsigned int tmpsectors=0;
    int nwritelock;


    logoutput2("insert block internal: new block 2 insert from %i to %i", startsector, endsector);

    nwritelock=get_writelock_caching_data(caching_data);

    if ( nwritelock<0 ) {

        // for now ignore the error and just insert without a lock

        logoutput2("insert block internal: unable to get a write lock....continue anyway without a lock");

        nwritelock=0;
        goto out;

    }


    cached_block=caching_data->cached_block;

    while ( cached_block ) {

	next_cached_block=cached_block->next; /* keep it cause cached_block might get lost */

	// check the cached block is really before

	if ( startsector > cached_block->endsector + 1 ) {

	    logoutput2("insert block internal: before cached block found (%i - %i)", cached_block->startsector, cached_block->endsector);

	    before_cached_block=cached_block;
	    cached_block=next_cached_block;
	    continue;

	}

	// check the cached block is really after

	if ( cached_block->startsector > endsector + 1 ) {

	    logoutput2("insert block internal: after cached block found (%i - %i)", cached_block->startsector, cached_block->endsector);

	    after_cached_block=cached_block;
	    break;

	}

	// everything else is to merge, is as simple as that

	logoutput2("insert block internal: merging with block from %i to %i", cached_block->startsector, cached_block->endsector);

        // merge

        if ( ! cached_block_found ) {

            // adjust cached_block

            tmpsectors=cached_block->endsector - cached_block->startsector + 1;

	    if ( cached_block->startsector > startsector ) {

                cached_block->startsector=startsector;

            } else {

                startsector=cached_block->startsector;

            }

	    if ( cached_block->endsector < endsector ) {

                cached_block->endsector=endsector;

            } else {

                endsector=cached_block->endsector;

            }

            cached_block_found=cached_block;

            // the actual sectors added are the difference

            nrsectors=cached_block->endsector - cached_block->startsector + 1 - tmpsectors;

        } else {

            // merge two existing blocks together
            // adjust cached_block_found and
            // move cached_block to unused list
            // is this ok???

            tmpsectors=endsector - startsector + 1;

            if ( cached_block->endsector>endsector ) {

                cached_block_found->endsector=cached_block->endsector;
                endsector=cached_block_found->endsector;

            }

            if ( cached_block->startsector<startsector ) {

                cached_block_found->startsector=cached_block->startsector;
                startsector=cached_block_found->startsector;

            }

	    if ( cached_block->prev ) cached_block->prev->next=cached_block->next;
	    if ( cached_block->next ) cached_block->next->prev=cached_block->prev;

	    move_cached_block_to_unused_list(cached_block);

            nrsectors=cached_block_found->endsector - cached_block_found->startsector + 1 - tmpsectors;

        }

	cached_block=next_cached_block;

    }


    //
    // no block found to merge with
    //

    if ( ! cached_block_found ) {

        cached_block_found=get_cached_block();

        cached_block_found->startsector=startsector;
        cached_block_found->endsector=endsector;
        cached_block_found->tracknr=caching_data->tracknr;

    }


    if ( before_cached_block ) {

	cached_block_found->prev=before_cached_block;
	before_cached_block->next=cached_block_found;

    } else {

	caching_data->cached_block=cached_block_found;
	cached_block_found->prev=NULL;

    }

    if ( after_cached_block ) {

	cached_block_found->next=after_cached_block;
	after_cached_block->prev=cached_block_found;

    } else {

	cached_block_found->next=NULL;

    }

    //
    // update the number of sectors read
    //

    caching_data->sectorsread+=nrsectors;
    cached_block=caching_data->cached_block;

    if ( cached_block ) {

        // check everything is read

        if ( ! cached_block->next && 
        cached_block->startsector==caching_data->startsector && 
        cached_block->endsector==caching_data->endsector ) {

            caching_data->ready=1;

        }

    }

    //
    // release the lock
    //

    if ( nwritelock>0 ) {

        nwritelock=release_writelock_caching_data(caching_data);

    }

    out:

    logoutput2("insert block internal: %i sectors added", nrsectors);

    return nrsectors;

}



struct cached_block_struct *get_first_cached_block_internal(struct caching_data_struct *caching_data, unsigned int startsector, unsigned int endsector)
{
    struct cached_block_struct *cached_block=NULL;


    if ( caching_data->cached_block ) {

        // there are blocks: look for the first block where
        // the endsector is past the first sector we're looking for

        cached_block=caching_data->cached_block;

        while (cached_block) {

            if ( cached_block->endsector >= startsector ) break;
            cached_block=cached_block->next;

        }

    }


    return cached_block;
}



//
// send a read result from the cdrom reader to the queue of the cache manager
//

int send_read_result_to_cache(struct read_call_struct *read_call, struct caching_data_struct *caching_data, unsigned int startsector, char *buffer, unsigned int nrsectors)
{
    int nreturn=0;
    struct read_result_struct *read_result;

    logoutput2("send result to cache: new block 2 insert: (%i - %i)", startsector, startsector + nrsectors);

    read_result=get_read_result();

    if ( ! read_result ) {

        nreturn=-ENOMEM;
        goto out;

    }

    read_result->startsector=startsector;
    read_result->endsector=startsector + nrsectors - 1;
    read_result->read_call=read_call;
    read_result->buffer=buffer;
    read_result->read_call=read_call;
    read_result->caching_data=caching_data;

    // lock the results queue: set lock to 1

    pthread_mutex_lock(&(results_lockmutex));

    while (results_lock==1) {

        // if already 1, wait for it to become 0

	pthread_cond_wait(&(results_lockcond), &(results_lockmutex));

    }

    results_lock=1;
    pthread_mutex_unlock(&(results_lockmutex));


    // simple insert at list

    if ( tail_queue_read_results ) {

        tail_queue_read_results->next=read_result;

    }

    read_result->prev=tail_queue_read_results;
    tail_queue_read_results=read_result;

    if ( ! head_queue_read_results ) head_queue_read_results=read_result;

    logoutput2("send result to cache: added to read results queue");


    // unlock the results queue and signal something has changed

    pthread_mutex_lock(&(results_lockmutex));
    results_lock=0;
    pthread_cond_broadcast(&(results_lockcond));
    pthread_mutex_unlock(&(results_lockmutex));


    out:

    return nreturn;

}

//
// notify waiting clients for data to be present in cache
//
// clients are probably only be threads for the read call
// one such a client has initiated the read command and is waiting for
// the result to be present in the cache
// this function is doing a broadcast 
// any client interested will take the right action, probably checking the data
// which has become available in the cache is enough for the read request
//

void notify_waiting_clients(struct read_call_struct *read_call, unsigned int startsector, unsigned int endsector)
{


    logoutput2("notify waiting clients: received read result: (%i - %i)", startsector, endsector);

    // increase the bytes read, and broadcast the original read call

    pthread_mutex_lock(&(read_call->lockmutex));

    read_call->lock=1; /* close for other actions */

    // increase the bytes read

    read_call->nrsectorsread += endsector - startsector + 1;

    read_call->lock=0; /* open for other actions */

    pthread_cond_broadcast(&(read_call->lockcond));
    pthread_mutex_unlock(&(read_call->lockmutex));


}

//
// write progress data to a fifo
// todo make a difference between tracks
//

void write_progress_to_fifo(struct caching_data_struct *caching_data)
{
    int procentread=0;
    char string2write[4];
    int fd=0;

    fd=open(cdfs_options.progressfifo, O_WRONLY);

    if ( fd>0 ) {

	procentread = 100 * caching_data->sectorsread / ( caching_data->endsector - caching_data->startsector + 1 );

	snprintf(string2write, 4, "%i", procentread);

	write(fd, string2write, strlen(string2write));

	close(fd);

    }

}


//
//
// thread to process the read results from the cdrom
// a. write the result to the cached file
// b. update the cache administration
// c. send a broadcast signal to waiting clients
// d. send progress information to a fifo
//
// TODO: howto terminate?
//
//


static void *cache_manager_thread()
{
    struct read_result_struct *read_result;
    off_t offset_infile;
    struct caching_data_struct *caching_data;
    unsigned int nrsectors=0;
    int nreturn=0, nerror=0;


    pthread_mutex_init(&results_lockmutex, NULL);
    pthread_cond_init(&results_lockcond, NULL);


    while (1) {

	// wait for something on the results queue and free to lock/use

	pthread_mutex_lock(&results_lockmutex);

	while ( results_lock==1 || ! head_queue_read_results) {

	    pthread_cond_wait(&results_lockcond, &results_lockmutex);

	}

        results_lock=1;

	// queue is not empty: get the read result

	read_result=head_queue_read_results;

	// remove from results queue

	head_queue_read_results=read_result->next;
	if ( tail_queue_read_results == read_result ) tail_queue_read_results=head_queue_read_results;
	if ( head_queue_read_results ) head_queue_read_results->prev=NULL;

	results_lock=0;

	pthread_cond_broadcast(&(results_lockcond));
	pthread_mutex_unlock(&(results_lockmutex)); /* release the results queue */


        logoutput2("cache manager: received read result: (%i - %i)", read_result->startsector, read_result->endsector);


	// really detach from the queue
	read_result->next=NULL;
	read_result->prev=NULL;


        // lookup caching_data

        caching_data=read_result->caching_data;

        if ( ! caching_data ) {

	    logoutput("cache manager: error!! caching_data not found!! serious io error");

	    //
	    // move the read_result to unused list
	    //

            move_read_result_to_unused_list(read_result);

            continue;

        }


        //
        // write to cached file
        //

        nrsectors=read_result->endsector - read_result->startsector + 1;
        logoutput1("cache manager: number of sectors from read result: %i", nrsectors);


        //
        // todo : differ per cache backend
        //

        offset_infile=SIZE_RIFFHEADER + ( read_result->startsector - caching_data->startsector ) * CDIO_CD_FRAMESIZE_RAW;
        write_to_cached_file(caching_data, read_result->buffer, offset_infile, nrsectors * CDIO_CD_FRAMESIZE_RAW);


        //
        // update the cache administration
        // the actual number of sectors added to the cache is kept
        // this maybe different from the nr sectors of the read result
        // if if if the read result is merged with an earlier result
        // and there is some overlap
        // but that's probably not the case
        //

        nrsectors=insert_cached_block_internal(caching_data, read_result->startsector, read_result->endsector);

        logoutput1("cache manager: number of sectors inserted: %i", nrsectors);

        // here also: update the sqlite db.....


        // write progress to fifo

        if ( cdfs_options.progressfifo ) {

            write_progress_to_fifo(caching_data);

        }

        //
        // notify waiting clients
        // only if there is one
        // in case of read ahead (which is not started by any client but by the fs automatic)
        // there are none

        if ( read_result->read_call ) {

            notify_waiting_clients(read_result->read_call, read_result->startsector, read_result->endsector);

        }


        //
        // data written to file... so it's safe to free the buffer and read_result
        //
        // a big TODO: what to do here when the original read_call is "orphaned/lost/not there"
        //

        if ( read_result->buffer ) free(read_result->buffer);
        move_read_result_to_unused_list(read_result);


    }

    pthread_mutex_destroy(&results_lockmutex);
    pthread_cond_destroy(&results_lockcond);

}


int start_cache_manager_thread(pthread_t *pthreadid)
{
    int nreturn=0;

    //
    // create a thread to manage the cache
    //

    nreturn=pthread_create(pthreadid, NULL, cache_manager_thread, NULL);

    if ( nreturn==-1 ) {

	// some error creating the thread

        nreturn=-errno;

	logoutput("Error creating a new thread (error: %i).", abs(nreturn));


    }

    return nreturn;

}

//
//
// sqlite functions
//
//

//
// create the table for a specific track
// every track has it's own table
//

static int create_table_track(unsigned char tracknr)
{
    int nreturn=0;
    char sql_string[SQL_STRING_MAX_SIZE];

    snprintf(sql_string, SQL_STRING_MAX_SIZE, "CREATE TABLE IF NOT EXISTS tracknr_%i (startsector INTEGER PRIMARY KEY, endsector INTEGER)", tracknr);

    nreturn=sqlite3_exec(cdfs_options.dbhandle, sql_string, 0, 0, 0);

    return nreturn;

}

//
// open (and eventually create) the sqlite db
//

int create_sqlite_db(unsigned char nrtracks)
{
    int nreturn=0, i;
    char sql_string[SQL_STRING_MAX_SIZE];

    // todo: add the hash to the path

    if ( cdfs_options.cachehash ) {

        snprintf(cdfs_options.dbpath, PATH_MAX, "%s/%s/cache.sqlite3", cdfs_options.cache_directory, cdfs_options.cachehash);

    } else {

        snprintf(cdfs_options.dbpath, PATH_MAX, "%s/cache.sqlite3", cdfs_options.cache_directory);

    }

    nreturn=sqlite3_open(cdfs_options.dbpath, &(cdfs_options.dbhandle));

    if ( nreturn==0 ) {

        for (i=1; i<=nrtracks;i++) {

            nreturn=create_table_track(i);

        }

    }

    return nreturn;

}


//
// create a specific interval into the specific track table
// typically used at shutdown when writing all information to the sqlite db
//

static int create_interval_sqlite(unsigned char tracknr, unsigned int startsector, unsigned int endsector)
{
    char sql_string[SQL_STRING_MAX_SIZE];
    int nreturn;

    logoutput2("add a new interval (%i, %i)\n", startsector, endsector);

    snprintf(sql_string, SQL_STRING_MAX_SIZE, "INSERT OR IGNORE INTO tracknr_%i (startsector, endsector) VALUES (%i, %i)", tracknr, startsector, endsector);

    sqlite3_exec(cdfs_options.dbhandle, sql_string, 0, 0, 0);

    if ( nreturn==0 ) nreturn=endsector - startsector + 1;

    return nreturn;

}


//
// remove all intervals from a specific track table
// typically used at shutdown to remove all (old) information in the db before
// writing new information to it.
//


int remove_all_intervals_sqlite(unsigned char tracknr)
{
    char sql_string[SQL_STRING_MAX_SIZE];
    int nreturn;

    logoutput2("removing all intervals for tracknr %i\n", tracknr);

    snprintf(sql_string, SQL_STRING_MAX_SIZE, "DELETE FROM tracknr_%i", tracknr);

    sqlite3_exec(cdfs_options.dbhandle, sql_string, 0, 0, 0);

    return nreturn;

}



//
// write every cached block per track to the sqlite db
// typically called when fs shuts down
//

int write_intervals_to_sqlitedb(struct caching_data_struct *caching_data)
{
    struct cached_block_struct *cached_block=NULL;
    int nreturn=0;

    // first clean everything, a little bit rude, but it works

    nreturn=remove_all_intervals_sqlite(caching_data->tracknr);

    cached_block=caching_data->cached_block;

    while (cached_block) {

        nreturn=create_interval_sqlite(caching_data->tracknr, cached_block->startsector, cached_block->endsector);

        if ( nreturn<0 ) {

            logoutput2("adding a new interval in sqlite failed: %i\n", nreturn);
            break;

        }

        cached_block=cached_block->next;

    }

    return nreturn;

}


//
// write all the interval for all the tracks to the sqlite db
//

int write_all_intervals_to_sqlitedb()
{
    struct caching_data_struct *caching_data=list_caching_data;
    int nreturn=0;

    while (caching_data) {

        nreturn=write_intervals_to_sqlitedb(caching_data);

        caching_data=caching_data->next;

    }

    return nreturn;

}

//
// read the sectors from the sqlite db
//
// typically used at open call when opening the cache for the first time
//


int get_intervals_from_sqlite(struct caching_data_struct *caching_data)
{
    char sql_string[SQL_STRING_MAX_SIZE];
    sqlite3_stmt *stmt;
    unsigned int startsector, endsector;
    struct cached_block_struct *cached_block_first=NULL;
    struct cached_block_struct *cached_block_last=NULL;
    struct cached_block_struct *cached_block_new=NULL;
    int nreturn=0;
    int count=0;


    // get the borders of the interval

    snprintf(sql_string, SQL_STRING_MAX_SIZE, "SELECT startsector,endsector FROM tracknr_%i ORDER BY startsector", caching_data->tracknr);


    nreturn=sqlite3_prepare(cdfs_options.dbhandle, sql_string, -1, &stmt, NULL);

    while (1) {


        // get a row


        nreturn=sqlite3_step(stmt);

        if ( nreturn==SQLITE_ROW ) {


            startsector=(unsigned int) sqlite3_column_int(stmt,0);
            endsector=(unsigned int) sqlite3_column_int(stmt,1);


            cached_block_new=get_cached_block();

            count++;

            cached_block_new->startsector=startsector;
            cached_block_new->endsector=endsector;
            cached_block_new->tracknr=caching_data->tracknr;


            cached_block_new->prev=cached_block_last;
            if ( cached_block_last ) cached_block_last->next=cached_block_new;
            cached_block_new->next=NULL;

            if ( ! cached_block_first ) cached_block_first=cached_block_new; /* first cached block */
            cached_block_last=cached_block_new; /* last cached block */


        } else {

            // no row anymore

            break;

        }

    }

    nreturn=sqlite3_finalize(stmt);


    caching_data->cached_block=cached_block_first;

    if ( count==1 ) {

        if ( cached_block_first->startsector==caching_data->startsector && 
             cached_block_first->endsector==caching_data->endsector ) {

                //
                // when there is one interval and it's the whole interval
                // then the cache is complete/ready
                //

                caching_data->ready=1;


        }

    }


    return nreturn;

}



