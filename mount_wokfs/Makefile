# $Id$

ROOTDIR=../..
include ${ROOTDIR}/Makefile.path

PROG=		mount_wokfs
MAN+=		mount_wokfs.8

SRCS+=		ctl.c
SRCS+=		main.c
SRCS+=		${PFL_BASE}/fuse.c

MODULES+=	pthread pscfs ctl clock pfl acl dl

include ${PFLMK}

LIBPFL:=		$(call WHOLE_LIB_FLAGS,${LIBPFL})
