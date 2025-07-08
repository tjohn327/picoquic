/*
* Comprehensive Multipath Evaluation Test
* 
* Tests multiple stream scenarios under various network conditions
* Based on configurations from NOTES/eval_confgurations.md
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include "picoquic.h"
#include "picoquic_internal.h"
#include "picoquictest_internal.h"
#include "tls_api.h"
#include "picoquic_binlog.h"

/* Network condition structure */
typedef struct st_network_condition_t {
    const char* name;
    uint64_t bandwidth_bps;      /* bits per second */
    double delay_sec;            /* one-way delay in seconds */
    double loss_rate;            /* packet loss rate (0.0 to 1.0) */
    double jitter_sec;           /* jitter in seconds */
} st_network_condition_t;

/* Multipath network configuration */
typedef struct st_multipath_network_config_t {
    const char* name;
    st_network_condition_t path1;
    st_network_condition_t path2;
} st_multipath_network_config_t;

/* Test configuration combining scenario and network */
typedef struct st_eval_config_t {
    const char* name;
    const char* scenario_name;
    const char* network_name;
    int is_multipath;
    int enable_deadline;
    int scenario_id;
} st_eval_config_t;

/* Stream scenarios from eval_configurations.md */

/* Scenario A: Video Conferencing */
static st_test_api_deadline_stream_desc_t scenario_video_conf[] = {
    /* Video: 30fps, ~5KB per frame, 100ms deadline */
    { 2, st_stream_type_deadline, 60000, 5000, 33, 100, 30, 0 },
    /* Audio: 20ms chunks, 160 bytes, 150ms deadline */
    { 6, st_stream_type_deadline, 8000, 160, 20, 150, 50, 0 },
    /* Screen share: larger chunks, 500ms deadline */
    { 10, st_stream_type_deadline, 100000, 5000, 100, 500, 20, 0 },
    /* File transfer: no deadline */
    { 14, st_stream_type_normal, 50000, 0, 0, 0, 0, 0 }
};

/* Scenario B: Live Streaming */
static st_test_api_deadline_stream_desc_t scenario_live_stream[] = {
    /* Live video: 4K 60fps, ~8KB per frame, 200ms deadline */
    { 2, st_stream_type_deadline, 480000, 8192, 16, 200, 60, 0 },
    /* Live audio: 150ms deadline */
    { 6, st_stream_type_deadline, 8000, 160, 20, 150, 50, 0 },
    /* Chat messages: 1000ms deadline */
    { 10, st_stream_type_deadline, 10000, 100, 500, 1000, 20, 0 },
    /* Analytics: no deadline */
    { 14, st_stream_type_normal, 20000, 0, 0, 0, 0, 0 }
};

/* Scenario C: Gaming */
static st_test_api_deadline_stream_desc_t scenario_gaming[] = {
    /* Game state: frequent small updates, 50ms deadline */
    { 2, st_stream_type_deadline, 50000, 500, 10, 50, 100, 0 },
    /* Voice chat: 100ms deadline */
    { 6, st_stream_type_deadline, 8000, 160, 20, 100, 50, 0 },
    /* Asset downloads: no deadline */
    { 10, st_stream_type_normal, 200000, 0, 0, 0, 0, 0 },
    /* Telemetry: 500ms deadline */
    { 14, st_stream_type_deadline, 10000, 200, 100, 500, 10, 0 }
};

/* Scenario D: IoT/Telemetry */
static st_test_api_deadline_stream_desc_t scenario_iot[] = {
    /* Sensor data streams with 200ms deadlines */
    { 2, st_stream_type_deadline, 10000, 100, 50, 200, 20, 0 },
    { 6, st_stream_type_deadline, 10000, 100, 50, 200, 20, 0 },
    { 10, st_stream_type_deadline, 10000, 100, 50, 200, 20, 0 },
    /* Control commands with 100ms deadlines */
    { 14, st_stream_type_deadline, 5000, 50, 20, 100, 50, 0 },
    { 18, st_stream_type_deadline, 5000, 50, 20, 100, 50, 0 },
    /* Bulk logs: no deadline */
    { 22, st_stream_type_normal, 50000, 0, 0, 0, 0, 0 }
};

/* Scenario E: Mixed Media */
static st_test_api_deadline_stream_desc_t scenario_mixed_media[] = {
    /* Real-time video: 150ms deadline */
    { 2, st_stream_type_deadline, 60000, 2048, 33, 150, 30, 0 },
    /* Buffered video: 2000ms deadline */
    { 6, st_stream_type_deadline, 100000, 4096, 100, 2000, 10, 0 },
    /* Interactive data: 100ms deadline */
    { 10, st_stream_type_deadline, 20000, 500, 20, 100, 50, 0 },
    /* Background sync: no deadline */
    { 14, st_stream_type_normal, 50000, 0, 0, 0, 0, 0 },
    /* Metrics: 1000ms deadline */
    { 18, st_stream_type_deadline, 10000, 200, 200, 1000, 5, 0 }
};

/* Multipath network configurations */
static st_multipath_network_config_t multipath_networks[] = {
    /* MP-1: WiFi + Cellular */
    { "WiFi_Cellular",
      { "WiFi", 50000000, 0.015, 0.01, 0.005 },      /* 50Mbps, 30ms RTT, 1% loss */
      { "LTE", 10000000, 0.040, 0.005, 0.010 }       /* 10Mbps, 80ms RTT, 0.5% loss */
    },
    
    /* MP-2: Dual WAN */
    { "Dual_WAN",
      { "Primary_ISP", 100000000, 0.010, 0.001, 0.002 },  /* 100Mbps, 20ms RTT */
      { "Backup_ISP", 50000000, 0.020, 0.002, 0.003 }     /* 50Mbps, 40ms RTT */
    },
    
    /* MP-3: Satellite + Terrestrial */
    { "Sat_Terrestrial",
      { "Satellite", 25000000, 0.300, 0.005, 0.005 },     /* 25Mbps, 600ms RTT */
      { "DSL", 10000000, 0.025, 0.001, 0.003 }            /* 10Mbps, 50ms RTT */
    },
    
    /* MP-4: Asymmetric Paths */
    { "Asymmetric",
      { "HighBW_HighLat", 100000000, 0.100, 0.001, 0.005 }, /* 100Mbps, 200ms RTT */
      { "LowBW_LowLat", 10000000, 0.010, 0.001, 0.002 }     /* 10Mbps, 20ms RTT */
    }
};

