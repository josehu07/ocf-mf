#!/usr/bin/bash

#
# Run a FlashSim SSD device instance.
#
# Usage: ./run-flashsim.sh SOCK_NAME CONF_PATH
#


if [[ $# -ne 2 ]]; then
    echo "Usage: ./run-flashsim.sh SOCK_NAME CONF_PATH"
    exit 1
fi


FLASHSIM_EXEC=../../flashsim/flashsim


if [[ ! -f ${FLASHSIM_EXEC} ]]; then
    (cd $(dirname ${FLASHSIM_EXEC}) && make)
fi


${FLASHSIM_EXEC} $1 $2
