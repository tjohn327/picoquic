#!/bin/bash
#
# Comprehensive Real-World Test for Deadline-Aware Streams
# 
# This script sets up virtual network environments using veth pairs
# and network namespaces to test deadline-aware streams in realistic
# network conditions.
#
# Requirements:
# - Linux with network namespace support
# - tc (traffic control) for network shaping
# - ip command
# - Root/sudo privileges

set -e

# Configuration
TEST_DIR="/tmp/deadline_test_$$"
RESULTS_DIR="$TEST_DIR/results"
LOGS_DIR="$TEST_DIR/logs"
QLOG_DIR="$TEST_DIR/qlogs"

# Network configuration
CLIENT_NS="deadline_client"
SERVER_NS="deadline_server"
VETH_CLIENT="veth_client"
VETH_SERVER="veth_server"
CLIENT_IP="10.0.0.2"
SERVER_IP="10.0.0.1"
SUBNET="10.0.0.0/24"

# Multipath configuration
CLIENT_NS2="deadline_client2"
VETH_CLIENT2="veth_client2"
VETH_SERVER2="veth_server2"
CLIENT_IP2="10.0.1.2"
SERVER_IP2="10.0.1.1"
SUBNET2="10.0.1.0/24"

# Test files
SMALL_FILE="test_1KB.bin"
MEDIUM_FILE="test_100KB.bin"
LARGE_FILE="test_10MB.bin"
URGENT_FILE="urgent_50KB.bin"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Helper functions
log() {
    echo -e "${GREEN}[$(date +'%H:%M:%S')]${NC} $*"
}

error() {
    echo -e "${RED}[ERROR]${NC} $*" >&2
}

warning() {
    echo -e "${YELLOW}[WARNING]${NC} $*"
}

# Check if running as root
check_root() {
    if [[ $EUID -ne 0 ]]; then
        error "This script must be run as root or with sudo"
        exit 1
    fi
}

# Create test directories
setup_directories() {
    log "Creating test directories..."
    mkdir -p "$RESULTS_DIR"
    mkdir -p "$LOGS_DIR"
    mkdir -p "$QLOG_DIR"
    mkdir -p "$TEST_DIR/server_files"
    mkdir -p "$TEST_DIR/client_files"
}

# Generate test files
generate_test_files() {
    log "Generating test files..."
    
    # Small file (1KB)
    dd if=/dev/urandom of="$TEST_DIR/server_files/$SMALL_FILE" bs=1024 count=1 2>/dev/null
    
    # Medium file (100KB)
    dd if=/dev/urandom of="$TEST_DIR/server_files/$MEDIUM_FILE" bs=1024 count=100 2>/dev/null
    
    # Large file (10MB)
    dd if=/dev/urandom of="$TEST_DIR/server_files/$LARGE_FILE" bs=1024 count=10240 2>/dev/null
    
    # Urgent file (50KB)
    dd if=/dev/urandom of="$TEST_DIR/server_files/$URGENT_FILE" bs=1024 count=50 2>/dev/null
    
    log "Test files created"
}

# Setup network namespaces and veth pairs
setup_network() {
    log "Setting up network namespaces and interfaces..."
    
    # Clean up any existing setup
    cleanup_network 2>/dev/null || true
    
    # Create namespaces
    ip netns add $CLIENT_NS
    ip netns add $SERVER_NS
    
    # Create veth pair
    ip link add $VETH_CLIENT type veth peer name $VETH_SERVER
    
    # Move interfaces to namespaces
    ip link set $VETH_CLIENT netns $CLIENT_NS
    ip link set $VETH_SERVER netns $SERVER_NS
    
    # Configure interfaces
    ip netns exec $CLIENT_NS ip addr add $CLIENT_IP/24 dev $VETH_CLIENT
    ip netns exec $CLIENT_NS ip link set $VETH_CLIENT up
    ip netns exec $CLIENT_NS ip link set lo up
    
    ip netns exec $SERVER_NS ip addr add $SERVER_IP/24 dev $VETH_SERVER
    ip netns exec $SERVER_NS ip link set $VETH_SERVER up
    ip netns exec $SERVER_NS ip link set lo up
    
    # Add routes
    ip netns exec $CLIENT_NS ip route add default via $SERVER_IP
    ip netns exec $SERVER_NS ip route add $SUBNET dev $VETH_SERVER
    
    log "Basic network setup complete"
}

