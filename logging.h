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

#include <syslog.h>

#define logoutput(args ...) syslog(LOG_DEBUG, args)

#define logoutput0(args ...) writelog(loglevel, 0, args)
#define logoutput1(args ...) writelog(loglevel, 1, args)
#define logoutput2(args ...) writelog(loglevel, 2, args)
#define logoutput3(args ...) writelog(loglevel, 3, args)

#ifdef LOGGING
extern unsigned char loglevel;
#define writelog(arg1, arg2, ...) if ( arg1>arg2 ) syslog(LOG_DEBUG, __VA_ARGS__)
#else
static inline void dummy_nolog()
{
    return;
}
#define writelog(args ...) dummy_nolog()
#endif


