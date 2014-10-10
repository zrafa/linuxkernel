#!/bin/sh
#
# treelink.sh - Tree Linking Script
#
# Copyright (C) 2001, 2004 Paul Mundt <lethal@linux-sh.org>
#
# Modified by M. R. Brown <mrbrown@0xd6.org>
#
# A simple shell script for linking a drop in tree into a stock
# kernel tree. Usable for drop in trees such as the linux-mips
# and linuxconsole trees.
#
# Released under the terms of the GNU GPL v2
#
[ "$#" -ne "2" ] && echo "Usage: $0 <drop in tree> <kernel tree>" && exit 1

ODIR=${PWD}
cd $1 || exit 1
LDIR=${PWD}
echo -n "Building file list ... "
LIST=`find * \( -type d -name CVS -prune \) -o \
	     \( -type d -name {arch} -prune \) -o \
	     \( -type d -name .arch-ids -prune \) -o \
	     \( -type d -name SCCS -prune \) -o \
	     \( -type d -name BitKeeper -prune \) -o -type f -print`
echo -e "done."
cd ${ODIR}

cd $2 || exit 1

# Make this a seperate step so that the user can cancel the operation
echo -n "Saving originals ..... "
for file in $LIST; do
	if [ -e $file  -a  ! -h $file ]; then
		DIR=`dirname $file`
		ofile=`basename $file`
		[ ! -d ${DIR}/.orig ] && mkdir -p ${DIR}/.orig
		cp $file ${DIR}/.orig/$ofile
	fi
done
echo -e "done."

echo -n "Linking files ........ "
for file in $LIST; do
	DIR=`dirname $file`
	[ ! -d $DIR ] && mkdir -p $DIR
	ln -sf $LDIR/$file $file
done
echo -e "done."
