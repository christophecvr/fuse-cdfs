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
#include <pthread.h>

#include <fuse/fuse_lowlevel.h>

#include <sqlite3.h>

#include "logging.h"
#include "cdfs.h"

#include "entry-management.h"
#include "cdfs-cache.h"
#include "cdfs-cdromutils.h"

extern struct cdfs_options_struct cdfs_options;
extern struct cdfs_device_struct cdfs_device;

struct read_command_struct *head_queue_read_commands=NULL;
struct read_command_struct *tail_queue_read_commands=NULL;

struct read_command_struct *unused_read_commands=NULL;
struct read_call_struct *unused_read_calls=NULL;

unsigned char queue_lock=0;

pthread_mutex_t queue_lockmutex;
pthread_cond_t  queue_lockcond;


// lock vars to lock the the queue



static char wavheader[SIZE_RIFFHEADER] = {
    'R', 'I', 'F', 'F',
    0, 0, 0, 0,					//file size
    'W', 'A', 'V', 'E',
    'f', 'm', 't', ' ',
    16, 0, 0, 0,				//size of tag
    1, 0,						//Format
    2, 0,						//Channels
    0x44, 0xac, 0, 0,			// Samplerate 44100
    0x10, 0xb1, 0x2, 0,			// avg byte/sec 44100*2*2
    4, 0,						//Block align
    16, 0,						//bits/sample
    'd', 'a', 't', 'a',
    0, 0, 0, 0					//size of tag
};


// utilities

//
// tool for determin the discid
//

static int cddb_sum(int n)
{
    int result=0;

    while ( n>0 ) {

        result+=n%10;
        n /= 10;

    }

    return result;

}

//
// create the table with track info
//
// when there is no device with audio there is an error
//
//


int create_track_info()
{
    int i, j, nreturn=0;
    struct track_info_struct *track_info;

    logoutput("get trackinfo from %s", cdfs_options.device);

    cdfs_device.p_cdio=cdio_open(cdfs_options.device, DRIVER_DEVICE);

    if ( ! cdfs_device.p_cdio ) {

        nreturn=-ENOENT;
        goto out;

    }

    // a valid cd device, now look at the number of tracks....

    cdfs_device.nrtracks=cdio_get_num_tracks(cdfs_device.p_cdio);

    logoutput("number tracks: %i", cdfs_device.nrtracks);

    if ( cdfs_device.nrtracks<=0 || cdfs_device.nrtracks==CDIO_INVALID_TRACK ) {

        nreturn=-EIO;
        cdio_destroy(cdfs_device.p_cdio);
        cdfs_device.p_cdio=NULL;
        goto out;

    }

    i=cdio_get_first_track_num(cdfs_device.p_cdio);

    // allocate the space for the tracks info (array)

    cdfs_device.track_info = malloc(cdfs_device.nrtracks * sizeof(struct track_info_struct));

    if ( ! cdfs_device.track_info ) {

        nreturn=-ENOMEM;
        cdio_destroy(cdfs_device.p_cdio);
        cdfs_device.p_cdio=NULL;
        goto out;

    }

    for ( j=0; j<cdfs_device.nrtracks; j++) {

        track_info = (struct track_info_struct *) (cdfs_device.track_info + j * sizeof(struct track_info_struct));

        track_info->firstsector_lsn=cdio_get_track_lsn(cdfs_device.p_cdio, i);
        track_info->firstsector_lba=cdio_get_track_lba(cdfs_device.p_cdio, i);

        track_info->lastsector=cdio_get_track_last_lsn(cdfs_device.p_cdio, i);

        logoutput("track %i: %i - %i", i, track_info->firstsector_lsn, track_info->lastsector);

        i++;

    }

    out:

    return nreturn;

}

