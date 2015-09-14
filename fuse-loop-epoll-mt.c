/*
  2010, 2011 Stef Bon <stefbon@gmail.com>
  fuse-loop-epoll-mt.c
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

#define FUSE_USE_VERSION 26
#define _REENTRANT
#define _GNU_SOURCE

#define PACKAGE_VERSION "1.2"

#include <fuse/fuse_lowlevel.h>

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>
#include <assert.h>

#include <inttypes.h>
#include <ctype.h>
#include <sys/types.h>

#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <pthread.h>
#include <semaphore.h>

#define LOGGING

#include "logging.h"
#include "fuse-loop-epoll-mt.h"


//
// a thread in the pool of active threads
// to process the fuse event
// the mainloop has noticed there is data available, and read in a buffer
// but is not processed yet
//

static void *thread_to_process_fuse_event(void *threadarg)
{
	struct fuse_worker_thread_data_struct *worker_data;
	struct fuse_event_data_struct *fuse_event_data;
	bool permanentthread;
	struct global_data_struct *global_data;
	unsigned char loglevel;

	logoutput("thread started..");

	worker_data=(struct fuse_worker_thread_data_struct *) threadarg;

	if ( worker_data ) {

	    permanentthread=false;
	    loglevel=worker_data->global_data->loglevel;

	    if ( worker_data->typeworker==TYPE_WORKER_PERMANENT ) permanentthread=true;

	    worker_data->threadid=pthread_self();

	    if ( permanentthread ) {

		writelog(loglevel, 1, "perm thread %i: starting for loop..", worker_data->nr);

	    } else {

		writelog(loglevel, 1, "temp thread started..");

	    }

	    while (1) {

		// if permanent : wait for the condition
		// this is set by the mainloop to inform this thread there is data ready to process

		if ( permanentthread ) {

		    // wait for semaphore to "release"
		    sem_wait(&(worker_data->busy_sem));

		    writelog(loglevel, 1, "thread %i waking up, setting state to busy", worker_data->nr);

		    worker_data->busy=2;

		}

		fuse_event_data=worker_data->fuse_event_data;

		if ( fuse_event_data && fuse_event_data->buff ) {


		    writelog(loglevel, 1, "data found, start fuse_session_process");


		    fuse_session_process(fuse_event_data->se, fuse_event_data->buff, fuse_event_data->res, fuse_event_data->ch);

		    // reset the various data

		    free(fuse_event_data->buff);
		    free(fuse_event_data);
		    worker_data->fuse_event_data=NULL;

		}

		if ( permanentthread ) {

		    writelog(loglevel, 1, "thread %i: set state to not busy", worker_data->nr);

		    worker_data->busy=0;

		} else {

		    writelog(loglevel, 1, "temp thread ready, exit");

		    break;

		}

	    }

	    if ( ! permanentthread ) {

		// freeing the worker data when temp thread
		// the buff is already freed..
		// free 
		// remove itself from list of temp threads

		global_data=worker_data->global_data;

		if ( worker_data->prev ) worker_data->prev->next=worker_data->next;
		if ( worker_data->next ) worker_data->next->prev=worker_data->prev;
		if ( global_data->temp_worker_data==worker_data) global_data->temp_worker_data=worker_data->next;

		free(worker_data);

	    }

	}

	logoutput("thread ending");

	pthread_exit(NULL);

}

static struct fuse_event_data_struct *create_fuse_event_data(struct fuse_epoll_data_struct *fuse_epoll_data)
{
	struct fuse_event_data_struct *fuse_event_data;
	unsigned ncount=0;

	while (ncount<4) {

	    fuse_event_data=malloc(sizeof(struct fuse_event_data_struct));

	    if ( fuse_event_data ) break;

	    ncount++;

	}

	if ( fuse_event_data ) {

	    ncount=0;
	    fuse_event_data->buffsize=fuse_epoll_data->buffsize;

	    while (ncount<4) {

		fuse_event_data->buff=malloc(fuse_event_data->buffsize);

		if (fuse_event_data->buff) break;

		ncount++;

	    }

	    if ( ! fuse_event_data->buff ) {

		free(fuse_event_data);
		fuse_event_data=NULL;

	    } else {

		memcpy(fuse_event_data->buff, fuse_epoll_data->buff, fuse_event_data->buffsize);
		fuse_event_data->ch=fuse_epoll_data->ch;
		fuse_event_data->se=fuse_epoll_data->se;
		fuse_event_data->res=fuse_epoll_data->res;

	    }

	}

	return fuse_event_data;

}

int fuse_session_loop_epoll_mt(struct fuse_session *se, unsigned char loglevel)
{
	int epoll_fd, signal_fd, fuse_fd;
	struct epoll_event epoll_events[MAX_EPOLL_NREVENTS];
	int i, res, nextfreethread=0, ncount;
	struct fuse_chan *ch = NULL;
	ssize_t readlen;
	struct signalfd_siginfo fdsi;
	int signo = 0, nreturn = 0, nerror = 0;
	sigset_t fuse_sigset;
	struct fuse_epoll_data_struct *fuse_epoll_data;
	struct fuse_worker_thread_data_struct fuse_worker_data[NUM_WORKER_THREADS];
	struct fuse_worker_thread_data_struct *tmp_worker_data;
	struct fuse_event_data_struct *fuse_event_data;
	pthread_t pthreadid;
	bool threadfound=false;
	struct global_data_struct global_data;

	// init mainloop data

	global_data.epoll_data=NULL;
	global_data.temp_worker_data=NULL;
	global_data.loglevel=loglevel;

	// create an epoll instance

	epoll_fd=epoll_create(MAX_EPOLL_NRFDS);

	// walk through all the channels and add the assiciated fd to the epoll

	ch = fuse_session_next_chan(se, NULL);

	while (ch) {

	    // determine the fd of the channel

	    fuse_fd=fuse_chan_fd(ch);

	    fuse_epoll_data=malloc(sizeof(struct fuse_epoll_data_struct));

	    if ( fuse_epoll_data ) {

		memset(fuse_epoll_data, 0, sizeof(struct fuse_epoll_data_struct));

		fuse_epoll_data->type_fd=TYPE_FD_FUSE;
		fuse_epoll_data->fd=fuse_fd;
		fuse_epoll_data->ch=ch;
		fuse_epoll_data->se=se;
		fuse_epoll_data->buffsize=fuse_chan_bufsize(fuse_epoll_data->ch);
		fuse_epoll_data->buff=(char *) malloc(fuse_epoll_data->buffsize);

		// add epoll_data to list (insert at begin)

		fuse_epoll_data->next=global_data.epoll_data;
		global_data.epoll_data=fuse_epoll_data;

		if ( ! fuse_epoll_data->buff ) {

		    // here some freeing of data?

		    nreturn=-ENOMEM;
		    goto out;

		}

	    } else {

		// here free data??

		res=-ENOMEM;
		goto out;

	    }

	    // add this fd to the epoll instance

	    static struct epoll_event epoll_fuse_instance;

	    epoll_fuse_instance.events=EPOLLIN;
	    epoll_fuse_instance.data.ptr=(void *) fuse_epoll_data;

	    res=epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fuse_fd, &epoll_fuse_instance);

	    if ( res==-1 ) {

		nreturn=-errno;
		goto out;

	    }

	    ch = fuse_session_next_chan(se, ch);

	}

	// set the set of signals for signalfd to listen to

	sigemptyset(&fuse_sigset);

	sigaddset(&fuse_sigset, SIGINT);
	sigaddset(&fuse_sigset, SIGIO);
	sigaddset(&fuse_sigset, SIGHUP);
	sigaddset(&fuse_sigset, SIGTERM);
	sigaddset(&fuse_sigset, SIGPIPE);
	sigaddset(&fuse_sigset, SIGCHLD);
	sigaddset(&fuse_sigset, SIGUSR1);

	signal_fd = signalfd(-1, &fuse_sigset, SFD_NONBLOCK);

	if (signal_fd == -1) {

	  nreturn=-errno;

	  logoutput("mainloop: unable to create signalfd, error: %i", nreturn);

	  goto out;

	}

	if (sigprocmask(SIG_BLOCK, &fuse_sigset, NULL) == -1) {

	  logoutput("mainloop: error sigprocmask");

	  goto out;

	}

	writelog(loglevel, 1, "mainloop: adding signalfd %i to epoll", signal_fd);

	fuse_epoll_data=malloc(sizeof(struct fuse_epoll_data_struct));

	if ( fuse_epoll_data ) {

	    memset(fuse_epoll_data, 0, sizeof(struct fuse_epoll_data_struct));

	    fuse_epoll_data->type_fd=TYPE_FD_SIGNAL;
	    fuse_epoll_data->fd=signal_fd;

	} else {

	    res=-ENOMEM;
	    goto out;

	}


	static struct epoll_event epoll_signalfd_instance;
	epoll_signalfd_instance.events=EPOLLIN;
	epoll_signalfd_instance.data.ptr=( void *) fuse_epoll_data;

	res=epoll_ctl(epoll_fd, EPOLL_CTL_ADD, signal_fd, &epoll_signalfd_instance);

	if ( res==-1 ) {

	  nreturn=-errno;
	  goto out;

	}

	// init and fire up the different worker threads

	for (i=0; i<NUM_WORKER_THREADS; i++) {

	    res=sem_init(&(fuse_worker_data[i].busy_sem), 0, 0);

	    if ( res!=0 ) {

		// error

		writelog(loglevel, 0, "Error creating a semaphore for thread(nr: %i, error: %i).", i, res);

		goto out;

	    }

	    fuse_worker_data[i].busy=0;
	    fuse_worker_data[i].nr=i;
	    fuse_worker_data[i].typeworker=TYPE_WORKER_PERMANENT;

	    fuse_worker_data[i].fuse_event_data=NULL;
	    fuse_worker_data[i].global_data=&global_data;

	    writelog(loglevel, 1, "creating a new thread: %i.", i);

	    res=pthread_create(&pthreadid, NULL, thread_to_process_fuse_event, (void *) &fuse_worker_data[i]);

	    if ( res!=0 ) {

		// error

		logoutput("Error creating a new thread (nr: %i, error: %i).", i, res);

		goto out;

	    }

	}


	writelog(loglevel, 0, "mainloop: starting epoll wait loop");


	while (1) {


	    int number_of_fds=epoll_wait(epoll_fd, epoll_events, MAX_EPOLL_NREVENTS, -1);

	    if (number_of_fds < 0) {

		nreturn=-errno;

		writelog(loglevel, 0, "mainloop: epoll_wait error");

		goto out;

	    }


	    for (i=0; i<number_of_fds; i++) {

		fuse_epoll_data=(struct fuse_epoll_data_struct *) epoll_events[i].data.ptr;

		// look what kind of fd this is

		if ( fuse_epoll_data->type_fd==TYPE_FD_FUSE ) {


		    // it a fuse thing
		    // create a new request to handle this event the first free thread

		    // check first it;s not exit

		    if ( fuse_session_exited(se) ) goto out;

		    memset(fuse_epoll_data->buff, 0, fuse_epoll_data->buffsize);
		    res=fuse_chan_recv(&fuse_epoll_data->ch, fuse_epoll_data->buff, fuse_epoll_data->buffsize);

		    if ( res>0 ) {

			fuse_epoll_data->res=res;

			// copy the data to a new event data 

			fuse_event_data=create_fuse_event_data(fuse_epoll_data);

			if ( ! fuse_event_data ) {

			    nreturn=-ENOMEM;
			    goto out;

			}

			threadfound=false;
			ncount=0;

			while ( ! threadfound ) {

			    if ( fuse_worker_data[nextfreethread].busy==0 ) {

				fuse_worker_data[nextfreethread].fuse_event_data=fuse_event_data;

				// not busy, activate ("verhogen") the sem

				writelog(loglevel, 0, "mainloop: sem post for thread %i", nextfreethread);

				res=sem_post(&(fuse_worker_data[nextfreethread].busy_sem));

				if ( res==0 ) {

				    threadfound=true;

				} else {

				    // some error, just try the next worker thread

				    writelog(loglevel, 0, "mainloop: sem post failed");

				    fuse_worker_data[nextfreethread].fuse_event_data=NULL;

				}

			    }

			    nextfreethread++;
			    ncount++;

			    if ( nextfreethread>=NUM_WORKER_THREADS ) {

				nextfreethread=0;

			    }

			    // stop if found or all the threads are tried

			    if ( threadfound || ncount>=NUM_WORKER_THREADS) break;

			}

			if ( ! threadfound) {

			    // no free thread found in the pool
			    // start a temp thread especially for this purpose

			    writelog(loglevel, 0, "mainloop: no free thread found, starting a temp thread");

			    tmp_worker_data=malloc(sizeof(struct fuse_worker_thread_data_struct));

			    if ( tmp_worker_data ) {

				tmp_worker_data->busy=0;
				tmp_worker_data->nr=0;
				tmp_worker_data->typeworker=TYPE_WORKER_TEMPORARY;
				tmp_worker_data->fuse_event_data=fuse_event_data;
				tmp_worker_data->global_data=&global_data;

				// insert at beginning

				tmp_worker_data->prev=NULL;

				if ( global_data.temp_worker_data ) {

				    global_data.temp_worker_data->prev=tmp_worker_data;
				    tmp_worker_data->next=global_data.temp_worker_data;

				} else {

				    tmp_worker_data->next=NULL;

				}

				global_data.temp_worker_data=tmp_worker_data;

				res=pthread_create(&pthreadid, NULL, thread_to_process_fuse_event, (void *) tmp_worker_data);

				if ( res!=0 ) {

				    free(fuse_event_data->buff);
				    free(fuse_event_data);
				    free(tmp_worker_data);

				    writelog(loglevel, 0, "mainloop: cannot start temp thread");

				} 

			    } else {

				writelog(loglevel, 0, "mainloop: cannot allocate space for temp thread");

			    }

			}


		    } else {

			writelog(loglevel, 0, "mainloop: error reading fuse event, %i", res);

		    }

		    if ( fuse_session_exited(se) ) goto out;


		} else if ( fuse_epoll_data->type_fd==TYPE_FD_SIGNAL ) {


                    //
		    // some data on signalfd
		    //

		    writelog(loglevel, 0, "mainloop: in signal loop");

		    readlen=read(signal_fd, &fdsi, sizeof(struct signalfd_siginfo));
		    nerror=errno;

		    if ( readlen==-1 ) {

			if ( nerror==EAGAIN ) {

                            // blocking error: back to the mainloop

			    continue;

                        }

			writelog(loglevel, 0, "error %i reading from signalfd......", nerror);

		    } else {

			if ( readlen == sizeof(struct signalfd_siginfo)) {

		    	    // check the signal

		    	    signo=fdsi.ssi_signo;

		    	    if ( signo==SIGHUP || signo==SIGINT || signo==SIGTERM ) {

				writelog(loglevel, 0, "mainloop: caught signal %i, exit session", signo);

				fuse_session_exit(se);

		    	    } else if ( signo==SIGPIPE ) {

				signo=0;

				writelog(loglevel, 0, "mainloop: caught signal SIGPIPE, ignoring");

	            	    } else if ( signo == SIGCHLD) {

		        	writelog(loglevel, 0, "Got SIGCHLD, from pid: %d", fdsi.ssi_pid);

		        	// look at the pid of the child with waitpid for preventing zombies

		        		waitpid(fdsi.ssi_pid, NULL, WNOHANG);

	            	    } else if ( signo == SIGIO) {

		        	writelog(loglevel, 0, "Got SIGIO.....");

		    	    } else {

				writelog(loglevel, 0, "got unknown signal %i", signo);

			    }

			}

		    }

		}

	    }

	}

	out:


	// stop any thread here

	for (i=0; i<NUM_WORKER_THREADS; i++) {

	    pthread_cancel(fuse_worker_data[i].threadid);
	    sem_destroy(&(fuse_worker_data[i].busy_sem));

	}


	// the epoll data associated with every channel

	fuse_epoll_data=global_data.epoll_data;

	while (fuse_epoll_data) {

	    fuse_epoll_data=fuse_epoll_data->next;

	    if (global_data.epoll_data->buff) free(global_data.epoll_data->buff);
	    free(global_data.epoll_data);

	    global_data.epoll_data=fuse_epoll_data;

	}

	// what to do with temp threads??

	tmp_worker_data=global_data.temp_worker_data;

	while (tmp_worker_data) {

	    // this is safe?
	    pthread_cancel(tmp_worker_data->threadid);

	    tmp_worker_data=tmp_worker_data->next;

	    free(global_data.temp_worker_data);
	    global_data.temp_worker_data=tmp_worker_data;

	}


	fuse_session_reset(se);

	return nreturn < 0 ? -1 : 0;
}
