#!/bin/bash
# Simple test script for deadline-aware streams

# Change to script directory
cd "$(dirname "$0")"

echo "=== Deadline-Aware Streams Simple Test ==="
echo "This test demonstrates basic deadline functionality"
echo

# Build the test programs
echo "Building test programs..."
make clean
make
if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

# Start server in background
echo "Starting server on port 4443..."
./deadline_server 4443 &
SERVER_PID=$!
sleep 1

# Check if server started
if ! ps -p $SERVER_PID > /dev/null; then
    echo "Server failed to start!"
    exit 1
fi

echo "Server started with PID $SERVER_PID"
echo

# Run client test
echo "Running client test..."
./deadline_client_fixed localhost 4443

# Give server time to process final messages
sleep 1

# Stop server
echo
echo "Stopping server..."
kill -INT $SERVER_PID
wait $SERVER_PID 2>/dev/null

echo
echo "Test completed!"