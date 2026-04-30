#!/bin/bash

LOGDIR=../logs/epstuning_v5
MAINLOG=${LOGDIR}/result

mkdir -p ${LOGDIR}

declare -a BSIZE=([16]=16 [32]=32 [64]=128 [128]=512)
declare -a DEPTH=([16]=4 [32]=4 [64]=3 [128]=3)
declare -a MODEARGS=([0]='' [1]='-1' [2]='-2')

# disable core dumps (shouldn't happen, though)
ulimit -c 0

for L in 32 64 64s 128; do
    if [[ $L == '64s' ]]; then
        sparse=1
        L=64
    else
        sparse=0
    fi
    for mode in 0 1 2; do
        for bshift in -1 0 1; do
            # compute base bucket size
            B=${BSIZE[$L]}
            depth=${DEPTH[$L]}
            if [[ $bshift == '-1' ]]; then
                B=$((B>>1))
            elif [[ $bshift == '1' ]]; then
                B=$((B<<1))
                # decrease depth to prevent crashes
                depth=$((depth - 1))
            fi
            modearg=${MODEARGS[$mode]}
            # compute epsilon and adjust bucket size for uncompressed thresholds
            if [[ $mode == '0' ]]; then
                baseeps=$(echo "-4.0/${L}" | bc -l);
                B=$((2*$B))
            elif [[ $mode == '1' ]]; then
                baseeps=$(echo "-0.6666666 * ${L} / (4*${B} + ${L})" | bc -l);
            elif [[ $mode == '2' ]]; then
                baseeps=$(echo "-4.0/${L}" | bc -l);
            else
                echo "Unknown mode: ${mode}";
            fi

            echo "Using B=${B} (bshift=${bshift}) for L=${L}, sparse? ${sparse}, mode=${mode}, depth=${depth}, base eps=${baseeps}"
            # Assemble epsilon range
            RANGE=$(seq 0 0.05 2)
            if [[ $bshift == '-1' && $mode != '1' ]]; then
                # try more epsilons further from zero
                RANGE="${RANGE} $(seq 2.1 0.1 3)"
            fi

            # Dense -> interleaved, sparse -> basic
            if [[ $sparse == '0' ]]; then
                storage='-I'
            else
                storage='-S'
            fi

            for epsf in ${RANGE}; do
                echo -n "."
                LOGFILE=${LOGDIR}/log_L${L}_m${mode}_s${sparse}_b${bshift}_${epsf};
                eps=$(echo "${baseeps} * ${epsf}" | bc -l)
                seq 1 16 | parallel ../bench -Q -m 100000000 ${storage} ${modearg} -L ${L} -b ${bshift} -d ${depth} -e ${eps} -s {} -t 1 > ${LOGFILE}
                grep RESULT ${LOGFILE} >> ${MAINLOG}
            done
            echo ""
        done
    done
done
