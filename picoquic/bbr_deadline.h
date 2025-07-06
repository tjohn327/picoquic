/*
* BBR Deadline-Aware Extensions
* 
* This file contains the deadline-aware extensions for BBRv3 congestion control.
* It integrates with the existing BBR implementation to provide:
* - Urgency-based pacing adjustments
* - Deadline-aware probing behavior
* - Temporary congestion window boosts for urgent traffic
*/

#ifndef PICOQUIC_BBR_DEADLINE_H
#define PICOQUIC_BBR_DEADLINE_H

#include "picoquic_internal.h"
#include "bbr_common.h"

/* Deadline urgency levels for BBR adjustments */
typedef enum {
    BBR_DEADLINE_URGENCY_NONE = 0,      /* No deadline pressure */
    BBR_DEADLINE_URGENCY_LOW = 1,       /* > 100ms to deadline */
    BBR_DEADLINE_URGENCY_MEDIUM = 2,    /* 50-100ms to deadline */
    BBR_DEADLINE_URGENCY_HIGH = 3,      /* 20-50ms to deadline */
    BBR_DEADLINE_URGENCY_CRITICAL = 4   /* < 20ms to deadline */
} bbr_deadline_urgency_t;

/* BBR deadline state - extends the main BBR state */
typedef struct st_bbr_deadline_state_t {
    /* Deadline tracking */
    bbr_deadline_urgency_t urgency_level;
    uint64_t next_deadline_check;
    uint64_t earliest_deadline;
    int has_deadline_streams;
    
    /* Pacing adjustments */
    double deadline_pacing_gain;        /* Additional gain for deadline streams (1.0 - 2.0) */
    double effective_pacing_gain;       /* Combined gain (bbr_gain * deadline_gain) */
    
    /* Congestion window adjustments */
    uint64_t deadline_cwnd_boost;       /* Temporary boost in bytes */
    uint64_t deadline_boost_end_time;   /* When to remove the boost */
    
    /* Fairness tracking */
    uint64_t deadline_bytes_sent;       /* Bytes sent with deadline boost */
    uint64_t total_bytes_sent;          /* Total bytes sent in window */
    uint64_t fairness_window_start;     /* Start of fairness measurement window */
    
    /* Probe behavior modifications */
    int skip_probe_down;                /* Skip probe_down for urgent traffic */
    int quick_probe_exit;               /* Exit probe states quickly */
    uint64_t last_urgency_update;       /* Last time urgency was recalculated */
} bbr_deadline_state_t;

/* Function declarations */

/* Initialize deadline state for BBR */
void bbr_deadline_init(bbr_deadline_state_t* deadline_state);

/* Update deadline urgency based on current streams */
void bbr_deadline_update_urgency(picoquic_cnx_t* cnx, picoquic_path_t* path_x, 
    bbr_deadline_state_t* deadline_state, uint64_t current_time);

/* Calculate deadline-aware pacing gain */
double bbr_deadline_pacing_gain(bbr_deadline_state_t* deadline_state, 
    double base_pacing_gain, int is_probe_up);

/* Calculate deadline-aware congestion window */
uint64_t bbr_deadline_cwnd_adjustment(bbr_deadline_state_t* deadline_state,
    uint64_t base_cwnd, uint64_t bdp, uint64_t current_time);

/* Check if should skip probe states for deadline */
int bbr_deadline_should_skip_probe(bbr_deadline_state_t* deadline_state,
    int current_state, uint64_t current_time);

/* Update fairness tracking */
void bbr_deadline_update_fairness(bbr_deadline_state_t* deadline_state,
    uint64_t bytes_sent, int is_deadline_boosted, uint64_t current_time);

/* Reset deadline boost (e.g., after deadline miss or completion) */
void bbr_deadline_reset_boost(bbr_deadline_state_t* deadline_state);

#endif /* PICOQUIC_BBR_DEADLINE_H */