# Setup multipath network
setup_multipath_network() {
    log "Setting up multipath network..."
    
    # Create second client namespace
    ip netns add $CLIENT_NS2 2>/dev/null || true
    
    # Create second veth pair
    ip link add $VETH_CLIENT2 type veth peer name $VETH_SERVER2
    
    # Move interfaces
    ip link set $VETH_CLIENT2 netns $CLIENT_NS2
    ip link set $VETH_SERVER2 netns $SERVER_NS
    
    # Configure second path
    ip netns exec $CLIENT_NS2 ip addr add $CLIENT_IP2/24 dev $VETH_CLIENT2
    ip netns exec $CLIENT_NS2 ip link set $VETH_CLIENT2 up
    ip netns exec $CLIENT_NS2 ip link set lo up
    
    ip netns exec $SERVER_NS ip addr add $SERVER_IP2/24 dev $VETH_SERVER2
    ip netns exec $SERVER_NS ip link set $VETH_SERVER2 up
    
    # Add routes for second path
    ip netns exec $CLIENT_NS2 ip route add default via $SERVER_IP2
    ip netns exec $SERVER_NS ip route add $SUBNET2 dev $VETH_SERVER2
    
    # Connect the two client namespaces
    ip link add veth_bridge type veth peer name veth_bridge2
    ip link set veth_bridge netns $CLIENT_NS
    ip link set veth_bridge2 netns $CLIENT_NS2
    
    ip netns exec $CLIENT_NS ip link set veth_bridge up
    ip netns exec $CLIENT_NS2 ip link set veth_bridge2 up
    
    log "Multipath network setup complete"
}

# Apply network conditions using tc
apply_network_conditions() {
    local interface=$1
    local delay=$2
    local bandwidth=$3
    local loss=$4
    local namespace=$5
    
    log "Applying network conditions to $interface: delay=${delay}ms, bandwidth=${bandwidth}mbit, loss=${loss}%"
    
    # Clear existing qdisc
    ip netns exec $namespace tc qdisc del dev $interface root 2>/dev/null || true
    
    # Apply new conditions
    ip netns exec $namespace tc qdisc add dev $interface root handle 1: htb default 1
    ip netns exec $namespace tc class add dev $interface parent 1: classid 1:1 htb rate ${bandwidth}mbit
    ip netns exec $namespace tc qdisc add dev $interface parent 1:1 handle 10: netem delay ${delay}ms loss ${loss}%
}

# Start picoquic server
start_server() {
    local test_name=$1
    local extra_args=$2
    
    log "Starting picoquic server for test: $test_name"
    
    cd "$TEST_DIR/server_files"
    
    ip netns exec $SERVER_NS ../../../picoquicdemo \
        -p 4443 \
        -c ../../../certs/cert.pem \
        -k ../../../certs/key.pem \
        -q "$QLOG_DIR/server_$test_name" \
        -l "$LOGS_DIR/server_$test_name.log" \
        -G bbr \
        $extra_args \
        > "$LOGS_DIR/server_$test_name.out" 2>&1 &
    
    local server_pid=$!
    sleep 2  # Give server time to start
    
    # Check if server started successfully
    if ! kill -0 $server_pid 2>/dev/null; then
        error "Server failed to start"
        return 1
    fi
    
    echo $server_pid
}

