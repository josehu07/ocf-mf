#!/usr/bin/bash


for MODE in pt wa mf
do
    echo "Doing mode ${MODE} - "

    for i in {10000..24000..1000}
    do
        echo "  Intensity $i ..."
        
        ./bench ${MODE} $i > results/result-$i-${MODE}.txt
    done
done