/* Test configurations - combinations of scenarios and networks */
static st_eval_config_t eval_configs[] = {
    /* Video Conferencing tests */
    { "VideoConf_WiFiCell_Vanilla", "video_conf", "WiFi_Cellular", 1, 0, 200 },
    { "VideoConf_WiFiCell_Deadline", "video_conf", "WiFi_Cellular", 1, 1, 201 },
    { "VideoConf_DualWAN_Vanilla", "video_conf", "Dual_WAN", 1, 0, 202 },
    { "VideoConf_DualWAN_Deadline", "video_conf", "Dual_WAN", 1, 1, 203 },
    
#if 1 /* Comment out remaining tests for now to test basic functionality */
    { "VideoConf_SatTerr_Vanilla", "video_conf", "Sat_Terrestrial", 1, 0, 204 },
    { "VideoConf_SatTerr_Deadline", "video_conf", "Sat_Terrestrial", 1, 1, 205 },
    { "VideoConf_Asym_Vanilla", "video_conf", "Asymmetric", 1, 0, 206 },
    { "VideoConf_Asym_Deadline", "video_conf", "Asymmetric", 1, 1, 207 },
    
    /* Live Streaming tests */
    { "LiveStream_WiFiCell_Vanilla", "live_stream", "WiFi_Cellular", 1, 0, 210 },
    { "LiveStream_WiFiCell_Deadline", "live_stream", "WiFi_Cellular", 1, 1, 211 },
    { "LiveStream_DualWAN_Vanilla", "live_stream", "Dual_WAN", 1, 0, 212 },
    { "LiveStream_DualWAN_Deadline", "live_stream", "Dual_WAN", 1, 1, 213 },
    { "LiveStream_SatTerr_Vanilla", "live_stream", "Sat_Terrestrial", 1, 0, 214 },
    { "LiveStream_SatTerr_Deadline", "live_stream", "Sat_Terrestrial", 1, 1, 215 },
    { "LiveStream_Asym_Vanilla", "live_stream", "Asymmetric", 1, 0, 216 },
    { "LiveStream_Asym_Deadline", "live_stream", "Asymmetric", 1, 1, 217 },
    
    /* Gaming tests */
    { "Gaming_WiFiCell_Vanilla", "gaming", "WiFi_Cellular", 1, 0, 220 },
    { "Gaming_WiFiCell_Deadline", "gaming", "WiFi_Cellular", 1, 1, 221 },
    { "Gaming_DualWAN_Vanilla", "gaming", "Dual_WAN", 1, 0, 222 },
    { "Gaming_DualWAN_Deadline", "gaming", "Dual_WAN", 1, 1, 223 },
    { "Gaming_SatTerr_Vanilla", "gaming", "Sat_Terrestrial", 1, 0, 224 },
    { "Gaming_SatTerr_Deadline", "gaming", "Sat_Terrestrial", 1, 1, 225 },
    { "Gaming_Asym_Vanilla", "gaming", "Asymmetric", 1, 0, 226 },
    { "Gaming_Asym_Deadline", "gaming", "Asymmetric", 1, 1, 227 },
    
    /* IoT tests */
    { "IoT_WiFiCell_Vanilla", "iot", "WiFi_Cellular", 1, 0, 230 },
    { "IoT_WiFiCell_Deadline", "iot", "WiFi_Cellular", 1, 1, 231 },
    { "IoT_DualWAN_Vanilla", "iot", "Dual_WAN", 1, 0, 232 },
    { "IoT_DualWAN_Deadline", "iot", "Dual_WAN", 1, 1, 233 },
    { "IoT_SatTerr_Vanilla", "iot", "Sat_Terrestrial", 1, 0, 234 },
    { "IoT_SatTerr_Deadline", "iot", "Sat_Terrestrial", 1, 1, 235 },
    { "IoT_Asym_Vanilla", "iot", "Asymmetric", 1, 0, 236 },
    { "IoT_Asym_Deadline", "iot", "Asymmetric", 1, 1, 237 },
    
    /* Mixed Media tests */
    { "Mixed_WiFiCell_Vanilla", "mixed_media", "WiFi_Cellular", 1, 0, 240 },
    { "Mixed_WiFiCell_Deadline", "mixed_media", "WiFi_Cellular", 1, 1, 241 },
    { "Mixed_DualWAN_Vanilla", "mixed_media", "Dual_WAN", 1, 0, 242 },
    { "Mixed_DualWAN_Deadline", "mixed_media", "Dual_WAN", 1, 1, 243 },
    { "Mixed_SatTerr_Vanilla", "mixed_media", "Sat_Terrestrial", 1, 0, 244 },
    { "Mixed_SatTerr_Deadline", "mixed_media", "Sat_Terrestrial", 1, 1, 245 },
    { "Mixed_Asym_Vanilla", "mixed_media", "Asymmetric", 1, 0, 246 },
    { "Mixed_Asym_Deadline", "mixed_media", "Asymmetric", 1, 1, 247 },
#endif
    
    /* Gaming tests on different networks for focused testing */
    { "Gaming_WiFiCell_Vanilla", "gaming", "WiFi_Cellular", 1, 0, 220 },
    { "Gaming_WiFiCell_Deadline", "gaming", "WiFi_Cellular", 1, 1, 221 },
    { "Gaming_Asym_Vanilla", "gaming", "Asymmetric", 1, 0, 226 },
    { "Gaming_Asym_Deadline", "gaming", "Asymmetric", 1, 1, 227 }
};


