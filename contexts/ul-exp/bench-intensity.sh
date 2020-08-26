#!/usr/bin/bash


for MODE in pt wa wb wt mfwa mfwb mfwt
do
    echo "Doing mode ${MODE} - "

    for i in {10000..25000..1000}
    do
        echo "  intensity $i ..."

        while true; do
            sleep 5

            timeout 300s ./bench ${MODE} throughput $i > result/result-$i-${MODE}.txt

            if [[ $? -eq 0 ]]; then
                break
            else
                echo "    retrying ..."
            fi
        done
    done
done
