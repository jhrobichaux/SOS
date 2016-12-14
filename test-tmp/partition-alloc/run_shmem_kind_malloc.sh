#!/bin/bash

#export SMA_DEBUG=foo

export SHMEM_SYMMETRIC_PARTITION1=size=32M:pgsize=4K
export SHMEM_SYMMETRIC_PARTITION2=size=32M:pgsize=4K
export SHMEM_SYMMETRIC_PARTITION3=size=32M:pgsize=4K
export SHMEM_SYMMETRIC_PARTITION4=size=32M:pgsize=4K
export SHMEM_SYMMETRIC_PARTITION5=size=32M:pgsize=4K
export SHMEM_SYMMETRIC_PARTITION6=size=32M:pgsize=4K
export SHMEM_SYMMETRIC_PARTITION7=size=32M:pgsize=4K
oshrun -np 2 ./shmem_kind_malloc  -v
