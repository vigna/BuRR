#!/bin/bash

LOGDIR=../logs/par2
LOG=${LOGDIR}/log
AGGLOG=${LOGDIR}/result_agg
ALLLOG=${LOGDIR}/result_all

mkdir -p ${LOGDIR}

ulimit -c 0

# 10^8 queries per thread
numq=100000000
# use 10 million items for L=64 and 128, 1 million for L=32
declare -a MKEYS=([32]=1 [64]=2 [128]=10)
declare -a DEPTH=([32]=3 [64]=2 [128]=2)
declare -a MODEARGS=([0]='' [1]='-1' [2]='-2')

# round up power of two
pow2up() {
    if [ $1 -eq 1 ]; then
        echo "1";
    else
        echo "x=l($1-1)/l(2); scale=0; 2^(x/1 + 1)" | bc -l;
    fi
}

if [ -z $1 ]; then
    # MB of free memory
    MEMAVAIL=$( free -m | grep Mem | awk '{print $4; }' )
    # leave 1GB free for misc overhead, account for 50% overhead
    MEM=$(echo "($MEMAVAIL - 1000) / 1.5" | bc)
else
    MEM=$1
fi
echo "Using up to ${MEM} million items"

threads=$(nproc)
while :; do
    echo "Using ${threads} threads"
    for L in 32 64; do
        depth=${DEPTH[$L]}
        mkeys=${MKEYS[$L]}
        numfilters=$(echo "${MEM} / ${mkeys}" | bc -l | cut -d '.' -f 1)

        for mode in 0 1 2; do
            modearg=${MODEARGS[$mode]}
            echo "  Running with ${numfilters} filters (L=${L}, mode=${mode})"

            # do queries when running with fewer threads?
            LOGFILE=${LOG}_L${L}_m${mode}_t${threads}
            ../parbench -I -t ${threads} -m ${mkeys}000000 -k ${numfilters} -d ${depth} -q ${numq} ${modearg} -L ${L} -s 1337 > ${LOGFILE}
            grep "RESULT type=agg" ${LOGFILE} >> ${AGGLOG}
            grep "RESULT" ${LOGFILE} >> ${ALLLOG}
        done
    done

    if [ $threads -eq 1 ]; then
        break;
    fi
    threads=$(pow2up $((threads/2)))
done
