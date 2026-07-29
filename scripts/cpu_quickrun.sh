#!/bin/bash

test -d tmp && rm -r tmp
mkdir tmp
/usr/bin/time --verbose ./hifiasm -t1 -o tmp/temprun.asm ../data/chr22_40k.fastq 2> tmp/log