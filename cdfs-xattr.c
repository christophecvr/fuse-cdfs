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

#ifndef ENOATTR
#define ENOATTR ENODATA        /* No such attribute */
#endif

#include <sqlite3.h>

#include <fuse/fuse_lowlevel.h>

#include "logging.h"
#include "cdfs.h"

#include "entry-management.h"
#include "cdfs-xattr.h"


extern struct cdfs_options_struct cdfs_options;


int setxattr4workspace(struct cdfs_entry_struct *entry, const char *name, const char *value)
{
    int nvalue, nreturn=-ENOATTR;

    if ( entry->type == ENTRY_TYPE_ROOT ) {

	// setting system values only on root entry

	if ( strcmp(name, "logging")==0 ) {

	    nvalue=atoi(value);

	    if ( nvalue>=0 ) {

		logoutput1("setxattr: value found %i", nvalue);

		loglevel=nvalue;
		nreturn=0;

	    } else {

		nreturn=-EINVAL;

	    }

	} else if ( strcmp(name, "global_nohide")==0 ) {

	    nvalue=atoi(value);

	    if ( nvalue==0 || nvalue==1 ) {

		logoutput1("setxattr: value found %i", nvalue);
                nreturn=0;
		cdfs_options.global_nohide=nvalue;

	    } else {

		nreturn=-EINVAL;

	    }

	} else if ( strcmp(name, "caching")==0 ) {

	    nvalue=atoi(value);

	    if ( nvalue==0 || nvalue==1 ) {

		logoutput1("setxattr: value found %i", nvalue);
                nreturn=0;
		cdfs_options.caching=nvalue;

	    } else {

		nreturn=-EINVAL;

	    }

	} else if ( strcmp(name, "readaheadpolicy")==0 ) {

	    nvalue=atoi(value);

	    if ( nvalue==READAHEAD_POLICY_NONE || nvalue== READAHEAD_POLICY_PIECE|| READAHEAD_POLICY_WHOLE ) {

		logoutput1("setxattr: value found %i", nvalue);
                nreturn=0;
		cdfs_options.readaheadpolicy=nvalue;

	    } else {

		nreturn=-EINVAL;

	    }

	}

    }

    if ( strcmp(name, "entry_hide")==0 ) {

	nvalue=atoi(value);

	if ( nvalue==0 || nvalue==1 ) {

	    logoutput1("setxattr: value found %i", nvalue);
            nreturn=0;
	    entry->hide=nvalue;

	} else {

	    nreturn=-EINVAL;

	}

    }


    return nreturn;

}

static void fill_in_simpleinteger(struct xattr_workspace_struct *xattr_workspace, int somenumber)
{
    char smallstring[10];

    xattr_workspace->nlen=snprintf(smallstring, 9, "%i", somenumber);

    if ( xattr_workspace->size>0 ) {

	if ( xattr_workspace->size > xattr_workspace->nlen ) {

	    xattr_workspace->value=malloc(xattr_workspace->size);

	    if ( ! xattr_workspace->value ) {

		xattr_workspace->nerror=-ENOMEM;

	    } else {

		memcpy(xattr_workspace->value, &smallstring, xattr_workspace->nlen);
		*((char *) xattr_workspace->value+xattr_workspace->nlen) = '\0';

	    }

	}

    }

}


/*static void fill_in_simplestring(struct xattr_workspace_struct *xattr_workspace, char *somestring)
{
    xattr_workspace->nlen=strlen(somestring);

    if ( xattr_workspace->size>0 ) {

	if ( xattr_workspace->size > xattr_workspace->nlen ) {

	    xattr_workspace->value=malloc(xattr_workspace->size);

	    if ( ! xattr_workspace->value ) {

		xattr_workspace->nerror=-ENOMEM;

	    } else {

		memcpy(xattr_workspace->value, somestring, xattr_workspace->nlen);
		*((char *) xattr_workspace->value+xattr_workspace->nlen) = '\0';

	    }

	}

    }

}*/



void getxattr4workspace(struct cdfs_entry_struct *entry, const char *name, struct xattr_workspace_struct *xattr_workspace)
{

    xattr_workspace->nerror=-ENOATTR; /* start with attr not found */

    logoutput2("getxattr4workspace, name: %s, size: %i", name, xattr_workspace->size);


    if ( entry->type == ENTRY_TYPE_ROOT ) {

	// only the system related

	if ( strcmp(name, "logging")==0 ) {

            logoutput2("getxattr4workspace, found: logging");

	    xattr_workspace->nerror=0;

	    fill_in_simpleinteger(xattr_workspace, (int) loglevel);

	} else if ( strcmp(name, "global_nohide")==0 ) {

            logoutput2("getxattr4workspace, found: global_nohide");

	    xattr_workspace->nerror=0;

	    fill_in_simpleinteger(xattr_workspace, (int) cdfs_options.global_nohide);

	} else if ( strcmp(name, "caching")==0 ) {

            logoutput2("getxattr4workspace, found: caching");

	    xattr_workspace->nerror=0;

	    fill_in_simpleinteger(xattr_workspace, (int) cdfs_options.caching);

	} else if ( strcmp(name, "readaheadpolicy")==0 ) {

            logoutput2("getxattr4workspace, found: readaheadpolicy");

	    xattr_workspace->nerror=0;

	    fill_in_simpleinteger(xattr_workspace, (int) cdfs_options.readaheadpolicy);

	} 

    }


    if ( strcmp(name, "entry_hide")==0 ) {

        logoutput2("getxattr4workspace, found: entry_nohide");

	xattr_workspace->nerror=0;

	fill_in_simpleinteger(xattr_workspace, (int) entry->hide);

    }

}

