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

#ifndef FUSE_CDFS_CDROMUTILS_H
#define FUSE_CDFS_CDROMUTILS_H

#define CPU2LE(w,v) ((w)[0] = (u_int8_t)(v), \
                     (w)[1] = (u_int8_t)((v) >> 8), \
                     (w)[2] = (u_int8_t)((v) >> 16), \
                     (w)[3] = (u_int8_t)((v) >> 24))



/* struct for read command */

struct read_command_struct {
    unsigned char readaheadlevel;
    unsigned char readaheadpolicy;
    unsigned int startsector;
    unsigned int endsector;
    struct read_command_struct *next;
    struct read_command_struct *prev;
    struct read_call_struct *read_call;
    struct caching_data_struct *caching_data;
};


/* struct for a read result from cdromreader
 to send to the cache manager */

struct read_result_struct {
    char *buffer;
    unsigned char status;
    unsigned int startsector;
    unsigned int endsector;
    struct read_result_struct *next;
    struct read_result_struct *prev;
    struct read_call_struct *read_call;
    struct caching_data_struct *caching_data;
};

// Prototypes

// cd misc utilties

int create_track_info();
int create_discid();

// special thread to determine the hash used in the cache path

int start_do_init_in_background_thread(pthread_t *pthreadid);

int get_tracknr(const char *name);
size_t get_size_track(int tracknr);
unsigned long get_totalblocks();
int stat_track(struct cdfs_entry_struct *entry, struct stat *st);
void write_wavheader(char *header, size_t filesize);
int get_sector_from_position(int tracknr, off_t pos);

struct read_call_struct *get_read_call();
void move_read_call_to_unused_list(struct read_call_struct *read_call);

// cd rom read utilities

int send_read_command(struct read_call_struct *read_call, struct caching_data_struct *caching_data, unsigned int startsector, unsigned int endsector, unsigned char readaheadpolicy, unsigned char readaheadlevel);
int start_cdrom_reader_thread(pthread_t *pthreadid);

#endif