// ... existing code ...

/*
 * Network pattern types for loss simulation
 */
typedef enum {
    picoquic_loss_pattern_uniform = 0,    /* Uniform random loss */
    picoquic_loss_pattern_wifi,           /* WiFi-like bursty loss */
    picoquic_loss_pattern_lte,            /* LTE-like periodic loss */
    picoquic_loss_pattern_wan,            /* WAN-like correlated loss */
    picoquic_loss_pattern_satellite,      /* Satellite-like long bursts */
    picoquic_loss_pattern_dsl             /* DSL-like intermittent loss */
} picoquic_loss_pattern_t;

/*
 * Convert percentage loss to loss mask with specified network pattern
 * 
 * Parameters:
 * - loss_percent: Loss percentage (0.0 to 100.0)
 * - pattern: Network pattern type
 * - random_seed: Seed for reproducible random generation
 * 
 * Returns:
 * - uint64_t loss mask suitable for picoquictest_sim_link_create
 * 
 * Network Pattern Characteristics:
 * - Uniform: Random loss with no correlation
 * - WiFi: Bursty loss with short bursts (1-3 packets) and longer gaps
 * - LTE: Periodic loss with regular intervals and short bursts
 * - WAN: Correlated loss with medium bursts (2-5 packets) and variable gaps
 * - Satellite: Long bursts (5-15 packets) with very long gaps
 * - DSL: Intermittent loss with irregular patterns and medium bursts
 */
uint64_t picoquic_loss_percent_to_mask(double loss_percent, picoquic_loss_pattern_t pattern, uint64_t random_seed)
{
    uint64_t loss_mask = 0;
    uint64_t random_context = random_seed;
    
    /* Validate input */
    if (loss_percent < 0.0 || loss_percent > 100.0) {
        return 0; /* No loss for invalid percentages */
    }
    
    if (loss_percent == 0.0) {
        return 0; /* No loss */
    }
    
    /* Convert percentage to probability (0.0 to 1.0) */
    double loss_probability = loss_percent / 100.0;
    
    /* Calculate target number of loss bits in 64-bit mask */
    int target_loss_bits = (int)(loss_probability * 64.0 + 0.5);
    
    if (target_loss_bits <= 0) {
        return 0; /* No loss for very small percentages */
    }
    
    if (target_loss_bits >= 64) {
        return UINT64_MAX; /* All packets lost for very high percentages */
    }
    
    switch (pattern) {
        case picoquic_loss_pattern_uniform:
            /* Uniform random loss - distribute loss bits randomly */
            for (int i = 0; i < 64; i++) {
                loss_mask <<= 1;
                if (picoquic_test_uniform_random(&random_context, 1000) < (loss_probability * 1000)) {
                    loss_mask |= 1;
                }
            }
            break;
            
        case picoquic_loss_pattern_wifi:
            /* WiFi-like pattern: short bursts (1-3 packets) with longer gaps */
            {
                int remaining_loss_bits = target_loss_bits;
                int position = 0;
                
                while (remaining_loss_bits > 0 && position < 64) {
                    /* Decide if this is the start of a burst */
                    if (picoquic_test_uniform_random(&random_context, 1000) < (loss_probability * 2000)) {
                        /* Start a burst */
                        int burst_size = 1 + (picoquic_test_uniform_random(&random_context, 3)); /* 1-3 packets */
                        burst_size = (burst_size > remaining_loss_bits) ? remaining_loss_bits : burst_size;
                        
                        /* Place burst bits */
                        for (int j = 0; j < burst_size && (position + j) < 64; j++) {
                            loss_mask |= (1ULL << (63 - position - j));
                            remaining_loss_bits--;
                        }
                        position += burst_size;
                    } else {
                        position++;
                    }
                }
            }
            break;
            
        case picoquic_loss_pattern_lte:
            /* LTE-like pattern: periodic loss with regular intervals */
            {
                int remaining_loss_bits = target_loss_bits;
                int period = 64 / (target_loss_bits + 1); /* Regular spacing */
                if (period < 2) period = 2;
                
                for (int i = 0; i < 64 && remaining_loss_bits > 0; i += period) {
                    if (remaining_loss_bits > 0) {
                        loss_mask |= (1ULL << (63 - i));
                        remaining_loss_bits--;
                    }
                }
                
                /* Add some randomness to the pattern */
                uint64_t random_mask = 0;
                for (int i = 0; i < 64; i++) {
                    random_mask <<= 1;
                    if (picoquic_test_uniform_random(&random_context, 1000) < (loss_probability * 500)) {
                        random_mask |= 1;
                    }
                }
                loss_mask |= random_mask;
            }
            break;
            
        case picoquic_loss_pattern_wan:
            /* WAN-like pattern: medium bursts (2-5 packets) with variable gaps */
            {
                int remaining_loss_bits = target_loss_bits;
                int position = 0;
                
                while (remaining_loss_bits > 0 && position < 64) {
                    /* Decide if this is the start of a burst */
                    if (picoquic_test_uniform_random(&random_context, 1000) < (loss_probability * 1500)) {
                        /* Start a burst */
                        int burst_size = 2 + (picoquic_test_uniform_random(&random_context, 4)); /* 2-5 packets */
                        burst_size = (burst_size > remaining_loss_bits) ? remaining_loss_bits : burst_size;
                        
                        /* Place burst bits */
                        for (int j = 0; j < burst_size && (position + j) < 64; j++) {
                            loss_mask |= (1ULL << (63 - position - j));
                            remaining_loss_bits--;
                        }
                        position += burst_size;
                        
                        /* Add variable gap */
                        int gap = 3 + (picoquic_test_uniform_random(&random_context, 8)); /* 3-10 packets */
                        position += gap;
                    } else {
                        position++;
                    }
                }
            }
            break;
            
        case picoquic_loss_pattern_satellite:
            /* Satellite-like pattern: long bursts (5-15 packets) with very long gaps */
            {
                int remaining_loss_bits = target_loss_bits;
                int position = 0;
                
                while (remaining_loss_bits > 0 && position < 64) {
                    /* Decide if this is the start of a burst */
                    if (picoquic_test_uniform_random(&random_context, 1000) < (loss_probability * 3000)) {
                        /* Start a long burst */
                        int burst_size = 5 + (picoquic_test_uniform_random(&random_context, 11)); /* 5-15 packets */
                        burst_size = (burst_size > remaining_loss_bits) ? remaining_loss_bits : burst_size;
                        
                        /* Place burst bits */
                        for (int j = 0; j < burst_size && (position + j) < 64; j++) {
                            loss_mask |= (1ULL << (63 - position - j));
                            remaining_loss_bits--;
                        }
                        position += burst_size;
                        
                        /* Add very long gap */
                        int gap = 10 + (picoquic_test_uniform_random(&random_context, 20)); /* 10-30 packets */
                        position += gap;
                    } else {
                        position++;
                    }
                }
            }
            break;
            
        case picoquic_loss_pattern_dsl:
            /* DSL-like pattern: intermittent loss with irregular patterns */
            {
                int remaining_loss_bits = target_loss_bits;
                int position = 0;
                
                while (remaining_loss_bits > 0 && position < 64) {
                    /* Decide if this is the start of a burst */
                    if (picoquic_test_uniform_random(&random_context, 1000) < (loss_probability * 1200)) {
                        /* Start a burst */
                        int burst_size = 1 + (picoquic_test_uniform_random(&random_context, 4)); /* 1-4 packets */
                        burst_size = (burst_size > remaining_loss_bits) ? remaining_loss_bits : burst_size;
                        
                        /* Place burst bits */
                        for (int j = 0; j < burst_size && (position + j) < 64; j++) {
                            loss_mask |= (1ULL << (63 - position - j));
                            remaining_loss_bits--;
                        }
                        position += burst_size;
                        
                        /* Add irregular gap */
                        int gap = 1 + (picoquic_test_uniform_random(&random_context, 15)); /* 1-15 packets */
                        position += gap;
                    } else {
                        position++;
                    }
                }
            }
            break;
            
        default:
            /* Fall back to uniform pattern */
            for (int i = 0; i < 64; i++) {
                loss_mask <<= 1;
                if (picoquic_test_uniform_random(&random_context, 1000) < (loss_probability * 1000)) {
                    loss_mask |= 1;
                }
            }
            break;
    }
    
    /* Ensure we don't exceed the target number of loss bits */
    int actual_loss_bits = 0;
    uint64_t temp_mask = loss_mask;
    while (temp_mask != 0) {
        actual_loss_bits += (temp_mask & 1);
        temp_mask >>= 1;
    }
    
    /* Adjust if we have too many or too few loss bits */
    if (actual_loss_bits > target_loss_bits) {
        /* Remove excess loss bits randomly */
        int excess = actual_loss_bits - target_loss_bits;
        for (int i = 0; i < excess; i++) {
            /* Find a random loss bit and clear it */
            int attempts = 0;
            while (attempts < 64) {
                int bit_pos = picoquic_test_uniform_random(&random_context, 64);
                if (loss_mask & (1ULL << bit_pos)) {
                    loss_mask &= ~(1ULL << bit_pos);
                    break;
                }
                attempts++;
            }
        }
    } else if (actual_loss_bits < target_loss_bits) {
        /* Add missing loss bits randomly */
        int missing = target_loss_bits - actual_loss_bits;
        for (int i = 0; i < missing; i++) {
            /* Find a random non-loss bit and set it */
            int attempts = 0;
            while (attempts < 64) {
                int bit_pos = picoquic_test_uniform_random(&random_context, 64);
                if (!(loss_mask & (1ULL << bit_pos))) {
                    loss_mask |= (1ULL << bit_pos);
                    break;
                }
                attempts++;
            }
        }
    }
    
    return loss_mask;
}

