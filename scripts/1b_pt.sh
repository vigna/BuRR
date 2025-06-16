#!/bin/bash

LOGDIR=../logs/1b_pt_v4
MAINLOG=${LOGDIR}/result

mkdir -p ${LOGDIR}

declare -a BSIZE=([16]=16 [32]=32 [64]=128 [128]=512)
declare -a DEPTH=([16]=3 [32]=3 [64]=2 [128]=1)

# too small for L=64 or L=128
for L in 32 64; do
    B=${BSIZE[$L]}
    baseeps=$(echo "-1.0 * ${L} / (4*${B} + ${L})" | bc -l)
    depth=${DEPTH[$L]}

    echo "Using B=${B} for L=${L}, base eps=${baseeps}"
    for fct in $(seq 0.1 0.1 1.0) $(seq 1.2 0.2 2.0); do
        echo -n "L=${L} trying fct=${fct}"
            for epsf in $(seq 0.5 0.025 0.75); do
            echo -n "."
            LOGFILE=${LOGDIR}/log_${L}_${fct}_${epsf};
            eps=$(echo "${baseeps} * ${epsf}" | bc -l)
            seq 1 32 | parallel ../bench -m 1500000 -1 -L ${L} -d ${depth} -f ${fct} -e ${eps} -s {} -t 1 > ${LOGFILE}
            grep RESULT ${LOGFILE} >> ${MAINLOG}
        done
        echo ""
    done
done
