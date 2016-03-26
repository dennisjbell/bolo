#
#  Copyright 2015 James Hunt <james@jameshunt.us>
#
#  This file is part of Bolo.
#
#  Bolo is free software: you can redistribute it and/or modify it under the
#  terms of the GNU General Public License as published by the Free Software
#  Foundation, either version 3 of the License, or (at your option) any later
#  version.
#
#  Bolo is distributed in the hope that it will be useful, but WITHOUT ANY
#  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
#  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
#  details.
#
#  You should have received a copy of the GNU General Public License along
#  with Bolo.  If not, see <http://www.gnu.org/licenses/>.
#

AC_PREREQ(2.63)
AC_INIT([Bolo], [m4_esyscmd([./version.sh])], [bolo@niftylogic.com],, [http://bolo.niftylogic.com])

AC_CONFIG_SRCDIR([src/bolo.h])
AC_CONFIG_AUX_DIR([build])
AC_CONFIG_MACRO_DIR([build])
AC_CONFIG_HEADERS(config.h)

AM_INIT_AUTOMAKE([-Wall -Werror foreign subdir-objects])

AX_TEST_COVERAGE

AC_PROG_CC
AC_CHECK_HEADERS(stdint.h)

BOLO_WITH([postgres], [the PostgreSQL libraries (libpq) and header files])
BOLO_WITH([hiredis],  [the HiRedis libraries (libhiredis) and header files])

AC_HAVE_LIBRARY(pthread,,  AC_MSG_ERROR(Missing pthread library))
AC_HAVE_LIBRARY(vigor,,    AC_MSG_ERROR(Missing libvigor - see http://github.com/jhunt/libvigor))
AC_HAVE_LIBRARY(zmq,,      AC_MSG_ERROR(Missing 0MQ library))
AC_HAVE_LIBRARY(pcre,,     AC_MSG_ERROR(Missing PCRE library))

build_ALL=auto
AC_ARG_WITH([all-subscribers],
	[AS_HELP_STRING([--with-all-subscribers],
		[Build all optional subscriber programs (--with-X-subscriber)])],
	[echo "triggered build_ALL logic"
	 case "${withval}" in
	 yes)  build_ALL=yes;  AC_MSG_NOTICE([Will build ALL optional subscribers])            ;;
	 no)   build_ALL=no;   AC_MSG_NOTICE([Will not build any optional subscribers])        ;;
	 auto) build_ALL=auto; AC_MSG_NOTICE([Will attempt to build ALL optional subscribers]) ;;
	 *)    AC_MSG_ERROR([bad value ${withval} for --with-all-subscribers]) ;;
	 esac],
	[build_ALL=auto])

BOLO_SUBSCRIBER([rrd], [auto], [stores sample data in round-robin database files],
	[#define HAVE_STDINT_H
	 #include <rrd.h>
	 int main() { return 0; }
	], [-lrrd_th])

BOLO_SUBSCRIBER([pg], [auto], [stores sample and state data in a Postgres database],
	[#include <libpq-fe.h>
	 int main() { return 0; }
	], [-lpq])

BOLO_SUBSCRIBER([sqlite], [auto], [stores sample and state data in a SQLite3 database],
	[#include <sqlite3.h>
	 int main() { return 0; }
	], [-lsqlite3])

BOLO_SUBSCRIBER([redis], [auto], [stores KEY data in a redis database],
	[#include <hiredis/hiredis.h>
	 int main() { return 0; }
	], [-lhiredis])

BOLO_SUBSCRIBER([influxdb], [auto], [stores sample and state data in InfluxDB],
	[#include <stdlib.h>
	 int main() { return 0; }
	], [])

x=0
AC_CHECK_LIB(pcre, pcre_free_study, [x=1], [x=0])
if test $x == 1
then
	AC_DEFINE([HAVE_PCRE_FREE_STUDY], 1, [PCRE library exports pcre_free_study()])
else
	AC_DEFINE([HAVE_PCRE_FREE_STUDY], 0, [PCRE library exports pcre_free_study()])
fi

AC_CONFIG_FILES([Makefile rpm.spec include/bolo.h])
AC_OUTPUT
