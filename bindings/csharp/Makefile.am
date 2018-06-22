## Makefile.am
##
## Copyright (C) 2018 Free Software Foundation, Inc.
##
## This file is part of LibreDWG.
##
## LibreDWG is free software: you can redistribute it and/or modify it
## under the terms of the GNU General Public License as published by
## the Free Software Foundation, either version 3 of the License, or
## (at your option) any later version.
##
## LibreDWG is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty
## of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
## See the GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program.  If not, see <http://www.gnu.org/licenses/>.

## bindings/csharp/Makefile.am
## written by Reini Urban

ACLOCAL_AMFLAGS = -I $(top_srcdir)/m4

if HAVE_SWIG

BUILT_SOURCES = swig_csharp.c
SWIG_SOURCES = $(srcdir)/../dwg.i $(top_srcdir)/include/dwg.h

dist_sharp_CSHARP = libredwg.csh
#pyexec_LTLIBRARIES = _libredwg.la

_libredwg_la_SOURCES = $(BUILT_SOURCES)  $(SWIG_SOURCES)

## more magic: SWIG_PYTHON_CPPFLAGS resolves to null and python includes
## are not passed to gcc via -I
##    _libredwg_la_CPPFLAGS = $(SWIG_PYTHON_CPPFLAGS) -I$(top_srcdir)/src
#_libredwg_la_CPPFLAGS = -I$(top_srcdir)/include $(AX_SWIG_PYTHON_CPPFLAGS)
#_libredwg_la_LDFLAGS = -module
#_libredwg_la_LIBADD = ../../src/libredwg.la

swig_sharp.c : $(SWIG_SOURCES) Makefile.am
	swig -csharp -I$(top_srcdir)/include -o $@ $(srcdir)/../dwg.i

endif
