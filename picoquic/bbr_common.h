/*
* BBR Common Definitions
* 
* This file contains common definitions shared between BBR and BBR deadline extensions.
*/

#ifndef PICOQUIC_BBR_COMMON_H
#define PICOQUIC_BBR_COMMON_H

/* BBR algorithm states */
typedef enum {
    picoquic_bbr_alg_startup = 0,
    picoquic_bbr_alg_drain,
    /* picoquic_bbr_alg_probe_bw, */
    picoquic_bbr_alg_probe_bw_down,
    picoquic_bbr_alg_probe_bw_cruise,
    picoquic_bbr_alg_probe_bw_refill,
    picoquic_bbr_alg_probe_bw_up,
    picoquic_bbr_alg_probe_rtt,
    picoquic_bbr_alg_startup_long_rtt,
    picoquic_bbr_alg_startup_resume
} picoquic_bbr_alg_state_t;

#endif /* PICOQUIC_BBR_COMMON_H */