# Run client test
run_client_test() {
    local test_name=$1
    local scenario=$2
    local deadline_config=$3
    local extra_args=$4
    local namespace=${5:-$CLIENT_NS}
    local server_ip=${6:-$SERVER_IP}
    
    log "Running client test: $test_name"
    log "  Scenario: $scenario"
    log "  Deadlines: $deadline_config"
    
    cd "$TEST_DIR/client_files"
    
    local cmd="ip netns exec $namespace ../../../picoquicdemo"
    
    # Add deadline configuration if provided
    if [[ -n "$deadline_config" ]]; then
        cmd="$cmd -D -d \"$deadline_config\""
    fi
    
    cmd="$cmd -q \"$QLOG_DIR/client_$test_name\""
    cmd="$cmd -l \"$LOGS_DIR/client_$test_name.log\""
    cmd="$cmd -G bbr"
    cmd="$cmd $extra_args"
    cmd="$cmd $server_ip 4443 \"$scenario\""
    
    # Run the client
    eval $cmd > "$LOGS_DIR/client_$test_name.out" 2>&1
    local ret=$?
    
    if [[ $ret -eq 0 ]]; then
        log "Client test completed successfully"
    else
        error "Client test failed with code $ret"
    fi
    
    return $ret
}

# Test 1: Basic deadline functionality
test_basic_deadline() {
    log "\n=== Test 1: Basic Deadline Functionality ==="
    
    # Good network conditions
    apply_network_conditions $VETH_CLIENT 5 100 0 $CLIENT_NS
    apply_network_conditions $VETH_SERVER 5 100 0 $SERVER_NS
    
    # Start server
    local server_pid=$(start_server "basic_deadline" "-D")
    
    # Test 1a: Single stream with soft deadline
    run_client_test "basic_soft" "0:$MEDIUM_FILE" "0:200:0"
    
    # Test 1b: Single stream with hard deadline
    run_client_test "basic_hard" "0:$MEDIUM_FILE" "0:100:1"
    
    # Test 1c: Multiple streams with different deadlines
    run_client_test "basic_multi" "0:$SMALL_FILE;4:$MEDIUM_FILE;8:$LARGE_FILE" "0:50:1,4:200:0,8:0:0"
    
    # Stop server
    kill $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
}

# Test 2: Deadline under constrained bandwidth
test_bandwidth_constraint() {
    log "\n=== Test 2: Deadline Under Bandwidth Constraint ==="
    
    # Limited bandwidth (1 Mbit/s)
    apply_network_conditions $VETH_CLIENT 10 1 0 $CLIENT_NS
    apply_network_conditions $VETH_SERVER 10 1 0 $SERVER_NS
    
    # Start server
    local server_pid=$(start_server "bandwidth_constraint" "-D")
    
    # Test with tight hard deadline - should trigger drops
    run_client_test "bandwidth_hard" "0:$LARGE_FILE" "0:100:1"
    
    # Test with soft deadline - should complete but slowly
    run_client_test "bandwidth_soft" "0:$LARGE_FILE" "0:100:0"
    
    # Stop server
    kill $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
}

# Test 3: Deadline with packet loss
test_packet_loss() {
    log "\n=== Test 3: Deadline With Packet Loss ==="
    
    # Network with 2% loss
    apply_network_conditions $VETH_CLIENT 20 10 2 $CLIENT_NS
    apply_network_conditions $VETH_SERVER 20 10 2 $SERVER_NS
    
    # Start server
    local server_pid=$(start_server "packet_loss" "-D")
    
    # Test deadline behavior under loss
    run_client_test "loss_deadline" "0:$MEDIUM_FILE;4:$MEDIUM_FILE" "0:300:1,4:500:0"
    
    # Stop server
    kill $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
}

# Test 4: Multipath with deadlines
test_multipath_deadline() {
    log "\n=== Test 4: Multipath With Deadlines ==="
    
    # Setup multipath network
    setup_multipath_network
    
    # Different conditions on each path
    # Path 1: Low latency, moderate bandwidth
    apply_network_conditions $VETH_CLIENT 5 10 0 $CLIENT_NS
    apply_network_conditions $VETH_SERVER 5 10 0 $SERVER_NS
    
    # Path 2: High latency, high bandwidth
    apply_network_conditions $VETH_CLIENT2 50 100 0 $CLIENT_NS2
    apply_network_conditions $VETH_SERVER2 50 100 0 $SERVER_NS
    
    # Start server with multipath
    local server_pid=$(start_server "multipath" "-D -A 0.0.0.0/0")
    
    # Run multipath client
    # Note: This requires proper multipath configuration in picoquic
    run_client_test "multipath_deadline" \
        "0:$URGENT_FILE;4:$LARGE_FILE" \
        "0:100:1,4:1000:0" \
        "-A \"$CLIENT_IP2/2\""
    
    # Stop server
    kill $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
}