char *create_unique_hash(const char *path)
{
    char *completeprogram=NULL;
    int nlen=0;
    char outputline[256];
    char *hash;

    // if defined use the hash program defined on the commandline
    // maybe do this by using a library like mhash in stead of running an external command

    if ( cdfs_options.hashprogram ) {

        nlen=strlen(cdfs_options.hashprogram) + 1 + strlen(path) + 1;

    } else {

        nlen=strlen("md5sum") + 1 + strlen(path) + 1;

    }

    completeprogram=malloc(nlen);

    if ( completeprogram ) {

        char *tmppos=NULL;
        FILE *pipe;

        memset(completeprogram, '\0', nlen);

        if ( cdfs_options.hashprogram ) {

            snprintf(completeprogram, nlen, "%s %s", cdfs_options.hashprogram, path);

        } else {

            // if no hashprogram defined use the default: md5sum

            snprintf(completeprogram, nlen, "md5sum %s", path);

        }

        logoutput2("create_cache_hash: running %s", completeprogram);

        pipe=popen(completeprogram, "r");

        if ( pipe ) {

            while ( ! feof(pipe) ) {

                memset(outputline, '\0', 256);

                if ( ! fgets(outputline, 256, pipe) ) continue;

                if ( strlen(outputline)>0 ) {

                    logoutput2("create_cache_hash: got output %s", outputline);

                    tmppos=strchr(outputline, ' '); /* look for the first space */

                    if ( tmppos ) break;

                }

            }

            pclose(pipe);

            if ( tmppos ) {

                // copy the first part till the first space

                nlen=tmppos-outputline;

                hash=malloc(nlen+1);

                if ( hash ) {

                    memset(hash, '\0', nlen+1);
                    strncpy(hash, outputline, nlen);

                }

            }

        }

        free(completeprogram);

    }

    return hash;

}



int create_discid()
{
    char path[PATH_MAX];
    int i, j, nreturn;
    struct track_info_struct *track_info;
    unsigned nsum=0;

    for ( j=0; j<cdfs_device.nrtracks; j++) {

        track_info = (struct track_info_struct *) (cdfs_device.track_info + j * sizeof(struct track_info_struct));

        nsum+= cddb_sum(track_info->firstsector_lba / CDIO_CD_FRAMES_PER_SEC);

    }


    {

        unsigned start_sec = cdio_get_track_lba(cdfs_device.p_cdio, 1) / CDIO_CD_FRAMES_PER_SEC;
        unsigned leadout_sec = cdio_get_track_lba(cdfs_device.p_cdio, CDIO_CDROM_LEADOUT_TRACK) / CDIO_CD_FRAMES_PER_SEC;
        unsigned total = leadout_sec - start_sec;
        unsigned discid=((nsum % 0xff) << 24 | total << 8 | cdfs_device.nrtracks);

        snprintf(cdfs_device.discidfile, PATH_MAX, "%s/discid.%i", cdfs_options.cache_directory, (int) getpid());

        FILE *pipe=fopen(cdfs_device.discidfile, "wb");

        if ( pipe ) {

            logoutput2("file open for writing");

            fprintf(pipe, "%08X %d", discid, cdfs_device.nrtracks);

            j=0;

            for ( i=1; i<= cdfs_device.nrtracks; i++) {

                track_info = (struct track_info_struct *) (cdfs_device.track_info + j * sizeof(struct track_info_struct));

                fprintf(pipe, " %ld", (long) track_info->firstsector_lba);

                j++;

            }

            fprintf(pipe, " %ld\n", (long) leadout_sec);

            fclose(pipe);


        } else {

            logoutput2("unable to open file for writing");

        }

    }

    out:

    return nreturn;

}

//
// function to do various calls in background
//

