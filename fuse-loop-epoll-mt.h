/*
  2010, 2011 Stef Bon <stefbon@gmail.com>
  fuse-loop-epoll-mt.h
  This is an alternative main eventloop for the fuse filesystem using epoll and threads.

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

#ifndef FUSE_LOOP_EPOLL_MT_H
#define FUSE_LOOP_EPOLL_MT_H

// epoll parameters
#define MAX_EPOLL_NREVENTS 		32
#define MAX_EPOLL_NRFDS			32

// types of fd's
#define TYPE_FD_SIGNAL			1
#define TYPE_FD_FUSE			2
#define TYPE_FD_TIMER			3

// types of threads
#define TYPE_WORKER_PERMANENT		1
#define TYPE_WORKER_TEMPORARY		2

// number of threads
#ifndef NUM_WORKER_THREADS
#define NUM_WORKER_THREADS		10
#endif

#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <pthread.h>
#include <semaphore.h>

// struct to identify the fd when epoll singals activity on that fd

struct fuse_epoll_data_struct {
	int type_fd;
	int fd;
	struct fuse_chan *ch;
	struct fuse_session *se;
	size_t buffsize;
	char *buff;
	int res;
	struct fuse_epoll_data_struct *next;
};

// struct for the threads (permanent and temporary)

struct fuse_worker_thread_data_struct {
	pthread_t threadid;
	int busy;
	int nr;
	int typeworker;
	sem_t busy_sem;
	struct fuse_event_data_struct *fuse_event_data;
	struct fuse_worker_thread_data_struct *next;
	struct fuse_worker_thread_data_struct *prev;
	struct global_data_struct *global_data;
};

// struct to store the fuse event in when receiving data on fuse channel

struct fuse_event_data_struct {
	struct fuse_chan *ch;
	struct fuse_session *se;
	char *buff;
	size_t buffsize;
	int res;
};

// struct with all the global data, used to have a reference to it in threads

struct global_data_struct {
	struct fuse_epoll_data_struct *epoll_data;
	struct fuse_worker_thread_data_struct *temp_worker_data;
	unsigned char loglevel;
};


// Prototypes

int fuse_session_loop_epoll_mt(struct fuse_session *se, unsigned char loglevel);


#endif

