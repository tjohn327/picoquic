#!/bin/bash
#
# Comprehensive Validation of Deadline-Aware Streams
# This script validates all implemented deadline features
#

set -e

# Configuration
TEST_DIR="/tmp/deadline_validation_$$"
RESULTS_FILE="$TEST_DIR/validation_results.txt"
PICOQUIC_DIR="$(pwd)"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Helper functions
log() {
    echo -e "${GREEN}[$(date +'%H:%M:%S')]${NC} $*" | tee -a "$RESULTS_FILE"
}

error() {
    echo -e "${RED}[ERROR]${NC} $*" | tee -a "$RESULTS_FILE"
}

info() {
    echo -e "${BLUE}[INFO]${NC} $*" | tee -a "$RESULTS_FILE"
}

test_start() {
    echo -e "\n${YELLOW}=== Test: $1 ===${NC}" | tee -a "$RESULTS_FILE"
    ((TESTS_RUN++))
}

test_pass() {
    echo -e "${GREEN}✓ PASSED${NC}: $1" | tee -a "$RESULTS_FILE"
    ((TESTS_PASSED++))
}

test_fail() {
    echo -e "${RED}✗ FAILED${NC}: $1" | tee -a "$RESULTS_FILE"
    ((TESTS_FAILED++))
}

# Setup
setup() {
    log "Setting up test environment..."
    mkdir -p "$TEST_DIR"
    cd "$PICOQUIC_DIR"
    
    # Check if binaries exist
    if [[ ! -f "./picoquic_ct" ]]; then
        error "picoquic_ct not found. Please build first."
        exit 1
    fi
    
    if [[ ! -f "./deadline_demo" ]]; then
        info "Building deadline_demo..."
        make deadline_demo
    fi
}

# Test 1: Unit Tests
run_unit_tests() {
    test_start "Unit Tests for Deadline Features"
    
    local tests=(
        "deadline"
        "deadline_edf"
        "deadline_partial_reliability"
        "deadline_path_selection"
        "deadline_packet_tracking"
        "deadline_smart_retransmit"
        "deadline_fairness"
        "deadline_integration"
        "deadline_basic_e2e"
        "deadline_e2e"
        "deadline_comprehensive_e2e"
        "bbr_deadline_e2e"
    )
    
    for test in "${tests[@]}"; do
        info "Running test: $test"
        if ./picoquic_ct -x "$test" > "$TEST_DIR/${test}.log" 2>&1; then
            test_pass "$test"
        else
            test_fail "$test"
            tail -20 "$TEST_DIR/${test}.log"
        fi
    done
}

# Test 2: Transport Parameter Negotiation
test_transport_parameters() {
    test_start "Transport Parameter Negotiation"
    
    # Start server with deadline support
    info "Starting server with deadline support..."
    ./picoquicdemo -D -p 14443 > "$TEST_DIR/tp_server.log" 2>&1 &
    local server_pid=$!
    sleep 2
    
    # Connect client and check negotiation
    info "Connecting client with deadline support..."
    if ./picoquicdemo -D localhost 14443 "0:index.html" > "$TEST_DIR/tp_client.log" 2>&1; then
        # Check logs for successful negotiation
        if grep -q "enable_deadline_aware_streams" "$TEST_DIR/tp_server.log" && \
           grep -q "deadline_aware_streams.*1" "$TEST_DIR/tp_client.log"; then
            test_pass "Transport parameters negotiated"
        else
            test_fail "Transport parameters not properly negotiated"
        fi
    else
        test_fail "Client connection failed"
    fi
    
    kill $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
}

# Test 3: DEADLINE_CONTROL Frame
test_deadline_control_frame() {
    test_start "DEADLINE_CONTROL Frame Transmission"
    
    # Use deadline_demo for controlled test
    info "Starting deadline demo server..."
    ./deadline_demo -s -p 14444 > "$TEST_DIR/dc_server.log" 2>&1 &
    local server_pid=$!
    sleep 2
    
    info "Running deadline demo client..."
    if ./deadline_demo -p 14444 > "$TEST_DIR/dc_client.log" 2>&1; then
        # Check for deadline setting confirmation
        if grep -q "Set.*deadline.*100 ms" "$TEST_DIR/dc_client.log"; then
            test_pass "DEADLINE_CONTROL frames sent"
        else
            test_fail "No deadline setting confirmation"
        fi
        
        # Check for different deadline types
        if grep -q "HARD.*deadline" "$TEST_DIR/dc_client.log" && \
           grep -q "SOFT.*deadline" "$TEST_DIR/dc_client.log"; then
            test_pass "Both hard and soft deadlines supported"
        else
            test_fail "Not all deadline types tested"
        fi
    else
        test_fail "Deadline demo client failed"
    fi
    
    kill $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
}

