#!/bin/bash

for((i=1; i <= 64; i++)); do make RIBBON_BITS=$i; done
