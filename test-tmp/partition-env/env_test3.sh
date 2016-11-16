#!/bin/bash

. ./source_shmem
export SMA_DEBUG=foo

export SHMEM_SYMMETRIC_SIZE=32M
export SHMEM_SYMMETRIC_PARTITION1=size=32M
oshrun -np 1 ./hello