//
// generalized utility to add a xattr name to list, used by listxattr4workspace
//

static int add_xattr_to_list(struct xattr_workspace_struct *xattr_workspace, char *list)
{
    unsigned nlenxattr=0;

    nlenxattr=strlen(xattr_workspace->name);

    // logoutput2("add_xattr_to_list, name : %s, size: %zd, len so far: %i", xattr_workspace->name, xattr_workspace->size, xattr_workspace->nlen);

    if ( xattr_workspace->size==0 ) {

	// just increase

	xattr_workspace->nlen+=nlenxattr+1;

    } else {

	// check the value fits (including the trailing \0)

	if ( xattr_workspace->nlen+nlenxattr+1 <= xattr_workspace->size ) {

	    memcpy(list+xattr_workspace->nlen, xattr_workspace->name, nlenxattr);

	    xattr_workspace->nlen+=nlenxattr;

	    *(list+xattr_workspace->nlen)='\0';

	    xattr_workspace->nlen+=1;

	} else {

	    // does not fit... return the size, calling proc will detect this is bigger than size

	    xattr_workspace->nlen+=nlenxattr+1;

	}

    }

    return xattr_workspace->nlen;

}


int listxattr4workspace(struct cdfs_entry_struct *entry, char *list, size_t size)
{
    unsigned nlenlist=0;
    struct xattr_workspace_struct *xattr_workspace;

    xattr_workspace=malloc(sizeof(struct xattr_workspace_struct));

    if ( ! xattr_workspace ) {

	nlenlist=-ENOMEM;
	goto out;

    }

    memset(xattr_workspace, 0, sizeof(struct xattr_workspace_struct));

    xattr_workspace->name=malloc(LINE_MAXLEN);

    if ( ! xattr_workspace->name ) {

	nlenlist=-ENOMEM;
	free(xattr_workspace);
	goto out;

    }

    xattr_workspace->size=size;
    xattr_workspace->nerror=0;
    xattr_workspace->value=NULL;
    xattr_workspace->nlen=0;

    if ( ! list ) size=0;


    // system related attributes, only available on the file .system

    if ( entry->type==ENTRY_TYPE_ROOT ) {

	// level of logging

	memset(xattr_workspace->name, '\0', LINE_MAXLEN);
	snprintf(xattr_workspace->name, LINE_MAXLEN, "system.%s_logging", XATTR_SYSTEM_NAME);

	nlenlist=add_xattr_to_list(xattr_workspace, list);
	if ( size > 0 && nlenlist > size ) goto out;

	// do not hide anything or not

	memset(xattr_workspace->name, '\0', LINE_MAXLEN);
	snprintf(xattr_workspace->name, LINE_MAXLEN, "system.%s_global_nohide", XATTR_SYSTEM_NAME);

	nlenlist=add_xattr_to_list(xattr_workspace, list);
	if ( size > 0 && nlenlist > size ) goto out;

	// do caching or not

	memset(xattr_workspace->name, '\0', LINE_MAXLEN);
	snprintf(xattr_workspace->name, LINE_MAXLEN, "system.%s_caching", XATTR_SYSTEM_NAME);

	nlenlist=add_xattr_to_list(xattr_workspace, list);
	if ( size > 0 && nlenlist > size ) goto out;

	// readaheadpolicy

	memset(xattr_workspace->name, '\0', LINE_MAXLEN);
	snprintf(xattr_workspace->name, LINE_MAXLEN, "system.%s_readaheadpolicy", XATTR_SYSTEM_NAME);

	nlenlist=add_xattr_to_list(xattr_workspace, list);
	if ( size > 0 && nlenlist > size ) goto out;

    }

    memset(xattr_workspace->name, '\0', LINE_MAXLEN);
    snprintf(xattr_workspace->name, LINE_MAXLEN, "system.%s_entry_hide", XATTR_SYSTEM_NAME);

    nlenlist=add_xattr_to_list(xattr_workspace, list);
    if ( size > 0 && nlenlist > size ) goto out;

    out:

    return nlenlist;

}
