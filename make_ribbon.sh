#!/bin/bash -e

for bits in 8 16 32 9 17 33; do
	make RIBBON_BITS=$i
done
