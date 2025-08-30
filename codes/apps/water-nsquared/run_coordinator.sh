#!/bin/bash
# Coordinator script for cross-machine validation (run on machine 0)
# Usage: ./run_coordinator.sh [mcmini]

MODE=${1:-normal}
COORD_PORT=5000

cd /home/aayushi/benchmarks/splash2/codes/apps/water-nsquared

if [ "$MODE" = "mcmini" ]; then
    rm -rf /tmp/mcmini_test1
    mkdir -p /tmp/mcmini_test1
fi

cat input

echo "Starting instance 0 (coordinator) on $(hostname) ..."
if [ "$MODE" = "mcmini" ]; then
    CROSS_VALIDATION_INSTANCE_ID=0 CROSS_VALIDATION_NUM_INSTANCES=2 \
    CROSS_VALIDATION_SERVER_ADDR=0.0.0.0 CROSS_VALIDATION_SERVER_PORT=$COORD_PORT \
    timeout --signal=SIGINT 60s /home/aayushi/tmp-mcmini/mcmini/dmtcp/build/mcmini \
        --coord-port 7780 \
        --ckptdir /tmp/mcmini_test1 \
        --interval 3 \
        --log-level 1 \
        ./WATER-NSQUARED < input
else
    CROSS_VALIDATION_INSTANCE_ID=0 CROSS_VALIDATION_NUM_INSTANCES=2 \
    CROSS_VALIDATION_SERVER_ADDR=0.0.0.0 CROSS_VALIDATION_SERVER_PORT=$COORD_PORT \
    timeout --signal=SIGINT 60s ./WATER-NSQUARED < input
fi