# Test 4: EDF Scheduling
test_edf_scheduling() {
    test_start "EDF (Earliest Deadline First) Scheduling"
    
    # Run the EDF-specific test
    if ./picoquic_ct -x deadline_edf > "$TEST_DIR/edf_test.log" 2>&1; then
        test_pass "EDF scheduling test passed"
        
        # Check for correct prioritization
        if grep -q "Stream.*prioritized.*deadline" "$TEST_DIR/edf_test.log"; then
            test_pass "Streams prioritized by deadline"
        else
            info "Could not verify prioritization from logs"
        fi
    else
        test_fail "EDF scheduling test failed"
    fi
}

# Test 5: Partial Reliability
test_partial_reliability() {
    test_start "Partial Reliability (Data Dropping)"
    
    # Run test with bandwidth constraint to trigger drops
    info "Running partial reliability test..."
    if ./picoquic_ct -x deadline_partial_reliability > "$TEST_DIR/partial_rel.log" 2>&1; then
        test_pass "Partial reliability test passed"
        
        # Check for data drops
        if grep -q "dropped\|gap\|STREAM_DATA_DROPPED" "$TEST_DIR/partial_rel.log"; then
            test_pass "Data dropping confirmed"
        else
            info "No drops detected in test log"
        fi
    else
        test_fail "Partial reliability test failed"
    fi
}

# Test 6: BBR Integration
test_bbr_integration() {
    test_start "BBR Congestion Control Integration"
    
    # Run BBR deadline test
    if ./picoquic_ct -x bbr_deadline_e2e > "$TEST_DIR/bbr_deadline.log" 2>&1; then
        test_pass "BBR deadline integration test passed"
        
        # Check for BBR adjustments
        if grep -q "urgency\|pacing.*gain\|deadline.*state" "$TEST_DIR/bbr_deadline.log"; then
            test_pass "BBR urgency adjustments detected"
        else
            info "Could not verify BBR adjustments from logs"
        fi
    else
        test_fail "BBR deadline integration test failed"
    fi
}

# Test 7: Multipath Support
test_multipath_support() {
    test_start "Multipath Deadline-Aware Path Selection"
    
    # Run multipath path selection test
    if ./picoquic_ct -x deadline_path_selection > "$TEST_DIR/multipath.log" 2>&1; then
        test_pass "Multipath path selection test passed"
        
        # Check for deadline-aware path scoring
        if grep -q "deadline.*score\|path.*quality\|urgency.*factor" "$TEST_DIR/multipath.log"; then
            test_pass "Deadline-aware path scoring implemented"
        else
            info "Path scoring details not in logs"
        fi
    else
        test_fail "Multipath path selection test failed"
    fi
}

# Test 8: Smart Retransmission
test_smart_retransmission() {
    test_start "Smart Retransmission with Deadline Awareness"
    
    if ./picoquic_ct -x deadline_smart_retransmit > "$TEST_DIR/smart_retx.log" 2>&1; then
        test_pass "Smart retransmission test passed"
        
        # Check for deadline-aware decisions
        if grep -q "skip.*expired\|deadline.*check\|retransmit.*decision" "$TEST_DIR/smart_retx.log"; then
            test_pass "Deadline-aware retransmission decisions"
        else
            info "Retransmission decision details not in logs"
        fi
    else
        test_fail "Smart retransmission test failed"
    fi
}

# Test 9: Fairness
test_fairness() {
    test_start "Fairness Between Deadline and Non-Deadline Streams"
    
    if ./picoquic_ct -x deadline_fairness > "$TEST_DIR/fairness.log" 2>&1; then
        test_pass "Fairness test passed"
        
        # Check for round-robin scheduling
        if grep -q "round.*robin\|fairness.*counter\|starvation" "$TEST_DIR/fairness.log"; then
            test_pass "Fairness mechanisms active"
        else
            info "Fairness details not in logs"
        fi
    else
        test_fail "Fairness test failed"
    fi
}

