#!/bin/bash


DEV_CONFIG=cache50-core10
# DEV_CONFIG=cache50-core25
# DEV_CONFIG=optane-flash
# DEV_CONFIG=nvdimm-optane


# for MODE in pt wa wb wt mfwa mfwb mfwt; do
for MODE in wa mfwa; do
    echo "Doing mode ${MODE}: "

    # for INTENSITY in {12000..22000..1000}; do
    for INTENSITY in 6400 12800 19200 25600; do
        echo "  intensity = ${INTENSITY}"

        # for READ_PERCENTAGE in 100 95 50 0; do
        for READ_PERCENTAGE in 100; do
            echo "    read_percentage = ${READ_PERCENTAGE}"

            # for HIT_RATIO in 99 95 80; do
            for HIT_RATIO in 99 80; do
                echo "      hit_ratio = ${HIT_RATIO}"
                
		for TRY_NO in {1..5}; do
                    echo "        try #${TRY_NO}"

		    while true; do
                        sleep 5
                        
                        timeout 90s ./bench ${MODE} throughput ${INTENSITY} ${READ_PERCENTAGE} ${HIT_RATIO} \
                            > result/result-int${INTENSITY}-read${READ_PERCENTAGE}-hit${HIT_RATIO}-${MODE}.try${TRY_NO}.txt

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
done
