#!/bin/bash

. ./source_shmem
export SMA_DEBUG=foo

export SHMEM_SYMMETRIC_PARTITION130=size=128M:pgsize=4K
export SHMEM_SYMMETRIC_PARTITION2=size=64M:pgsize=4K
export SHMEM_SYMMETRIC_PARTITION1=size=32M:pgsize=2M
oshrun -np 1 ./hello
