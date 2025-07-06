/*
* Test BBR deadline-aware extensions
*/

#include <stdlib.h>
#include <string.h>
#include "picoquic_internal.h"
#include "picoquic_utils.h"
#include "picoquictest_internal.h"
#include "../picoquic/bbr.c"

/* Test 1: Verify BBR deadline state initialization */
int bbr_deadline_init_test()
{
    picoquic_bbr_state_t bbr_state;
    
    /* Initialize BBR state */
    memset(&bbr_state, 0, sizeof(bbr_state));
    bbr_deadline_init(&bbr_state.deadline_state);
    
    /* Verify initialization */
    if (bbr_state.deadline_state.urgency_level != BBR_DEADLINE_URGENCY_NONE) {
        DBG_PRINTF("Expected urgency level NONE, got %d\n", bbr_state.deadline_state.urgency_level);
        return -1;
    }
    
    if (bbr_state.deadline_state.deadline_pacing_gain != 1.0) {
        DBG_PRINTF("Expected pacing gain 1.0, got %f\n", bbr_state.deadline_state.deadline_pacing_gain);
        return -1;
    }
    
    if (bbr_state.deadline_state.has_deadline_streams != 0) {
        DBG_PRINTF("%s", "Expected no deadline streams initially\n");
        return -1;
    }
    
    return 0;
}

/* Test 2: Verify deadline-aware pacing gain calculations */
int bbr_deadline_pacing_gain_test()
{
    bbr_deadline_state_t deadline_state;
    double base_gain = 1.25;
    double effective_gain;
    
    /* Initialize */
    bbr_deadline_init(&deadline_state);
    
    /* Test 1: No deadline streams - should return base gain */
    deadline_state.has_deadline_streams = 0;
    effective_gain = bbr_deadline_pacing_gain(&deadline_state, base_gain, 0);
    if (effective_gain != base_gain) {
        DBG_PRINTF("Expected base gain %f, got %f\n", base_gain, effective_gain);
        return -1;
    }
    
    /* Test 2: Critical urgency - should return boosted gain */
    deadline_state.has_deadline_streams = 1;
    deadline_state.urgency_level = BBR_DEADLINE_URGENCY_CRITICAL;
    effective_gain = bbr_deadline_pacing_gain(&deadline_state, base_gain, 0);
    if (effective_gain <= base_gain) {
        DBG_PRINTF("Expected boosted gain > %f, got %f\n", base_gain, effective_gain);
        return -1;
    }
    
    /* Test 3: Already in probe_up - should not boost */
    effective_gain = bbr_deadline_pacing_gain(&deadline_state, base_gain, 1);
    if (effective_gain != base_gain) {
        DBG_PRINTF("Expected no boost during probe_up, got %f\n", effective_gain);
        return -1;
    }
    
    return 0;
}

/* Test 3: Verify congestion window adjustments */
int bbr_deadline_cwnd_test()
{
    bbr_deadline_state_t deadline_state;
    uint64_t base_cwnd = 10000;
    uint64_t bdp = 8000;
    uint64_t current_time = 1000000;
    uint64_t adjusted_cwnd;
    
    /* Initialize */
    bbr_deadline_init(&deadline_state);
    
    /* Test 1: No deadline pressure - should return base cwnd */
    deadline_state.has_deadline_streams = 0;
    adjusted_cwnd = bbr_deadline_cwnd_adjustment(&deadline_state, base_cwnd, bdp, current_time);
    if (adjusted_cwnd != base_cwnd) {
        DBG_PRINTF("Expected base cwnd %lu, got %lu\n", 
            (unsigned long)base_cwnd, (unsigned long)adjusted_cwnd);
        return -1;
    }
    
    /* Test 2: High urgency - should boost cwnd */
    deadline_state.has_deadline_streams = 1;
    deadline_state.urgency_level = BBR_DEADLINE_URGENCY_HIGH;
    adjusted_cwnd = bbr_deadline_cwnd_adjustment(&deadline_state, base_cwnd, bdp, current_time);
    if (adjusted_cwnd <= base_cwnd) {
        DBG_PRINTF("Expected boosted cwnd > %lu, got %lu\n", 
            (unsigned long)base_cwnd, (unsigned long)adjusted_cwnd);
        return -1;
    }
    
    /* Test 3: Verify boost is capped at 1.5x BDP */
    uint64_t max_allowed = bdp + (bdp >> 1);
    if (adjusted_cwnd > max_allowed) {
        DBG_PRINTF("Cwnd boost exceeded cap: %lu > %lu\n", 
            (unsigned long)adjusted_cwnd, (unsigned long)max_allowed);
        return -1;
    }
    
    return 0;
}