/*
 * Helper function to create a loss mask with default random seed
 */
uint64_t picoquic_loss_percent_to_mask_default(double loss_percent, picoquic_loss_pattern_t pattern)
{
    return picoquic_loss_percent_to_mask(loss_percent, pattern, 0xDEADBEEFBABAC001ull);
}


/* External functions */
extern int multipath_deadline_test_one(int scenario, 
                                      st_test_api_deadline_stream_desc_t* test_scenario,
                                      size_t nb_scenario,
                                      uint64_t max_completion_microsec,
                                      int simulate_path_failure);

/* Test metrics structure */
typedef struct st_test_metrics_t {
    int test_completed;
    double duration_sec;
    double throughput_mbps;
    double compliance_pct;
    double avg_latency_ms;
    uint64_t total_bytes;
    uint64_t path0_bytes;
    uint64_t path1_bytes;
    double path0_rtt_ms;
    double path1_rtt_ms;
    int deadline_streams;
    int total_chunks;
    int on_time_chunks;
} st_test_metrics_t;

/* Global test metrics */
static st_test_metrics_t g_test_metrics;

/* Forward declaration */
static int multipath_test_with_network(int scenario,
                                      st_test_api_deadline_stream_desc_t* test_scenario,
                                      size_t nb_scenario,
                                      uint64_t max_completion_microsec,
                                      st_network_condition_t* path1_config,
                                      st_network_condition_t* path2_config);

