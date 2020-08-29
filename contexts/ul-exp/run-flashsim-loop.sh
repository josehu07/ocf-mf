#!/bin/bash

#
# Run a FlashSim SSD device instance.
#
# Usage: ./run-flashsim.sh SOCK_NAME CONF_PATH
#


if [[ $# -ne 1 ]]; then
    echo "Usage: ./run-flashsim.sh cache|core"
    exit 1
fi


FLASHSIM_EXEC=../../flashsim/flashsim


if [[ ! -f ${FLASHSIM_EXEC} ]]; then
    (cd $(dirname ${FLASHSIM_EXEC}) && make)
fi


if [[ $1 == "cache" ]]; then
    
    while true; do
        ${FLASHSIM_EXEC} cache-sock cache-ssd.conf

        if [[ $? -eq 0 ]] || [[ $? -eq 130 ]]; then
            break
        else
            echo "  Re-launching..."
            sleep 5
        fi
    done

elif [[ $1 == "core" ]]; then
    
    while true; do
        ${FLASHSIM_EXEC} core-sock core-ssd.conf

        if [[ $? -eq 0 ]] || [[ $? -eq 130 ]]; then
            break
        else
            echo "  Re-launching..."
            sleep 5
        fi
    done

else
    echo "Usage: ./run-flashsim.sh cache|core"
    exit 1
fi
