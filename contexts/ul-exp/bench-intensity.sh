#!/usr/bin/bash


for MODE in pt wa wb wt mfwa mfwb mfwt
do
    echo "Doing mode ${MODE} - "

    for i in {10000..25000..1000}
    do
        echo "  intensity $i ..."
        
        ./bench ${MODE} intensity $i > result/result-$i-${MODE}.txt

        sleep 5
    done
done