/* Helper to get scenario by name */
static int get_scenario_data(const char* scenario_name,
                            st_test_api_deadline_stream_desc_t** scenario,
                            size_t* nb_streams)
{
    if (strcmp(scenario_name, "video_conf") == 0) {
        *scenario = scenario_video_conf;
        *nb_streams = sizeof(scenario_video_conf) / sizeof(st_test_api_deadline_stream_desc_t);
    } else if (strcmp(scenario_name, "live_stream") == 0) {
        *scenario = scenario_live_stream;
        *nb_streams = sizeof(scenario_live_stream) / sizeof(st_test_api_deadline_stream_desc_t);
    } else if (strcmp(scenario_name, "gaming") == 0) {
        *scenario = scenario_gaming;
        *nb_streams = sizeof(scenario_gaming) / sizeof(st_test_api_deadline_stream_desc_t);
    } else if (strcmp(scenario_name, "iot") == 0) {
        *scenario = scenario_iot;
        *nb_streams = sizeof(scenario_iot) / sizeof(st_test_api_deadline_stream_desc_t);
    } else if (strcmp(scenario_name, "mixed_media") == 0) {
        *scenario = scenario_mixed_media;
        *nb_streams = sizeof(scenario_mixed_media) / sizeof(st_test_api_deadline_stream_desc_t);
    } else {
        return -1;
    }
    return 0;
}

/* Helper to get network config by name */
static st_multipath_network_config_t* get_network_config(const char* network_name)
{
    size_t num_networks = sizeof(multipath_networks) / sizeof(st_multipath_network_config_t);
    for (size_t i = 0; i < num_networks; i++) {
        if (strcmp(multipath_networks[i].name, network_name) == 0) {
            return &multipath_networks[i];
        }
    }
    return NULL;
}

/* Run one comprehensive evaluation test */
static int comprehensive_eval_test_one(st_eval_config_t* config, FILE* output_file)
{
    st_test_api_deadline_stream_desc_t* scenario_template = NULL;
    st_test_api_deadline_stream_desc_t* test_scenario = NULL;
    size_t nb_streams = 0;
    int ret = 0;
    
    DBG_PRINTF("=== Running %s ===\n", config->name);
    DBG_PRINTF("  Scenario: %s\n", config->scenario_name);
    DBG_PRINTF("  Network: %s\n", config->network_name);
    DBG_PRINTF("  Deadline-aware: %s\n", config->enable_deadline ? "Yes" : "No");
    
    /* Get scenario data */
    ret = get_scenario_data(config->scenario_name, &scenario_template, &nb_streams);
    if (ret != 0) {
        DBG_PRINTF("Unknown scenario: %s\n", config->scenario_name);
        return ret;
    }
    
    /* Get network configuration */
    st_multipath_network_config_t* net_config = get_network_config(config->network_name);
    if (net_config == NULL) {
        DBG_PRINTF("Unknown network: %s\n", config->network_name);
        return -1;
    }
    
    /* Create a copy of the scenario */
    test_scenario = (st_test_api_deadline_stream_desc_t*)malloc(
        nb_streams * sizeof(st_test_api_deadline_stream_desc_t));
    if (test_scenario == NULL) {
        return -1;
    }
    memcpy(test_scenario, scenario_template, 
           nb_streams * sizeof(st_test_api_deadline_stream_desc_t));
    
    /* For vanilla tests, set deadline to 0 for scheduler */
    if (!config->enable_deadline) {
        for (size_t i = 0; i < nb_streams; i++) {
            if (test_scenario[i].stream_type == st_stream_type_deadline) {
                test_scenario[i].deadline_ms = 0;
            }
        }
    }
    
    /* Record start time */
    time_t test_start = time(NULL);
    
    /* Run the test with custom network configuration */
    ret = multipath_test_with_network(config->scenario_id,
                                     test_scenario,
                                     nb_streams,
                                     15000000, /* 15 seconds max */
                                     &net_config->path1,
                                     &net_config->path2);
    
    /* Record results with detailed metrics */
    if (output_file != NULL) {
        /* Extract metrics from deadline context */
        double duration_sec = 0;
        double throughput_mbps = 0;
        double compliance_pct = 0;
        double avg_latency_ms = 0;
        uint64_t total_bytes = 0;
        uint64_t path0_bytes = 0;
        uint64_t path1_bytes = 0;
        double path0_rtt_ms = 0;
        double path1_rtt_ms = 0;
        int deadline_streams = 0;
        int total_chunks = 0;
        int on_time_chunks = 0;
        
        /* Get detailed metrics if test passed */
        if (ret == 0 && g_test_metrics.test_completed) {
            duration_sec = g_test_metrics.duration_sec;
            throughput_mbps = g_test_metrics.throughput_mbps;
            compliance_pct = g_test_metrics.compliance_pct;
            avg_latency_ms = g_test_metrics.avg_latency_ms;
            total_bytes = g_test_metrics.total_bytes;
            path0_bytes = g_test_metrics.path0_bytes;
            path1_bytes = g_test_metrics.path1_bytes;
            path0_rtt_ms = g_test_metrics.path0_rtt_ms;
            path1_rtt_ms = g_test_metrics.path1_rtt_ms;
            deadline_streams = g_test_metrics.deadline_streams;
            total_chunks = g_test_metrics.total_chunks;
            on_time_chunks = g_test_metrics.on_time_chunks;
        }
        
        fprintf(output_file, "%ld,%s,%s,%s,%d,%s,%.2f,%.2f,%.1f,%.1f,%lu,%lu,%lu,%.1f,%.1f,%d,%d,%d\n",
            test_start,
            config->name,
            config->scenario_name,
            config->network_name,
            config->enable_deadline,
            ret == 0 ? "PASSED" : "FAILED",
            duration_sec,
            throughput_mbps,
            compliance_pct,
            avg_latency_ms,
            total_bytes,
            path0_bytes,
            path1_bytes,
            path0_rtt_ms,
            path1_rtt_ms,
            deadline_streams,
            total_chunks,
            on_time_chunks);
        fflush(output_file);  /* Ensure results are written immediately */
    }
    
    if (test_scenario != NULL) {
        free(test_scenario);
    }
    
    return ret;
}

