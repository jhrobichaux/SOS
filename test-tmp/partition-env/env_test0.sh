#!/bin/bash
# Simple test just prints out information if SMA_DEBUG is set. 
. ./source_shmem
export SMA_DEBUG=foo

oshrun -np 1 ./hello
