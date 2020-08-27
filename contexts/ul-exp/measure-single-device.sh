#!/bin/bash


INTENSITY=4500


echo "Doing cache device throughput measurement..."


echo "  Read: WA on intensity ${INTENSITY} with 100% read..."

while true; do
    sleep 5

    timeout 300s ./bench wa throughput ${INTENSITY} 100 99 > result/measure-read-int${INTENSITY}.txt

    if [[ $? -eq 0 ]]; then
        break
    else
        echo "    retrying ..."
    fi
done


echo "  Write: WB on intensity ${INTENSITY} with 0% read..."

while true; do
    sleep 5

    timeout 300s ./bench wb throughput ${INTENSITY} 0 99 > result/measure-write-int${INTENSITY}.txt

    if [[ $? -eq 0 ]]; then
        break
    else
        echo "    retrying ..."
    fi
done