/* Main comprehensive multipath evaluation test */
int multipath_comprehensive_eval_test()
{
    FILE* output_file = NULL;
    int ret = 0;
    size_t num_configs = sizeof(eval_configs) / sizeof(st_eval_config_t);
    
    DBG_PRINTF("%s", "\n========== COMPREHENSIVE MULTIPATH EVALUATION ==========\n");
    DBG_PRINTF("%s", "Testing multiple scenarios under various network conditions\n");
    DBG_PRINTF("%s", "Based on NOTES/eval_confgurations.md\n\n");
    
    /* Open output file (always create new for consistent format) */
    output_file = fopen("comprehensive_multipath_eval_results.csv", "w");
    if (output_file != NULL) {
        fprintf(output_file, "timestamp,test_name,scenario,network,deadline_enabled,result,"
                           "duration_sec,throughput_mbps,compliance_pct,avg_latency_ms,"
                           "total_bytes,path0_bytes,path1_bytes,path0_rtt_ms,path1_rtt_ms,"
                           "deadline_streams,total_chunks,on_time_chunks\n");
    }
    
    /* Run all test configurations */
    int failed_tests = 0;
    int passed_tests = 0;
    
    for (size_t i = 0; i < num_configs; i++) {
        DBG_PRINTF("[%zu/%zu] Running test: %s\n", i+1, num_configs, eval_configs[i].name);
        
        ret = comprehensive_eval_test_one(&eval_configs[i], output_file);
        
        if (ret != 0) {
            DBG_PRINTF("Test %s failed with error %d\n", eval_configs[i].name, ret);
            failed_tests++;
        } else {
            passed_tests++;
        }
        
        /* Always continue with other tests */
        ret = 0;
    }
    
    DBG_PRINTF("%s", "\n========== EVALUATION SUMMARY ==========\n");
    DBG_PRINTF("Total tests: %zu\n", num_configs);
    DBG_PRINTF("Passed: %d\n", passed_tests);
    DBG_PRINTF("Failed: %d\n", failed_tests);
    DBG_PRINTF("Success rate: %.1f%%\n", 
        num_configs > 0 ? (100.0 * passed_tests / num_configs) : 0.0);
    
    /* Return success only if all tests passed */
    ret = (failed_tests > 0) ? -1 : 0;
    
    if (output_file != NULL) {
        fclose(output_file);
        DBG_PRINTF("%s", "\nResults saved to: comprehensive_multipath_eval_results.csv\n");
    }
    
    DBG_PRINTF("\nComprehensive multipath evaluation %s\n", ret == 0 ? "COMPLETED" : "FAILED");
    
    return ret;
}