/* Test 4: Verify probe state skipping */
int bbr_deadline_probe_skip_test()
{
    bbr_deadline_state_t deadline_state;
    uint64_t current_time = 1000000;
    int should_skip;
    
    /* Initialize */
    bbr_deadline_init(&deadline_state);
    
    /* Test 1: Low urgency - should not skip probe_down */
    deadline_state.urgency_level = BBR_DEADLINE_URGENCY_LOW;
    should_skip = bbr_deadline_should_skip_probe(&deadline_state, 
        picoquic_bbr_alg_probe_bw_down, current_time);
    if (should_skip) {
        DBG_PRINTF("%s", "Should not skip probe_down for low urgency\n");
        return -1;
    }
    
    /* Test 2: High urgency - should skip probe_down */
    deadline_state.urgency_level = BBR_DEADLINE_URGENCY_HIGH;
    should_skip = bbr_deadline_should_skip_probe(&deadline_state, 
        picoquic_bbr_alg_probe_bw_down, current_time);
    if (!should_skip) {
        DBG_PRINTF("%s", "Should skip probe_down for high urgency\n");
        return -1;
    }
    
    /* Test 3: Critical urgency - should set quick probe exit */
    deadline_state.urgency_level = BBR_DEADLINE_URGENCY_CRITICAL;
    should_skip = bbr_deadline_should_skip_probe(&deadline_state, 
        picoquic_bbr_alg_probe_bw_cruise, current_time);
    if (!should_skip || !deadline_state.quick_probe_exit) {
        DBG_PRINTF("%s", "Should set quick probe exit for critical urgency\n");
        return -1;
    }
    
    return 0;
}

/* Test 5: Verify fairness tracking */
int bbr_deadline_fairness_test()
{
    bbr_deadline_state_t deadline_state;
    uint64_t current_time = 1000000;
    
    /* Initialize */
    bbr_deadline_init(&deadline_state);
    deadline_state.fairness_window_start = current_time;
    
    /* Send some deadline-boosted bytes */
    bbr_deadline_update_fairness(&deadline_state, 1000, 1, current_time);
    if (deadline_state.deadline_bytes_sent != 1000 || 
        deadline_state.total_bytes_sent != 1000) {
        DBG_PRINTF("Fairness tracking error: deadline=%lu, total=%lu\n",
            (unsigned long)deadline_state.deadline_bytes_sent,
            (unsigned long)deadline_state.total_bytes_sent);
        return -1;
    }
    
    /* Send some normal bytes */
    bbr_deadline_update_fairness(&deadline_state, 500, 0, current_time);
    if (deadline_state.deadline_bytes_sent != 1000 || 
        deadline_state.total_bytes_sent != 1500) {
        DBG_PRINTF("%s", "Fairness tracking error after normal bytes\n");
        return -1;
    }
    
    /* Test window reset */
    current_time += 150000; /* Move past fairness window */
    bbr_deadline_update_fairness(&deadline_state, 100, 1, current_time);
    if (deadline_state.deadline_bytes_sent != 100 || 
        deadline_state.total_bytes_sent != 100) {
        DBG_PRINTF("%s", "Fairness window not reset properly\n");
        return -1;
    }
    
    return 0;
}