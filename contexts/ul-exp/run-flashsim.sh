#!/usr/bin/bash

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
    ${FLASHSIM_EXEC} cache-sock cache-ssd.conf
elif [[ $1 == "core" ]]; then
    ${FLASHSIM_EXEC} core-sock core-ssd.conf
else
    echo "Usage: ./run-flashsim.sh cache|core"
    exit 1
fi