/* Implementation of multipath test with custom network configuration */
static int multipath_test_with_network(int scenario,
                                      st_test_api_deadline_stream_desc_t* test_scenario,
                                      size_t nb_scenario,
                                      uint64_t max_completion_microsec,
                                      st_network_condition_t* path1_config,
                                      st_network_condition_t* path2_config)
{
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    uint64_t loss_mask = 0;
    deadline_api_test_ctx_t* deadline_ctx = NULL;
    picoquic_connection_id_t initial_cid = { {0x1d, 0xea, 0xd1, 0x1e, 5, 6, 7, 8}, 8 };
    picoquic_tp_t server_parameters;
    int ret = 0;
    
    initial_cid.id[3] = (uint8_t)scenario;
    
    /* Create context */
    ret = tls_api_init_ctx_ex2(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 1, 0, &initial_cid,
        8, 0, 0, 0);
    
    if (ret == 0 && test_ctx == NULL) {
        ret = -1;
    }

    /* Configure primary path with custom network characteristics */
    if (ret == 0 && path1_config != NULL) {
        /* Don't delete default links, just reconfigure them */
        /* The default links were already created by tls_api_init_ctx_ex2 */

        uint64_t loss_mask = picoquic_loss_percent_to_mask_default(path1_config->loss_rate, picoquic_loss_pattern_uniform);

        test_ctx->c_to_s_link = picoquictest_sim_link_create(
            path1_config->bandwidth_bps/1000000000.0,
            (uint64_t)(path1_config->delay_sec * 1000000.0),
            &loss_mask,
            0,
            0);

        test_ctx->s_to_c_link = picoquictest_sim_link_create(
            path1_config->bandwidth_bps/1000000000.0,
            (uint64_t)(path1_config->delay_sec * 1000000.0),
            &loss_mask,
            0,
            0);            
    }
    
    /* Configure transport parameters */
    if (ret == 0) {
        memset(&server_parameters, 0, sizeof(picoquic_tp_t));
        picoquic_init_transport_parameters(&server_parameters, 1);
        server_parameters.is_multipath_enabled = 1;
        server_parameters.initial_max_path_id = 2;
        server_parameters.enable_time_stamp = 3;
        
        /* Check if we're running vanilla mode */
        int is_vanilla = (scenario >= 100 && scenario % 2 == 0);
        server_parameters.enable_deadline_aware_streams = is_vanilla ? 0 : 1;
        
        picoquic_set_default_tp(test_ctx->qserver, &server_parameters);
        
        test_ctx->cnx_client->local_parameters.enable_time_stamp = 3;
        test_ctx->cnx_client->local_parameters.is_multipath_enabled = 1;
        test_ctx->cnx_client->local_parameters.initial_max_path_id = 2;
        test_ctx->cnx_client->local_parameters.enable_deadline_aware_streams = is_vanilla ? 0 : 1;
        
        /* Enable logging if needed */
        /* picoquic_set_binlog(test_ctx->qserver, "."); */
        /* test_ctx->qserver->use_long_log = 1; */
    }


    
    
    /* Initialize deadline context BEFORE establishing connection */
    if (ret == 0) {
        deadline_ctx = (deadline_api_test_ctx_t*)calloc(1, sizeof(deadline_api_test_ctx_t));
        if (deadline_ctx == NULL) {
            ret = -1;
        } else {
            deadline_ctx->start_time = simulated_time;
            deadline_ctx->scenario = test_scenario;
            deadline_ctx->nb_scenario = nb_scenario;
            g_deadline_ctx = deadline_ctx;
            
            deadline_ctx->client_callback.client_mode = 1;
            deadline_ctx->server_callback.client_mode = 0;
            
            picoquic_set_default_callback(test_ctx->qserver, deadline_api_callback, &deadline_ctx->server_callback);
            picoquic_set_callback(test_ctx->cnx_client, deadline_api_callback, &deadline_ctx->client_callback);
        }
    }

    
    /* Establish the connection */
    if (ret == 0) {
        picoquic_start_client_cnx(test_ctx->cnx_client);
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }
    
    /* Verify multipath is negotiated */
    if (ret == 0) {
        if (!test_ctx->cnx_client->is_multipath_enabled || !test_ctx->cnx_server->is_multipath_enabled) {
            DBG_PRINTF("Multipath not negotiated (c=%d, s=%d)\n",
                test_ctx->cnx_client->is_multipath_enabled, test_ctx->cnx_server->is_multipath_enabled);
            ret = -1;
        }
    }
    
    /* Wait until connection is ready */
    if (ret == 0) {
        ret = wait_client_connection_ready(test_ctx, &simulated_time);
    }
    
    /* Add second path with custom network characteristics */
    if (ret == 0 && path2_config != NULL) {
        test_ctx->client_addr_2 = test_ctx->client_addr;
        test_ctx->client_addr_2.sin_port += 17;
        
        DBG_PRINTF("  Path 2 config: %s, BW: %.2f Mbps, RTT: %.0f ms, Loss: %.1f%%\n",
            path2_config->name,
            path2_config->bandwidth_bps / 1000000.0,
            path2_config->delay_sec * 2000.0,  /* Round trip time */
            path2_config->loss_rate * 100.0);
        
        uint64_t loss_mask = picoquic_loss_percent_to_mask_default(path2_config->loss_rate, picoquic_loss_pattern_uniform);

        /* Create path 2 with config characteristics */
        test_ctx->c_to_s_link_2 = picoquictest_sim_link_create(
            path2_config->bandwidth_bps/1000000000.0,
            (uint64_t)(path2_config->delay_sec * 1000000.0),
            &loss_mask,
            0,
            0);
        test_ctx->s_to_c_link_2 = picoquictest_sim_link_create(
            path2_config->bandwidth_bps/1000000000.0,
            (uint64_t)(path2_config->delay_sec * 1000000.0),
            &loss_mask,
            0,
            0);
        
        if (test_ctx->c_to_s_link_2 == NULL || test_ctx->s_to_c_link_2 == NULL) {
            ret = -1;
        }
    }
    
    /* Probe new path */
    if (ret == 0 && path2_config != NULL) {
        ret = picoquic_probe_new_path(test_ctx->cnx_client,
            (struct sockaddr*)&test_ctx->server_addr,
            (struct sockaddr*)&test_ctx->client_addr_2,
            simulated_time);
    }
    
    /* Wait for second path to be ready */
    if (ret == 0 && path2_config != NULL) {
        uint64_t timeout = simulated_time + 4000000;
        int nb_inactive = 0;
        
        while (simulated_time < timeout && ret == 0 && nb_inactive < 64 &&
               (test_ctx->cnx_client->nb_paths != 2 ||
                !test_ctx->cnx_client->path[1]->first_tuple->challenge_verified ||
                test_ctx->cnx_server == NULL ||
                test_ctx->cnx_server->nb_paths != 2 ||
                !test_ctx->cnx_server->path[1]->first_tuple->challenge_verified)) {
            int was_active = 0;
            ret = tls_api_one_sim_round(test_ctx, &simulated_time, timeout, &was_active);
            nb_inactive = was_active ? 0 : nb_inactive + 1;
        }
        
        if (test_ctx->cnx_client->nb_paths != 2) {
            DBG_PRINTF("Failed to establish second path: client has %d paths\n", 
                test_ctx->cnx_client->nb_paths);
            ret = -1;
        } else {
            /* Server might take longer to establish the second path */
            if (test_ctx->cnx_server != NULL && test_ctx->cnx_server->nb_paths < 2) {
                DBG_PRINTF("Warning: Server has only %d paths (may establish later)\n", 
                    test_ctx->cnx_server->nb_paths);
            }
        }
    }
    
    /* Run the deadline stream test */
    if (ret == 0) {
        DBG_PRINTF("Starting data transfer with %zu streams\n", nb_scenario);
        for (int i = 0; i < test_ctx->cnx_client->nb_paths; i++) {
            DBG_PRINTF("Path %d: RTT min=%lu, smoothed=%lu\n", i,
                test_ctx->cnx_client->path[i]->rtt_min,
                test_ctx->cnx_client->path[i]->smoothed_rtt);
        }
        
        ret = deadline_api_data_sending_loop(test_ctx, deadline_ctx,
                                           test_scenario, nb_scenario,
                                           &simulated_time);
    }
    
    /* Calculate and collect detailed metrics */
    if (ret == 0) {
        /* Clear metrics */
        memset(&g_test_metrics, 0, sizeof(g_test_metrics));
        
        /* Calculate test duration */
        g_test_metrics.duration_sec = (simulated_time - deadline_ctx->start_time) / 1000000.0;
        
        /* Check if we're running vanilla mode */
        int is_vanilla = (scenario >= 100 && scenario % 2 == 0);
        
        /* Create a stats scenario that has original deadlines for vanilla mode */
        st_test_api_deadline_stream_desc_t* stats_scenario = test_scenario;
        if (is_vanilla) {
            stats_scenario = (st_test_api_deadline_stream_desc_t*)malloc(
                nb_scenario * sizeof(st_test_api_deadline_stream_desc_t));
            if (stats_scenario != NULL) {
                memcpy(stats_scenario, test_scenario, 
                       nb_scenario * sizeof(st_test_api_deadline_stream_desc_t));
                /* Restore original deadlines for stats calculation */
                for (size_t j = 0; j < nb_scenario; j++) {
                    if (stats_scenario[j].stream_type == st_stream_type_deadline) {
                        /* Restore original deadline based on stream type and scenario */
                        if (stats_scenario[j].stream_id == 2 && nb_scenario == 4) {
                            /* Gaming scenario - game state has 50ms deadline */
                            stats_scenario[j].deadline_ms = 50;
                        } else if (stats_scenario[j].stream_id == 6) {
                            /* Audio/voice streams typically have 100-150ms deadline */
                            stats_scenario[j].deadline_ms = (nb_scenario == 4) ? 100 : 150;
                        } else {
                            /* Default deadline for other streams */
                            stats_scenario[j].deadline_ms = 100;
                        }
                    }
                }
            }
        }
        
        /* Calculate stats for each stream */
        double total_compliance = 0;
        double total_latency = 0;
        int compliance_count = 0;
        
        for (int i = 0; i < deadline_ctx->nb_streams; i++) {
            g_test_metrics.total_bytes += deadline_ctx->stream_state[i].bytes_sent;
            
            int scenario_idx = -1;
            for (size_t j = 0; j < nb_scenario; j++) {
                if (test_scenario[j].stream_id == deadline_ctx->stream_state[i].stream_id) {
                    scenario_idx = j;
                    break;
                }
            }
            
            if (scenario_idx >= 0) {
                if (test_scenario[scenario_idx].stream_type == st_stream_type_normal) {
                    deadline_api_calculate_normal_stats(deadline_ctx, i);
                } else {
                    /* Use stats_scenario for deadline calculation */
                    deadline_api_calculate_deadline_stats(deadline_ctx, i, 
                        stats_scenario ? stats_scenario : test_scenario, nb_scenario);
                    
                    if (deadline_ctx->deadline_stats[i] != NULL) {
                        g_test_metrics.deadline_streams++;
                        total_compliance += deadline_ctx->deadline_stats[i]->deadline_compliance_percent;
                        total_latency += deadline_ctx->deadline_stats[i]->avg_latency_ms;
                        compliance_count++;
                        g_test_metrics.total_chunks += deadline_ctx->deadline_stats[i]->num_chunks;
                        /* Count on-time chunks */
                        for (int c = 0; c < deadline_ctx->deadline_stats[i]->num_chunks; c++) {
                            if (deadline_ctx->deadline_stats[i]->chunk_stats[c].deadline_met) {
                                g_test_metrics.on_time_chunks++;
                            }
                        }
                    }
                }
            }
        }
        
        /* Calculate averages */
        if (compliance_count > 0) {
            g_test_metrics.compliance_pct = total_compliance / compliance_count;
            g_test_metrics.avg_latency_ms = total_latency / compliance_count;
        } else {
            g_test_metrics.compliance_pct = 100.0;  /* No deadline streams */
            g_test_metrics.avg_latency_ms = 0;
        }
        
        /* Calculate throughput */
        if (g_test_metrics.duration_sec > 0) {
            g_test_metrics.throughput_mbps = (g_test_metrics.total_bytes * 8.0) / 
                                           (g_test_metrics.duration_sec * 1000000.0);
        }
        
        /* Get path statistics */
        if (test_ctx->cnx_client != NULL) {
            for (int i = 0; i < test_ctx->cnx_client->nb_paths && i < 2; i++) {
                picoquic_path_t* path = test_ctx->cnx_client->path[i];
                if (i == 0) {
                    g_test_metrics.path0_bytes = path->bytes_sent;
                    g_test_metrics.path0_rtt_ms = path->smoothed_rtt / 1000.0;
                } else {
                    g_test_metrics.path1_bytes = path->bytes_sent;
                    g_test_metrics.path1_rtt_ms = path->smoothed_rtt / 1000.0;
                }
            }
        }
        
        g_test_metrics.test_completed = 1;
        
        /* Clean up stats scenario if allocated */
        if (is_vanilla && stats_scenario != NULL && stats_scenario != test_scenario) {
            free(stats_scenario);
        }
        
        /* Print summary */
        DBG_PRINTF("  Test completed: duration=%.2fs, throughput=%.2f Mbps\n",
            g_test_metrics.duration_sec, g_test_metrics.throughput_mbps);
        if (g_test_metrics.deadline_streams > 0) {
            DBG_PRINTF("  Deadline compliance: %.1f%%, avg latency: %.1f ms\n",
                g_test_metrics.compliance_pct, g_test_metrics.avg_latency_ms);
        }
        DBG_PRINTF("  Path usage: Path0=%lu bytes (%.1f%%), Path1=%lu bytes (%.1f%%)\n",
            g_test_metrics.path0_bytes, 
            g_test_metrics.total_bytes > 0 ? (100.0 * g_test_metrics.path0_bytes / g_test_metrics.total_bytes) : 0,
            g_test_metrics.path1_bytes,
            g_test_metrics.total_bytes > 0 ? (100.0 * g_test_metrics.path1_bytes / g_test_metrics.total_bytes) : 0);
    }
    
    if (test_ctx != NULL) {
        if (test_ctx->cnx_client != NULL) {
            picoquic_set_callback(test_ctx->cnx_client, NULL, NULL);
        }
        if (test_ctx->cnx_server != NULL) {
            picoquic_set_callback(test_ctx->cnx_server, NULL, NULL);
        }
        if (test_ctx->qserver != NULL) {
            picoquic_set_default_callback(test_ctx->qserver, NULL, NULL);
        }
    }
    
    g_deadline_ctx = NULL;
    if (deadline_ctx != NULL) {
        deadline_api_delete_test_ctx(deadline_ctx);
    }
    
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    return ret;
}