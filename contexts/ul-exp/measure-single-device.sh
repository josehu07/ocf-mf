#!/bin/bash


READ_INTENSITY=30000
WRITE_INTENSITY=8000


echo "Doing cache device throughput measurement..."


echo "  Read: WA on intensity ${READ_INTENSITY} with 100% read..."

while true; do
    sleep 5

    timeout 300s ./bench wa throughput ${READ_INTENSITY} 100 99 > result/measure-read-int${READ_INTENSITY}.txt

    if [[ $? -eq 0 ]]; then
        break
    else
        echo "    retrying ..."
    fi
done


echo "  Write: WB on intensity ${WRITE_INTENSITY} with 0% read..."

while true; do
    sleep 5

    timeout 300s ./bench wb throughput ${WRITE_INTENSITY} 0 99 > result/measure-write-int${WRITE_INTENSITY}.txt

    if [[ $? -eq 0 ]]; then
        break
    else
        echo "    retrying ..."
    fi
done
