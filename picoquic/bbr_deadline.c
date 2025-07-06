/*
* BBR Deadline-Aware Extensions Implementation
*/

#include "bbr_deadline.h"
#include "picoquic_internal.h"
#include "picoquic_utils.h"
#include <math.h>
#include <string.h>

/* Constants for deadline-aware BBR */
#define BBR_DEADLINE_PACING_GAIN_MAX 2.0      /* Maximum pacing boost for deadlines */
#define BBR_DEADLINE_PACING_GAIN_MIN 1.0      /* No boost */
#define BBR_DEADLINE_CWND_BOOST_MAX 1.5       /* Maximum 50% cwnd boost */
#define BBR_DEADLINE_FAIRNESS_WINDOW 100000   /* 100ms fairness window */
#define BBR_DEADLINE_MAX_SHARE 0.6            /* Maximum 60% for deadline traffic */
#define BBR_DEADLINE_CHECK_INTERVAL 10000     /* Check deadlines every 10ms */

/* Initialize deadline state for BBR */
void bbr_deadline_init(bbr_deadline_state_t* deadline_state)
{
    if (deadline_state != NULL) {
        memset(deadline_state, 0, sizeof(bbr_deadline_state_t));
        deadline_state->urgency_level = BBR_DEADLINE_URGENCY_NONE;
        deadline_state->deadline_pacing_gain = BBR_DEADLINE_PACING_GAIN_MIN;
        deadline_state->effective_pacing_gain = BBR_DEADLINE_PACING_GAIN_MIN;
    }
}

/* Calculate urgency level based on time to deadline */
static bbr_deadline_urgency_t calculate_urgency_level(uint64_t time_to_deadline_us)
{
    if (time_to_deadline_us > 100000) {  /* > 100ms */
        return BBR_DEADLINE_URGENCY_LOW;
    } else if (time_to_deadline_us > 50000) {  /* 50-100ms */
        return BBR_DEADLINE_URGENCY_MEDIUM;
    } else if (time_to_deadline_us > 20000) {  /* 20-50ms */
        return BBR_DEADLINE_URGENCY_HIGH;
    } else {  /* < 20ms */
        return BBR_DEADLINE_URGENCY_CRITICAL;
    }
}

/* Update deadline urgency based on current streams */
void bbr_deadline_update_urgency(picoquic_cnx_t* cnx, picoquic_path_t* path_x, 
    bbr_deadline_state_t* deadline_state, uint64_t current_time)
{
    uint64_t earliest_deadline = UINT64_MAX;
    int has_deadline = 0;
    bbr_deadline_urgency_t max_urgency = BBR_DEADLINE_URGENCY_NONE;
    
    /* Only update if enough time has passed */
    if (current_time < deadline_state->next_deadline_check) {
        return;
    }
    deadline_state->next_deadline_check = current_time + BBR_DEADLINE_CHECK_INTERVAL;
    
    /* Check if deadline awareness is enabled */
    if (cnx->deadline_context == NULL || !cnx->deadline_context->deadline_aware_enabled) {
        deadline_state->urgency_level = BBR_DEADLINE_URGENCY_NONE;
        deadline_state->has_deadline_streams = 0;
        return;
    }
    
    /* Scan all streams for earliest deadline */
    picoquic_stream_head_t* stream = picoquic_first_stream(cnx);
    while (stream != NULL) {
        if (stream->deadline_ctx != NULL && 
            stream->deadline_ctx->deadline_enabled &&
            stream->send_queue != NULL) {  /* Has data to send */
            
            has_deadline = 1;
            if (stream->deadline_ctx->absolute_deadline < earliest_deadline) {
                earliest_deadline = stream->deadline_ctx->absolute_deadline;
            }
            
            /* Calculate urgency for this stream */
            if (stream->deadline_ctx->absolute_deadline > current_time) {
                uint64_t time_to_deadline = stream->deadline_ctx->absolute_deadline - current_time;
                bbr_deadline_urgency_t urgency = calculate_urgency_level(time_to_deadline);
                if (urgency > max_urgency) {
                    max_urgency = urgency;
                }
            }
        }
        stream = picoquic_next_stream(stream);
    }
    
    /* Update state */
    deadline_state->has_deadline_streams = has_deadline;
    deadline_state->earliest_deadline = earliest_deadline;
    deadline_state->urgency_level = max_urgency;
    deadline_state->last_urgency_update = current_time;
}

