#!/bin/bash


for MODE in pt wa wb wt mfwa mfwb mfwt; do
    echo "Doing mode ${MODE}: "

    for INTENSITY in {12000..22000..1000}; do
        echo "  intensity = ${INTENSITY}"

        for READ_PERCENTAGE in 100 95 50 0; do
            echo "    read_percentage = ${READ_PERCENTAGE}"

            for HIT_RATIO in 99 95 80; do
                echo "      hit_ratio = ${HIT_RATIO}"

                while true; do
                    sleep 5
                    
                    timeout 90s ./bench ${MODE} throughput ${INTENSITY} ${READ_PERCENTAGE} ${HIT_RATIO} \
                        > result/result-int${INTENSITY}-read${READ_PERCENTAGE}-hit${HIT_RATIO}-${MODE}.txt

                    if [[ $? -eq 0 ]]; then
                        break
                    else
                        echo "        retrying ..."
                    fi
                done
            done
        done
    done
done