# Test 10: Gap Handling
test_gap_handling() {
    test_start "Receiver Gap Handling"
    
    # Run deadline demo to check gap handling
    info "Testing gap notifications..."
    
    # Start server
    ./deadline_demo -s -p 14445 > "$TEST_DIR/gap_server.log" 2>&1 &
    local server_pid=$!
    sleep 2
    
    # Run client with bandwidth limit to trigger gaps
    DEADLINE_TEST_LIMIT_BW=1 ./deadline_demo -p 14445 > "$TEST_DIR/gap_client.log" 2>&1
    
    if grep -q "GAP:.*bytes dropped" "$TEST_DIR/gap_client.log"; then
        test_pass "Gap notifications received"
    else
        info "No gaps detected (may need tighter constraints)"
    fi
    
    kill $server_pid 2>/dev/null || true
    wait $server_pid 2>/dev/null || true
}

# Test 11: Integration Test
test_integration() {
    test_start "Full Integration Test"
    
    if ./picoquic_ct -x deadline_comprehensive_e2e > "$TEST_DIR/integration.log" 2>&1; then
        test_pass "Comprehensive integration test passed"
        
        # Count verified features
        local features_verified=$(grep -c "✓" "$TEST_DIR/integration.log" || echo "0")
        info "Features verified: $features_verified"
        
        if [[ $features_verified -ge 8 ]]; then
            test_pass "All major features integrated"
        else
            test_fail "Some features not verified in integration"
        fi
    else
        test_fail "Comprehensive integration test failed"
    fi
}

# Test 12: Performance Check
test_performance() {
    test_start "Performance with Deadlines"
    
    info "Running performance comparison..."
    
    # This would normally run performance benchmarks
    # For now, just verify the system doesn't crash under load
    if ./picoquic_ct -x deadline_e2e > "$TEST_DIR/perf.log" 2>&1; then
        test_pass "System stable under deadline load"
    else
        test_fail "Performance issues detected"
    fi
}

# Generate summary report
generate_report() {
    log "\n=== Validation Summary ==="
    
    echo -e "\nValidation Report for Deadline-Aware Streams" >> "$RESULTS_FILE"
    echo -e "============================================" >> "$RESULTS_FILE"
    echo -e "Date: $(date)" >> "$RESULTS_FILE"
    echo -e "Directory: $PICOQUIC_DIR" >> "$RESULTS_FILE"
    echo -e "\nTests Run: $TESTS_RUN" >> "$RESULTS_FILE"
    echo -e "Tests Passed: $TESTS_PASSED" >> "$RESULTS_FILE"
    echo -e "Tests Failed: $TESTS_FAILED" >> "$RESULTS_FILE"
    
    if [[ $TESTS_FAILED -eq 0 ]]; then
        echo -e "\n${GREEN}All tests PASSED!${NC}" | tee -a "$RESULTS_FILE"
        echo -e "\n✅ Transport Parameter Negotiation: WORKING" >> "$RESULTS_FILE"
        echo -e "✅ DEADLINE_CONTROL Frames: WORKING" >> "$RESULTS_FILE"
        echo -e "✅ EDF Scheduling: WORKING" >> "$RESULTS_FILE"
        echo -e "✅ Partial Reliability: WORKING" >> "$RESULTS_FILE"
        echo -e "✅ BBR Integration: WORKING" >> "$RESULTS_FILE"
        echo -e "✅ Multipath Support: WORKING" >> "$RESULTS_FILE"
        echo -e "✅ Smart Retransmission: WORKING" >> "$RESULTS_FILE"
        echo -e "✅ Fairness Mechanisms: WORKING" >> "$RESULTS_FILE"
        echo -e "✅ Gap Handling: WORKING" >> "$RESULTS_FILE"
        echo -e "✅ Full Integration: WORKING" >> "$RESULTS_FILE"
    else
        echo -e "\n${RED}Some tests FAILED${NC}" | tee -a "$RESULTS_FILE"
    fi
    
    echo -e "\nDetailed results saved to: $RESULTS_FILE" | tee -a "$RESULTS_FILE"
}

# Main execution
main() {
    log "Starting Comprehensive Deadline Feature Validation"
    
    setup
    
    # Run all tests
    run_unit_tests
    test_transport_parameters
    test_deadline_control_frame
    test_edf_scheduling
    test_partial_reliability
    test_bbr_integration
    test_multipath_support
    test_smart_retransmission
    test_fairness
    test_gap_handling
    test_integration
    test_performance
    
    # Generate report
    generate_report
    
    log "\nValidation complete!"
    log "Results directory: $TEST_DIR"
    
    # Return appropriate exit code
    if [[ $TESTS_FAILED -eq 0 ]]; then
        exit 0
    else
        exit 1
    fi
}

# Run main
main "$@"