/* Calculate deadline-aware pacing gain */
double bbr_deadline_pacing_gain(bbr_deadline_state_t* deadline_state, 
    double base_pacing_gain, int is_probe_up)
{
    double deadline_gain = BBR_DEADLINE_PACING_GAIN_MIN;
    
    /* No boost if no deadline streams or already in probe_up */
    if (!deadline_state->has_deadline_streams || is_probe_up) {
        deadline_state->deadline_pacing_gain = BBR_DEADLINE_PACING_GAIN_MIN;
        deadline_state->effective_pacing_gain = base_pacing_gain;
        return base_pacing_gain;
    }
    
    /* Calculate gain based on urgency */
    switch (deadline_state->urgency_level) {
        case BBR_DEADLINE_URGENCY_CRITICAL:
            deadline_gain = 2.0;  /* 100% boost */
            break;
        case BBR_DEADLINE_URGENCY_HIGH:
            deadline_gain = 1.5;  /* 50% boost */
            break;
        case BBR_DEADLINE_URGENCY_MEDIUM:
            deadline_gain = 1.25; /* 25% boost */
            break;
        case BBR_DEADLINE_URGENCY_LOW:
            deadline_gain = 1.1;  /* 10% boost */
            break;
        default:
            deadline_gain = 1.0;  /* No boost */
            break;
    }
    
    /* Apply fairness constraint */
    if (deadline_state->total_bytes_sent > 0) {
        double deadline_share = (double)deadline_state->deadline_bytes_sent / 
                               (double)deadline_state->total_bytes_sent;
        if (deadline_share > BBR_DEADLINE_MAX_SHARE) {
            /* Reduce gain to maintain fairness */
            deadline_gain = 1.0 + (deadline_gain - 1.0) * 
                           (BBR_DEADLINE_MAX_SHARE / deadline_share);
        }
    }
    
    /* Store and apply the gain */
    deadline_state->deadline_pacing_gain = deadline_gain;
    deadline_state->effective_pacing_gain = base_pacing_gain * deadline_gain;
    
    /* Cap the total gain */
    if (deadline_state->effective_pacing_gain > BBR_DEADLINE_PACING_GAIN_MAX) {
        deadline_state->effective_pacing_gain = BBR_DEADLINE_PACING_GAIN_MAX;
    }
    
    return deadline_state->effective_pacing_gain;
}

/* Calculate deadline-aware congestion window */
uint64_t bbr_deadline_cwnd_adjustment(bbr_deadline_state_t* deadline_state,
    uint64_t base_cwnd, uint64_t bdp, uint64_t current_time)
{
    uint64_t adjusted_cwnd = base_cwnd;
    
    /* Remove expired boost */
    if (deadline_state->deadline_boost_end_time > 0 &&
        current_time >= deadline_state->deadline_boost_end_time) {
        deadline_state->deadline_cwnd_boost = 0;
        deadline_state->deadline_boost_end_time = 0;
    }
    
    /* Apply boost for urgent deadlines */
    if (deadline_state->has_deadline_streams && 
        deadline_state->urgency_level >= BBR_DEADLINE_URGENCY_HIGH) {
        
        /* Calculate boost based on urgency */
        double boost_factor = 1.0;
        switch (deadline_state->urgency_level) {
            case BBR_DEADLINE_URGENCY_CRITICAL:
                boost_factor = 1.5;  /* 50% boost */
                break;
            case BBR_DEADLINE_URGENCY_HIGH:
                boost_factor = 1.25; /* 25% boost */
                break;
            default:
                boost_factor = 1.0;
                break;
        }
        
        /* Calculate boosted cwnd, but cap at 1.5x BDP */
        uint64_t target_cwnd = (uint64_t)(base_cwnd * boost_factor);
        uint64_t max_cwnd = bdp + (bdp >> 1);  /* 1.5x BDP */
        if (target_cwnd > max_cwnd) {
            target_cwnd = max_cwnd;
        }
        
        /* Apply boost */
        if (target_cwnd > base_cwnd) {
            deadline_state->deadline_cwnd_boost = target_cwnd - base_cwnd;
            deadline_state->deadline_boost_end_time = current_time + 50000; /* 50ms boost */
            adjusted_cwnd = target_cwnd;
        }
    }
    
    return adjusted_cwnd;
}

/* Check if should skip probe states for deadline */
int bbr_deadline_should_skip_probe(bbr_deadline_state_t* deadline_state,
    int current_state, uint64_t current_time)
{
    /* Skip probe_down for high urgency */
    if (deadline_state->urgency_level >= BBR_DEADLINE_URGENCY_HIGH) {
        if (current_state == picoquic_bbr_alg_probe_bw_down) {
            deadline_state->skip_probe_down = 1;
            return 1;
        }
    }
    
    /* Quick exit from probe states for critical urgency */
    if (deadline_state->urgency_level == BBR_DEADLINE_URGENCY_CRITICAL) {
        deadline_state->quick_probe_exit = 1;
        return 1;
    }
    
    return 0;
}

/* Update fairness tracking */
void bbr_deadline_update_fairness(bbr_deadline_state_t* deadline_state,
    uint64_t bytes_sent, int is_deadline_boosted, uint64_t current_time)
{
    /* Reset window if needed */
    if (current_time >= deadline_state->fairness_window_start + BBR_DEADLINE_FAIRNESS_WINDOW) {
        deadline_state->deadline_bytes_sent = 0;
        deadline_state->total_bytes_sent = 0;
        deadline_state->fairness_window_start = current_time;
    }
    
    /* Update counters */
    deadline_state->total_bytes_sent += bytes_sent;
    if (is_deadline_boosted) {
        deadline_state->deadline_bytes_sent += bytes_sent;
    }
}

/* Reset deadline boost */
void bbr_deadline_reset_boost(bbr_deadline_state_t* deadline_state)
{
    deadline_state->deadline_cwnd_boost = 0;
    deadline_state->deadline_boost_end_time = 0;
    deadline_state->deadline_pacing_gain = BBR_DEADLINE_PACING_GAIN_MIN;
    deadline_state->effective_pacing_gain = BBR_DEADLINE_PACING_GAIN_MIN;
    deadline_state->skip_probe_down = 0;
    deadline_state->quick_probe_exit = 0;
}