#!/bin/bash
# Client script for cross-machine validation (run on machine 1)
# Usage: ./run_client.sh [mcmini]

MODE=${1:-normal}
COORD_ADDR=10.200.205.81
COORD_PORT=5000

cd /home/aayushi/benchmarks/splash2/codes/apps/water-nsquared

if [ "$MODE" = "mcmini" ]; then
    rm -rf /tmp/mcmini_test2
    mkdir -p /tmp/mcmini_test2
fi

cat input

echo "Starting instance 1 (client) on $(hostname) ..."
if [ "$MODE" = "mcmini" ]; then
    CROSS_VALIDATION_INSTANCE_ID=1 CROSS_VALIDATION_NUM_INSTANCES=2 \
    CROSS_VALIDATION_SERVER_ADDR=$COORD_ADDR CROSS_VALIDATION_SERVER_PORT=$COORD_PORT \
    timeout --signal=SIGINT 60s /home/aayushi/tmp-mcmini/mcmini/dmtcp/build/mcmini \
        --coord-port 7781 \
        --ckptdir /tmp/mcmini_test2 \
        --interval 3 \
        --log-level 1 \
        ./WATER-NSQUARED < input
else
    CROSS_VALIDATION_INSTANCE_ID=1 CROSS_VALIDATION_NUM_INSTANCES=2 \
    CROSS_VALIDATION_SERVER_ADDR=$COORD_ADDR CROSS_VALIDATION_SERVER_PORT=$COORD_PORT \
    timeout --signal=SIGINT 60s ./WATER-NSQUARED < input
fi
