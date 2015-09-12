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
#ifndef FUSE_CDFS_CACHE_COMMON_H
#define FUSE_CDFS_CACHE_COMMON_H


#define SQL_STRING_MAX_SIZE 256

#define CDFS_INTERVAL_BORDER_NONE       0
#define CDFS_INTERVAL_BORDER_START      1
#define CDFS_INTERVAL_BORDER_END        2

#define CDFS_SQLITE_BLOB_SIZE   CDIO_CD_FRAMESIZE_RAW;


/* struct to decribe the interval of data which is cached */

struct cached_block_struct {
    unsigned char tracknr;
    unsigned int startsector;
    unsigned int endsector;
    struct cached_block_struct *next;
    struct cached_block_struct *prev;
};


/* struct to describe a file to be cached */

struct caching_data_struct {
    pathstring path;
    int fd;
    unsigned char tracknr;
    unsigned int startsector;
    unsigned int endsector;
    unsigned int sectorsread;
    unsigned char ready;
    int sizeheader;
    size_t size;
    struct caching_data_struct *next;
    struct caching_data_struct *prev;
    pthread_mutex_t cachelockmutex;
    pthread_cond_t  cachelockcond;
    unsigned char cachelock;
    unsigned char nrreads;
    unsigned char writelock;
    struct cached_block_struct *cached_block;
};


// Prototypes

//
//
// locking cache functions
// only used when using linked lists in C/internal
//
//

int get_readlock_caching_data(struct caching_data_struct *caching_data);
int release_readlock_caching_data(struct caching_data_struct *caching_data);

int get_writelock_caching_data(struct caching_data_struct *caching_data);
int release_writelock_caching_data(struct caching_data_struct *caching_data);


// general cache functions

struct caching_data_struct *create_caching_data();
struct caching_data_struct *find_caching_data_by_tracknr(unsigned char tracknr);

int create_cache_file(struct caching_data_struct *caching_data, const char *name);
int write_to_cached_file(struct caching_data_struct *caching_data, char *buffer, off_t offset, size_t size);

int send_read_result_to_cache(struct read_call_struct *read_call, struct caching_data_struct *caching_data, unsigned int startsector, char *buffer, unsigned int nrsectors);
void notify_waiting_clients(struct read_call_struct *read_call, unsigned int startsector, unsigned int endsector);

int start_cache_manager_thread(pthread_t *pthreadid);

struct cached_block_struct *get_first_cached_block_internal(struct caching_data_struct *caching_data, unsigned int startsector, unsigned int endsector);
int insert_cached_block_internal(struct caching_data_struct *caching_data, unsigned int startsector, unsigned int endsector);


// sqlite functions

int create_sqlite_db(unsigned char nrtracks);
int remove_all_intervals_sqlite(unsigned char tracknr);

int write_intervals_to_sqlitedb(struct caching_data_struct *caching_data);
int write_all_intervals_to_sqlitedb();

int get_intervals_from_sqlite(struct caching_data_struct *caching_data);


#endif
