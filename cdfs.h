/*
  cdfs.h, part of cdfs, a simple overlay fs, as basis for converting media files 
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

#include <fuse/fuse_lowlevel.h>
#include <ulockmgr.h>
#include <dirent.h>

#include <cdio/paranoia/paranoia.h>
#include <cdio/paranoia/cdda.h>


typedef char pathstring[PATH_MAX+1];
typedef char smallpathstring[SMALL_PATH_MAX+1];

struct track_info_struct {
        long int firstsector_lba; /* used in discid */
        long int firstsector_lsn; /* used in overall io like filesize */
        long int lastsector;
};

struct cdfs_device_struct {
    CdIo_t *p_cdio;
    cdrom_drive_t *cddevice;
    unsigned long totalblocks;
    unsigned char nrtracks;
    unsigned char initready;
    void *track_info;
    pthread_mutex_t initmutex;
    pthread_cond_t initcond;
    pathstring discidfile;
};

struct cdfs_options_struct {
     char *cache_directory;
     char *progressfifo;
     char *hashprogram;
     char *cachehash;
     char *device;
     char *discid;
     unsigned char cachebackend;
     unsigned char logging;
     unsigned char global_nohide;
     unsigned char caching;
     unsigned char readaheadpolicy;
     unsigned char secondswaitforread;
     double attr_timeout;
     double entry_timeout;
     double negative_timeout;
     struct statvfs default_statvfs;
     struct stat default_stat;
     pathstring dbpath;
     sqlite3 *dbhandle;
};


struct cdfs_generic_fh_struct {
    struct cdfs_entry_struct *entry;
    int fd;
    void *data;
};

struct cdfs_generic_dirp_struct {
    struct cdfs_generic_fh_struct generic_fh;
    struct cdfs_entry_struct *entry;
    struct stat st;
    struct dirent direntry;
    off_t upperfs_offset;
    off_t underfs_offset;
};


/* struct for a base for every read command */

struct read_call_struct {
    pthread_t fuse_cdfs_thread_id;
    unsigned char tracknr;
    unsigned int startsector;
    unsigned int endsector;
    size_t  nrsectorstoread;
    size_t  nrsectorsread;
    unsigned char complete;
    pthread_mutex_t lockmutex;
    pthread_cond_t  lockcond;
    unsigned char   lock;
    struct read_call_struct *next;
    struct read_call_struct *prev;
    struct caching_data_struct *caching_data;
};
