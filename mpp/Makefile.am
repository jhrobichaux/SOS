# -*- Makefile -*-
#
# Copyright 2011 Sandia Corporation. Under the terms of Contract
# DE-AC04-94AL85000 with Sandia Corporation, the U.S.  Government
# retains certain rights in this software.
# 
# Copyright (c) 2015 Intel Corporation. All rights reserved.
# This software is available to you under the BSD license.
#
# This file is part of the Sandia OpenSHMEM software package. For license
# information, see the LICENSE file in the top level directory of the
# distribution.
#

# shmem.h should appear in BOTH $(includedir)/shmem.h and $(includedir)/mpp/shmem.h!
mppdir = $(includedir)/mpp

mpp_HEADERS = shmem.h shmemx.h pshmem.h
include_HEADERS = shmem.h shmemx.h pshmem.h

if HAVE_FORTRAN
mpp_HEADERS += shmem.fh shmemx.fh
include_HEADERS += shmem.fh shmemx.fh
endif