# Test 5: Mixed traffic (deadline and non-deadline)
test_mixed_traffic() {
    log "\n=== Test 5: Mixed Deadline and Non-Deadline Traffic ==="
    
    # Moderate network conditions
    apply_network_conditions $VETH_CLIENT 15 20 0.5 $CLIENT_NS
    apply_network_conditions $VETH_SERVER 15 20 0.5 $SERVER_NS
    
    # Start server
    local server_pid=$(start_server "mixed_traffic" "-D")
    
    # Mix of deadline and non-deadline streams
    run_client_test "mixed" \
        "0:$URGENT_FILE;4:$LARGE_FILE;8:$MEDIUM_FILE;12:$SMALL_FILE" \
        "0:100:1,8:300:0"  # Only streams 0 and 8 have deadlines
    
    # Stop server
    kill $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
}

# Analyze results
analyze_results() {
    log "\n=== Analyzing Test Results ==="
    
    # Check for deadline drops in logs
    log "Checking for deadline-triggered drops..."
    grep -h "Dropped.*due to deadline" $LOGS_DIR/client_*.out 2>/dev/null || echo "No drops detected"
    
    # Check for completed transfers
    log "\nChecking completed transfers..."
    grep -h "Stream.*completed" $LOGS_DIR/client_*.out 2>/dev/null | wc -l | xargs echo "Completed streams:"
    
    # Generate summary report
    cat > "$RESULTS_DIR/summary.txt" <<EOF
Deadline-Aware Streams Real-World Test Summary
==============================================
Test Date: $(date)

Test Environment:
- Client Namespace: $CLIENT_NS
- Server Namespace: $SERVER_NS
- Network: $SUBNET

Tests Executed:
1. Basic Deadline Functionality
2. Deadline Under Bandwidth Constraint
3. Deadline With Packet Loss
4. Multipath With Deadlines
5. Mixed Traffic

Results Location: $RESULTS_DIR
Logs Location: $LOGS_DIR
QLOG Location: $QLOG_DIR

Key Findings:
EOF
    
    # Add key findings from logs
    if grep -q "Dropped.*due to deadline" $LOGS_DIR/client_*.out 2>/dev/null; then
        echo "- Hard deadlines triggered data drops as expected" >> "$RESULTS_DIR/summary.txt"
    fi
    
    if grep -q "deadline_aware_streams.*1" $LOGS_DIR/server_*.out 2>/dev/null; then
        echo "- Transport parameter negotiation successful" >> "$RESULTS_DIR/summary.txt"
    fi
    
    log "Summary report generated: $RESULTS_DIR/summary.txt"
}

# Cleanup network setup
cleanup_network() {
    log "Cleaning up network configuration..."
    
    # Kill any running processes
    pkill -f picoquicdemo 2>/dev/null || true
    
    # Remove namespaces (this also removes veth pairs)
    ip netns del $CLIENT_NS 2>/dev/null || true
    ip netns del $CLIENT_NS2 2>/dev/null || true
    ip netns del $SERVER_NS 2>/dev/null || true
    
    # Clean up any remaining interfaces
    ip link del veth_bridge 2>/dev/null || true
}

# Main test execution
main() {
    log "Starting Deadline-Aware Streams Real-World Tests"
    
    # Check prerequisites
    check_root
    
    # Setup
    setup_directories
    generate_test_files
    setup_network
    
    # Run tests
    test_basic_deadline
    test_bandwidth_constraint
    test_packet_loss
    test_multipath_deadline
    test_mixed_traffic
    
    # Analyze results
    analyze_results
    
    # Cleanup
    cleanup_network
    
    log "\nAll tests completed!"
    log "Results available in: $TEST_DIR"
}

# Handle interrupts
trap cleanup_network EXIT INT TERM

# Run main
main "$@"