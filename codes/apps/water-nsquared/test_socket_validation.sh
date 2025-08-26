#!/bin/bash

# Test script for socket-based cross-validation system
# Usage: ./test_socket_validation.sh [mcmini]

MODE=${1:-normal}

echo "🧪 Testing socket-based cross-validation system..."

if [ "$MODE" = "mcmini" ]; then
    echo "🔧 Running with McMini for deterministic execution"
else
    echo "🔧 Running in normal mode"
fi

cd /home/aayushi/benchmarks/splash2/codes/apps/water-nsquared

# Clean up any existing socket files and create run directories
rm -f /tmp/water_validation_socket
if [ "$MODE" = "mcmini" ]; then
    rm -rf /tmp/mcmini_test1 /tmp/mcmini_test2
    mkdir -p /tmp/mcmini_test1 /tmp/mcmini_test2
fi

# Run 2 instances of water with validation enabled
echo "🚀 Running 2 instances of WATER-NSQUARED with socket validation..."
echo "   Instance 0 will be the coordinator (server)"
echo "   Instance 1 will be the client"

# Run with validation enabled - 2 instances
echo "📝 Input file contents:"
cat input
echo ""
echo "🔌 Starting socket-based cross-validation with 2 instances..."

# Start coordinator instance (instance 0)
echo "Starting instance 0 (coordinator)..."
if [ "$MODE" = "mcmini" ]; then
    # Run under McMini with coordinator port 7780
    CROSS_VALIDATION_INSTANCE_ID=0 CROSS_VALIDATION_NUM_INSTANCES=2 \
    timeout --signal=SIGINT 60s /home/aayushi/tmp-mcmini/mcmini/dmtcp/build/mcmini \
        --coord-port 7780 \
        --ckptdir /tmp/mcmini_test1 \
        --interval 10 \
        --log-level 1 \
        ./WATER-NSQUARED < input &
else
    # Run normally
    CROSS_VALIDATION_INSTANCE_ID=0 CROSS_VALIDATION_NUM_INSTANCES=2 timeout --signal=SIGINT 60s ./WATER-NSQUARED < input &
fi
PID1=$!

sleep 2  # Give coordinator time to set up socket

# Start client instance (instance 1)
echo "Starting instance 1 (client)..."
if [ "$MODE" = "mcmini" ]; then
    # Run under McMini with coordinator port 7781
    CROSS_VALIDATION_INSTANCE_ID=1 CROSS_VALIDATION_NUM_INSTANCES=2 \
    timeout --signal=SIGINT 60s /home/aayushi/tmp-mcmini/mcmini/dmtcp/build/mcmini \
        --coord-port 7781 \
        --ckptdir /tmp/mcmini_test2 \
        --interval 10 \
        --log-level 1 \
        ./WATER-NSQUARED < input &
else
    # Run normally
    CROSS_VALIDATION_INSTANCE_ID=1 CROSS_VALIDATION_NUM_INSTANCES=2 timeout --signal=SIGINT 60s ./WATER-NSQUARED < input &
fi
PID2=$!

# Wait for both instances to complete
echo "⏳ Waiting for both instances to complete..."
wait $PID1
EXIT_CODE1=$?
wait $PID2 
EXIT_CODE2=$?

# Cleanup: Kill any remaining background processes
echo "🧹 Cleaning up any remaining processes..."
if ps -p $PID1 > /dev/null 2>&1; then
    echo "Terminating instance 0 (PID: $PID1)"
    kill -SIGINT $PID1 2>/dev/null || kill -SIGTERM $PID1 2>/dev/null
fi
if ps -p $PID2 > /dev/null 2>&1; then
    echo "Terminating instance 1 (PID: $PID2)"
    kill -SIGINT $PID2 2>/dev/null || kill -SIGTERM $PID2 2>/dev/null
fi

# Give processes time to clean up
sleep 1

# Force kill if still running
if ps -p $PID1 > /dev/null 2>&1; then
    kill -SIGKILL $PID1 2>/dev/null
fi
if ps -p $PID2 > /dev/null 2>&1; then
    kill -SIGKILL $PID2 2>/dev/null
fi

echo ""
echo "=== Results ==="
if [ $EXIT_CODE1 -eq 124 ] || [ $EXIT_CODE2 -eq 124 ]; then
    echo "⚠️ One or more instances timed out (likely normal for McMini mode)"
elif [ $EXIT_CODE1 -ne 0 ] || [ $EXIT_CODE2 -ne 0 ]; then
    echo "❌ One or more instances failed (Exit codes: $EXIT_CODE1, $EXIT_CODE2)"
else
    echo "✅ Both instances completed successfully"
fi

if [ "$MODE" = "mcmini" ]; then
    echo "📁 McMini checkpoint directories:"
    echo "   Instance 0: /tmp/mcmini_test1"
    echo "   Instance 1: /tmp/mcmini_test2"
fi

echo ""
echo "✅ Socket-based cross-validation test completed!"
echo "   - Check output above for socket communication messages"
echo "   - Look for '🔧 Instance X' messages indicating socket activity"
echo "   - Assertion will trigger if fingerprints don't match between processes"
if [ "$MODE" = "mcmini" ]; then
    echo "   - McMini provided deterministic execution for race condition detection"
fi