void *do_init_in_background()
{
    char *hash=NULL;
    char path[PATH_MAX];
    int nreturn=0;

    // first create the discid
    // store the file with the discid somewhere

    nreturn=create_discid();

    if ( nreturn<0 ) goto out;

    if ( cdfs_options.caching > 0 ) {

        // create an unique hash to use in the cache path 

        hash=create_unique_hash(cdfs_device.discidfile);

    }

    out:

    if ( hash ) {

        logoutput2("do_init_in_background: got hash %s", hash);

        cdfs_options.cachehash=hash;

        snprintf(path, PATH_MAX, "%s/%s", cdfs_options.cache_directory, hash);

        nreturn=mkdir(path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

        if ( nreturn==0 || ( nreturn==-1 && errno==EEXIST ) ) {

            // move the discid file to the new created directory

            snprintf(path, PATH_MAX, "%s/%s/discid", cdfs_options.cache_directory, hash);

            nreturn=rename(cdfs_device.discidfile, path);

            strcpy(cdfs_device.discidfile, path);

            // make the cache set and available

            pthread_mutex_lock(&cdfs_device.initmutex);

            cdfs_device.initready=1;

            pthread_cond_broadcast(&(cdfs_device.initcond));
            pthread_mutex_unlock(&cdfs_device.initmutex);

        }

    }


    if ( cdfs_device.initready==1 && cdfs_options.cachebackend==CDFS_CACHE_ADMIN_BACKEND_SQLITE ) {

        // create the sqlite db

        nreturn=create_sqlite_db(cdfs_device.nrtracks);

        if ( nreturn<0 ) {

            fprintf(stderr, "Error, cannot create the sqlite db (error: %i).\n", abs(nreturn));

        }

    }

    return;

}

int start_do_init_in_background_thread(pthread_t *pthreadid)
{
    int nreturn=0;

    //
    // create a thread to read the cd
    //

    nreturn=pthread_create(pthreadid, NULL, do_init_in_background, NULL);

    if ( nreturn==-1 ) {

	// some error creating the thread

        nreturn=-errno;

	logoutput("Error creating a new thread (error: %i).", abs(nreturn));


    }

    return nreturn;

}



//
// function to open the cdrom using cdda functions
// this is required to open
//

static int open_cdrom_cdda()
{
    int nreturn=0;

    // open the device using "high level" calls from  cddap library

    // identify the cdrom using the p_cdio which has been set before

    cdfs_device.cddevice=cdio_cddap_identify_cdio(cdfs_device.p_cdio, 0, NULL);

    if ( ! cdfs_device.cddevice ) {

	logoutput("Error, cannot identify device %s.", cdfs_options.device);
	nreturn=-EIO;
	goto out;

    } else {

	logoutput("Found cdrom model %s.", cdfs_device.cddevice->drive_model);

    }


    if ( cdio_cddap_open(cdfs_device.cddevice) != 0 ) {

	logoutput("Cannot open device %s with cdda.", cdfs_options.device);
	cdio_cddap_close(cdfs_device.cddevice);
	nreturn=-EIO;
	goto out;

    }

    // get additional info from cd like:
    // a. number of tracks
    // b. first sector
    // c. last sector
    // d. totalblocks

    logoutput("Number of tracks: %li.", cdio_cddap_tracks(cdfs_device.cddevice));
    logoutput("First audio sector: %li.", cdfs_device.cddevice->audio_first_sector);
    logoutput("Last audio sector: %li.", cdfs_device.cddevice->audio_last_sector);

    cdfs_device.totalblocks=get_totalblocks();

    logoutput("Total blocks: %li.", cdfs_device.totalblocks);

    out:

    return nreturn;

}


//
// determine the tracknummer given the name, which is of the form:
//
// track-%nr%.wav
//
// return the tracknr
//
// if it cannot resolve the number, return 0
// 


int get_tracknr(const char *name)
{
    int i;

    if (strlen(name) == 12 && strncmp(name, "track-", 6)==0 && strcmp(name + 8, ".wav")==0 ) {

	if ( sscanf(name + 6, "%d", &i) == 1 && i > 0 && i <= cdfs_device.nrtracks) return i;

    }

    return 0;

}

//
// get size of track
//

size_t get_size_track(int tracknr)
{
    size_t size=0;
    struct track_info_struct *track_info;

    // get the right trackinfo from the array

    track_info = (struct track_info_struct *) (cdfs_device.track_info + (tracknr - 1) * sizeof(struct track_info_struct));

    size =  SIZE_RIFFHEADER + (track_info->lastsector  - track_info->firstsector_lsn + 1) * CDIO_CD_FRAMESIZE_RAW;

    logoutput3("get_size_track: size %zi for track %i", size, tracknr);

    return size;

}

//
// get total nr of blocks used by tracks on the cd
// note the calculation of the total size first and then by dividing this by the block size (=sector size)
// this is because the file size presented by this fs is bigger than on the cd
// the difference is the header ( which is not part of the track on the cd!!! here this fs has to add this header)
//

unsigned long get_totalblocks()
{
    unsigned long totalsectors=0;
    size_t totalsize=0;
    int i=1;

    while ( i<= cdfs_device.nrtracks) {

        totalsize+=get_size_track(i);
        i++;

    }

    totalsectors=(unsigned long) totalsize / CDIO_CD_FRAMESIZE_RAW;

    return totalsectors;

}



//
// give stat of track
// just let if be a simple readonly file
//

int stat_track(struct cdfs_entry_struct *entry, struct stat *st)
{
    int nreturn=0;
    int tracknr;

    // entries are or:
    //
    // root entry, is directory, has root ino
    // OR
    // files with names track-%nr%.wav


    if ( entry->type==ENTRY_TYPE_ROOT ) {

	st->st_mode=S_IFDIR | 0555;

	st->st_nlink=2;

	st->st_blksize=CDIO_CD_FRAMESIZE_RAW;
	st->st_blocks=st->st_size / 512 + 1 ;

	st->st_uid=0; /* what here ? */
	st->st_gid=0; /* what here ? */

	st->st_size=4096; /* default size for directories */

    } else {

	tracknr=get_tracknr(entry->name);

	if ( tracknr>0 ) {

	    st->st_mode=S_IFREG | 0444; /* dealing with a read only fs */
	    st->st_nlink=1;

	    st->st_size=get_size_track(tracknr);

	    st->st_blksize=CDIO_CD_FRAMESIZE_RAW;
	    st->st_blocks=st->st_size / 512 + 1 ;

	    st->st_uid=0; /* what here ? */
	    st->st_gid=0; /* what here ? */

	} else {

	    nreturn=-ENOENT;

	}

    }

    return nreturn;

}

void write_wavheader(char *header, size_t filesize)
{
    size_t size1, size2;

    logoutput2("creating wav header: filesize : %zi", filesize);

    if ( header ) {

	memcpy(header, wavheader, SIZE_RIFFHEADER);

	size1=filesize-8;
	size2=filesize-SIZE_RIFFHEADER;

	*(header + 4) = (u_int8_t) (size1);
	*(header + 5) = (u_int8_t) (size1 >> 8);
	*(header + 6) = (u_int8_t) (size1 >> 16);
	*(header + 7) = (u_int8_t) (size1 >> 24);

	*(header + 40) = (u_int8_t) (size2);
	*(header + 41) = (u_int8_t) (size2 >> 8);
	*(header + 42) = (u_int8_t) (size2 >> 16);
	*(header + 43) = (u_int8_t) (size2 >> 24);

    }

}

//
// translate the file position (in bytes) in a track to a sector 
//

int get_sector_from_position(int tracknr, off_t pos)
{

    struct track_info_struct *track_info;

    // get the right trackinfo from the array

    track_info = (struct track_info_struct *) (cdfs_device.track_info + (tracknr - 1) * sizeof(struct track_info_struct));

    return pos / CDIO_CD_FRAMESIZE_RAW + track_info->firstsector_lsn;

}

//
// manage the read_commands
//

struct read_call_struct *get_read_call()
{
    struct read_call_struct *read_call;
    unsigned char created=0;

    if ( ! unused_read_calls ) {

	// no unused ones... : create a new one

	read_call=malloc(sizeof(struct read_call_struct));
	created=1;


    } else {

	// take from list

	read_call=unused_read_calls;

	unused_read_calls=read_call->next;
	if (unused_read_calls) unused_read_calls->prev=NULL;

    }

    if ( read_call ) {

	read_call->fuse_cdfs_thread_id=0;

	read_call->tracknr=0;
	read_call->startsector=0;
	read_call->endsector=0;

	read_call->nrsectorsread=0;
	read_call->nrsectorstoread=0;

	read_call->complete=0;

	read_call->next=NULL;
	read_call->prev=NULL;

        if ( created>0 ) {

            // only once

            pthread_mutex_init(&read_call->lockmutex, NULL);
            pthread_cond_init(&read_call->lockcond, NULL);

        }

	read_call->lock=0;

    }

    return read_call;

}


// move a read_command to the unused list...

void move_read_call_to_unused_list(struct read_call_struct *read_call)
{

    // remove first from the active list

    if ( read_call->next ) read_call->next->prev=read_call->prev;
    if ( read_call->prev ) read_call->prev->next=read_call->next;

    // insert in unused list at beginning

    if ( unused_read_calls ) {

	read_call->next=unused_read_calls;

	if ( read_call->next ) read_call->next->prev=read_call;

	read_call->prev=NULL;

    }

    unused_read_calls=read_call;

}

//
// manage the read_commands
//

struct read_command_struct *get_read_command()
{
    struct read_command_struct *read_command;

    if ( ! unused_read_commands ) {

	// no unused ones... : create a new one

	read_command=malloc(sizeof(struct read_command_struct));

    } else {

	// take from list

	read_command=unused_read_commands;

	unused_read_commands=read_command->next;
	if (unused_read_commands) unused_read_commands->prev=NULL;

    }

    if ( read_command ) {

	read_command->startsector=0;
	read_command->endsector=0;
	read_command->read_call=NULL;
	read_command->next=NULL;
	read_command->prev=NULL;

    }

    return read_command;

}


// move a read_command to the unused list...

void move_read_command_to_unused_list(struct read_command_struct *read_command)
{

    // remove first from the active list

    if ( read_command->next ) read_command->next->prev=read_command->prev;
    if ( read_command->prev ) read_command->prev->next=read_command->next;

    // insert in unused list at beginning

    if ( unused_read_commands ) {

	read_command->next=unused_read_commands;

	if ( read_command->next ) read_command->next->prev=read_command;

	read_command->prev=NULL;

    }

    unused_read_commands=read_command;

}



//
// add a read command to the queue
//
// check the interval is already in the queue: if completly than ignore
// if partly then merge
//
// todo: rewrite to add the sectors, and only create a new read_command when not merged
// like the result queue
//

void add_read_command_to_queue(struct read_command_struct *read_command)
{
    struct read_command_struct *read_command_tmp;
    struct read_command_struct *read_command_prev;
    unsigned char merged=0;


    logoutput2("add read command to queue: to read %zi to %zi", read_command->startsector, read_command->endsector);

    // lock the queue: set lock to 1

    pthread_mutex_lock(&(queue_lockmutex));

    while (queue_lock==1) {

	pthread_cond_wait(&(queue_lockcond), &(queue_lockmutex));

    }

    queue_lock=1;
    pthread_mutex_unlock(&(queue_lockmutex));


    if ( tail_queue_read_commands ) {


        // walk back to head of list, start at tail


	read_command_tmp=tail_queue_read_commands;

	while (read_command_tmp) {

            // remember the prev

	    read_command_prev=read_command_tmp->prev;

            //
	    // look at some overlap
	    // first look at cases there is no overlap at all
	    // if those are not the case then there must be some overlap
	    // excluding first the non overlap cases is far much easier then look at every
	    // possible overlap

            if ( read_command->read_call != read_command_tmp->read_call ) {

                // read command is for other track... skip
		read_command_tmp=read_command_prev;
		continue;

	    } else if ( read_command->startsector > read_command_tmp->endsector + 1 ) {

		// no overlap: too left
		read_command_tmp=read_command_prev;
		continue;

	    } else if ( read_command->endsector < read_command_tmp->startsector - 1 ) {

		// no overlap: too right
		read_command_tmp=read_command_prev;
		continue;

	    } else {

		// some overlap: merge
		// add the request to the already existing readcommand

		logoutput2("add to queue: merging with block from %i to %i", read_command_tmp->startsector, read_command_tmp->endsector);

		if ( read_command->startsector < read_command_tmp->startsector ) read_command_tmp->startsector=read_command->startsector;
		if ( read_command->endsector > read_command_tmp->endsector ) read_command_tmp->endsector=read_command->endsector;

		move_read_command_to_unused_list(read_command);
		read_command=read_command_tmp;

		merged=1;

	    }

	    read_command_tmp=read_command_prev;

	    continue;

	}

	if ( merged==0 ) {

	    // not merged somewhere..add it to the queue

	    logoutput2("add to queue: not merging... adding at tail");

	    tail_queue_read_commands->next=read_command;

	    read_command->prev=tail_queue_read_commands;

	    tail_queue_read_commands=read_command;

	    if ( ! head_queue_read_commands ) head_queue_read_commands=read_command;

	}

    } else {

	// queue was empty

	tail_queue_read_commands=read_command;
	head_queue_read_commands=read_command;

    }

    logoutput2("add to queue: ready");

    pthread_mutex_lock(&(queue_lockmutex));

    queue_lock=0;

    pthread_cond_broadcast(&(queue_lockcond));
    pthread_mutex_unlock(&(queue_lockmutex));

}

int send_read_command(struct read_call_struct *read_call, struct caching_data_struct *caching_data, unsigned int startsector, unsigned int endsector, unsigned char readaheadpolicy, unsigned char readaheadlevel)
{
    int nreturn=0;
    struct read_command_struct *read_command;

    read_command=get_read_command();

    if ( ! read_command) {

        nreturn=-ENOMEM;
	goto out;

    }

    read_command->read_call=read_call;
    read_command->caching_data=caching_data;
    read_command->startsector=startsector;
    read_command->endsector=endsector;
    read_command->readaheadpolicy=readaheadpolicy;

    if ( readaheadpolicy==READAHEAD_POLICY_PIECE ) read_command->readaheadlevel=readaheadlevel;

    add_read_command_to_queue(read_command);

    out:

    return nreturn;

}

/*
unsigned char track_is_in_queue(unsigned char tracknr)
{
    struct _struct *cached_block=tail_queue_cached_blocks;
    unsigned char isinqueue=0;

    while ( cached_block ) {

	if ( cached_block->tracknr==tracknr ) {

	    isinqueue=1;
	    break;

	}

	cached_block=cached_block->next;

    }

    return isinqueue;

}*/


void log_what_is_in_queue()
{
    struct read_command_struct *read_command;

    read_command=head_queue_read_commands;

    if ( read_command ) {

	while (read_command) {

	    logoutput2("found block (%i - %i) in queue", read_command->startsector, read_command->endsector);

	    read_command=read_command->next;

	}

    } else {

	logoutput2("no blocks in queue");

    }

}


//
// thread to read from cd , getting requests via queue
//
// TODO: howto terminate


static void *cdromreader_thread()
{
    int nreturn=0, nrsectors, nrstartsector, nrsectorsread, nrtotalsectorsread;
    struct caching_data_struct *caching_data;
    struct cached_block_struct *cached_block;
    struct read_call_struct *read_call;
    unsigned char tries1, tries2;
    bool foundincache, cachetested;
    struct read_command_struct *read_command=NULL;
    struct read_command_struct *read_command_again=NULL;
    char *buffread, *buffer;
    unsigned char cachereadlock;



    logoutput0("CDROMREADER");


    pthread_mutex_init(&queue_lockmutex, NULL);
    pthread_cond_init(&queue_lockcond, NULL);

    while (1) {


	// wait for something on the queue and free to lock

	pthread_mutex_lock(&queue_lockmutex);

	while ( queue_lock==1 || ! head_queue_read_commands) {

	    pthread_cond_wait(&queue_lockcond, &queue_lockmutex);

	}

	// set it to locked

	queue_lock=1;

	// log_what_is_in_queue();

	// queue is not empty: process the read command
	// first get the read request (to read cached_block)

	read_command=head_queue_read_commands;

	// remove from queue

	head_queue_read_commands=read_command->next;
	if ( tail_queue_read_commands == read_command ) tail_queue_read_commands=head_queue_read_commands;
	if ( head_queue_read_commands ) head_queue_read_commands->prev=NULL;

	queue_lock=0;

	pthread_cond_broadcast(&(queue_lockcond));
	pthread_mutex_unlock(&(queue_lockmutex)); /* release the queue */

	// really detach from the queue
	read_command->next=NULL;
	read_command->prev=NULL;

	// process the read request
	// first find out which file to write to
	// TODO: no read_call when a read ahead

        if ( read_command->readaheadpolicy==READAHEAD_POLICY_NONE ) {

            // not initiated for read ahead purposes
            // so there must be a read_call

            read_call=read_command->read_call;

            if ( ! read_call ) {

                move_read_command_to_unused_list(read_command);
                logoutput2("error!! original read call not found....");
                continue;

            }

        }

        //
        // determine the track
        //

	caching_data=read_command->caching_data;

	if ( ! caching_data ) {

	    logoutput2("error!! caching_data not found!! serious io error");
	    continue;

	}


	logoutput2("cdromreader: check data already available and readehead");


        foundincache=false;
        cachetested=false;
        read_command_again=NULL;
        cached_block=NULL;

        //
        // a read lock on the cache for this file/track
        //

        nreturn=get_readlock_caching_data(caching_data);

        if ( nreturn<0 ) {

	    logoutput2("error!! not able to get read lock caching_data for track %i!! serious io error", read_call->tracknr);

            // what to do here??? try again?

        } else {

            cachereadlock=(unsigned char) nreturn;

        }

        while ( ! cachetested ) {


            if ( ! cached_block ) {

                // get the first cached block...


                cached_block=get_first_cached_block_internal(caching_data, read_command->startsector, read_command->endsector);


            } else {

                cached_block=cached_block->next;

            }


            if ( ! cached_block ) {

                // nothing found in cache
                break;

            } else if ( cached_block->startsector<=read_command->startsector ) {

                // a cached block found, with startsector left of the desired sector
                // check the endsector

                if ( cached_block->endsector >= read_command->endsector ) {

                    // read_command->(startsector,endsector) is in cache

                    foundincache=true;
                    break;

                } else {

                    //
                    // check the next interval right of cached_block->endsector
                    //
                    // right of cached_block->endsector is not in cache
                    // correct the read command

                    read_command->startsector=cached_block->endsector+1;
                    continue;

                }


            } else {

                // cached_block->startsector>tmpsector

                if ( cached_block->startsector>read_command->endsector ) {

                    // the cached block found doesn't offer anything
                    // so the interval has to be read

	            break;


                } else {

                    // cached_block->startsector<=read_command->endsector

                    if ( cached_block->endsector<read_command->endsector ) {

                        read_command_again=get_read_command();

                        if ( read_command_again ) {

                            read_command_again->read_call=read_command->read_call;
                            read_command_again->caching_data=caching_data;

                            read_command_again->startsector=cached_block->endsector+1;
                            read_command_again->endsector=read_command->endsector;

                            read_command_again->readaheadlevel=read_command->readaheadlevel;
                            read_command_again->readaheadpolicy=read_command->readaheadpolicy;

                        }

                    }

                    read_command->endsector=cached_block->startsector-1;
                    break;


                }

            }

        }


        if ( cachereadlock>0 ) {

            // a read lock has been set, release it

            nreturn=release_readlock_caching_data(caching_data);

        }

        // readahead
        // put more read_commands in the queue
        // if there is read_command_again (created above) use that


        if ( read_command->readaheadpolicy==READAHEAD_POLICY_PIECE || read_command->readaheadpolicy==READAHEAD_POLICY_WHOLE ) {

            logoutput2("cdromreader: readahead");

    	    if ( ! read_command_again ) {

                if ( read_command->endsector<caching_data->endsector ) {

                    // create only when there is some space left over
                    // to readahead

                    if ( read_command->readaheadpolicy==READAHEAD_POLICY_PIECE ) {

                        if ( read_command->readaheadlevel > 0 ) {

                            // read ahead for a piece
                            // and do this only when level>0

                            read_command_again=get_read_command();

                            if ( read_command_again ) {

                                read_command_again->read_call=NULL;
                                read_command_again->caching_data=caching_data;

                                read_command_again->startsector=read_command->endsector+1;
                                read_command_again->endsector=read_command_again->startsector+READAHEAD_POLICY_PIECE_SECTORS;

                                read_command_again->readaheadpolicy=READAHEAD_POLICY_PIECE;
                                read_command_again->readaheadlevel=read_command->readaheadlevel-1;


                            }

                        }

                    } else if ( read_command->readaheadpolicy==READAHEAD_POLICY_WHOLE ) {

                        read_command_again=get_read_command();

                        if ( read_command_again ) {

                            read_command_again->read_call=NULL;
                            read_command_again->caching_data=caching_data;
                        
                            read_command_again->startsector=read_command->endsector+1;
                            read_command_again->endsector=read_command_again->startsector+READAHEAD_POLICY_WHOLE_SECTORS;

                            read_command_again->readaheadpolicy=READAHEAD_POLICY_WHOLE;
                            read_command_again->readaheadlevel=read_command->readaheadlevel;

                        }

                    }

                }


            } else {

                // there is already a read_command_again
                // adjust that
                // do not adjust the readeahead level

                if ( read_command_again->readaheadpolicy==READAHEAD_POLICY_PIECE ) {

                    if ( read_command_again->startsector + READAHEAD_POLICY_PIECE_SECTORS > read_command_again->endsector ) {

                        read_command_again->endsector = read_command_again->startsector + READAHEAD_POLICY_PIECE_SECTORS;

                    }

                } else if ( read_command_again->readaheadpolicy==READAHEAD_POLICY_WHOLE ) {

                    if ( read_command_again->startsector + READAHEAD_POLICY_WHOLE_SECTORS > read_command_again->endsector ) {

                        read_command_again->endsector = read_command_again->startsector + READAHEAD_POLICY_WHOLE_SECTORS;

                    }

                }

            }

        }


        if ( read_command_again ) {

            // do not read past last sector for this track

            if ( read_command_again->endsector > caching_data->endsector ) {

                read_command_again->endsector = caching_data->endsector;

            }

            logoutput2("cdrom reader: sending a read command (%i - %i) again", read_command_again->startsector, read_command_again->endsector);

            add_read_command_to_queue(read_command_again);

        }


        if (foundincache) {

            //
            // signal and loop
            //

            if ( read_command->read_call ) {

                // notify waiting clients ..... of course only when there is one

                notify_waiting_clients(read_command->read_call, read_command->startsector, read_command->endsector);

            }

            move_read_command_to_unused_list(read_command);

            continue;

        }

        // correction and readahead done
        // ready to process read_command

	nrsectors = read_command->endsector - read_command->startsector + 1;

	if ( nrsectors <= 0 ) {

	    // invalid 

	    logoutput2("invalid block,nrsectors %i serious io error", nrsectors);
	    move_read_command_to_unused_list(read_command);
	    continue;

	}


        // when here: open the cd using cdda when not already done

        if ( ! cdfs_device.cddevice ) {

            nreturn=open_cdrom_cdda();

            if ( nreturn<0 ) {

                nreturn=0;

                // errors opening cd 
                // this will not happen likely since the cdrom has already been opened using cdio

	        move_read_command_to_unused_list(read_command);
	        continue;

            }

	}

        // does this work..???
        // mail on list.....

        if ( cdio_cddap_speed_set(cdfs_device.cddevice, -1) ) {

	    logoutput2("setting cdrom to full speed failed...");

        } else {

	    logoutput2("cdrom set to full speed...");

        }

        // the read of the cdrom goes in batches with size probably much smaller than the size requested
        // (batch size 20110920: 25 sectors)
        // but because it's not know here how much sectors are read in one batch (and how to change that)
        // we take the "overall" size of the buffer
        // and copy the sectors read to another buffer with the right size

        size_t size=nrsectors * CDIO_CD_FRAMESIZE_RAW;

        createbuffer:

	buffer=malloc(size);

	if ( ! buffer ) {

	    // maybe retry here in case of errors

	    logoutput("error!! cannot create buffer to read cdrom.....ioerrors will be result...");
	    continue;

	}

	tries1=0;
	nrstartsector=read_command->startsector;
	nrsectorsread=0;
	nrtotalsectorsread=0;

	logoutput2("cdromreader: about to read %i sectors from sector %i", nrsectors, nrstartsector);

	readfromcd:

        // clear the buffer

        memset(buffer, '\0', size);
	nrsectorsread=cdio_cddap_read(cdfs_device.cddevice, buffer, nrstartsector, nrsectors - nrtotalsectorsread);

        if ( nrsectorsread<0 ) {

            // handle error here

            logoutput2("cdromreader: error %i reading cd", nrsectorsread);

            tries1++;

            if ( tries1>4 ) {

                // howto report back to the original caller there is an error
                //
                // just for now do nothing to report back
                //
                // just leave it for the original caller (a cdfs_read call!) to expire and leave it there
                //
                // for an incident this is ok, but for serious error, like the cdrom is not readable
                // anymore the reading should be stopped in an earlier stage than here
                // 

	        move_read_command_to_unused_list(read_command);
	        logoutput("error!! serious errors (%i) reading the cd", nrsectorsread);
	        free(buffer);
	        continue;

            }

            goto readfromcd;

        } else {


            // send the bytes to the cache

            if ( nrtotalsectorsread+nrsectorsread > nrsectors ) nrsectorsread=nrsectors-nrtotalsectorsread;
            tries2=0;

            //
            // create the buffer read, it's best to copy this to another buffer cause the writing to the cache
            // might take longer than the reading of the next buffer, overwriting existing read result
            // maybe this is nonsense but to be safe
            //

            createbufferread:

            buffread=malloc(nrsectorsread * CDIO_CD_FRAMESIZE_RAW);

            if ( ! buffread ) {

                tries2++;

                if ( tries2>4 ) {

                    // same as above
                    // no way yet to report back
                    //
                    //

                    move_read_command_to_unused_list(read_command);
                    logoutput("error!! serious errors creating buffer reading the cd");
                    free(buffer);
                    continue;

                }

                goto createbufferread;

            }

            // copy the bytes just read in the cdrom read buffer (size: nrsectors) to the 
            // result buffer (size: nrsectorsread)

            memcpy(buffread, buffer, nrsectorsread * CDIO_CD_FRAMESIZE_RAW);
            nreturn=send_read_result_to_cache(read_command->read_call, caching_data, nrstartsector, buffread, nrsectorsread);

            if ( nreturn<0 ) {

                move_read_command_to_unused_list(read_command);
                logoutput("error!! serious errors creating buffer reading the cd");
                free(buffread);
                free(buffer);

                continue;

            }

            // update counters

	    nrtotalsectorsread+=nrsectorsread;
	    nrstartsector+=nrsectorsread;

	    logoutput2("cdromreader: %i sectors read, total so far: %i, %i requested)", nrsectorsread, nrtotalsectorsread, nrsectors);

            if ( nrtotalsectorsread>=nrsectors ) {

                // ready: continue
                // note it's not necessary to free the buffread, that's freed when it's written to the cache

                move_read_command_to_unused_list(read_command);
                free(buffer);

                continue;

            }

            goto readfromcd;

	}

    }

    pthread_mutex_destroy(&queue_lockmutex);
    pthread_cond_destroy(&queue_lockcond);

}


int start_cdrom_reader_thread(pthread_t *pthreadid)
{
    int nreturn=0;

    //
    // create a thread to read the cd
    //

    nreturn=pthread_create(pthreadid, NULL, cdromreader_thread, NULL);

    if ( nreturn==-1 ) {

	// some error creating the thread

        nreturn=-errno;

	logoutput("Error creating a new thread (error: %i).", abs(nreturn));


    }

    return nreturn;

}
