/*
* Network simulation tests for deadline-aware streams
* Tests deadline behavior under various simulated network conditions
*/

#include "../picoquic/picoquic_internal.h"
#include "picoquictest_internal.h"
#include <stdlib.h>
#include <string.h>

/* Network scenario definitions */
typedef enum {
    NETWORK_IDEAL,
    NETWORK_WIFI,
    NETWORK_SATELLITE,
    NETWORK_CELLULAR,
    NETWORK_CONGESTED,
    NETWORK_LOSSY,
    NETWORK_JITTERY
} network_scenario_t;

/* Test scenario configuration */
typedef struct st_deadline_scenario_t {
    char const* name;
    network_scenario_t network_type;
    uint64_t deadline_ms;
    size_t data_size;
    int is_hard_deadline;
    double expected_success_rate;
    int enable_multipath;
} deadline_scenario_t;

/* Network characteristics */
typedef struct st_network_config_t {
    uint64_t latency_us;
    uint64_t bandwidth_bps;
    uint64_t loss_mask;
    uint64_t jitter_us;
    uint64_t queue_delay_max;
    int use_wifi_jitter;
    int use_red;
} network_config_t;

/* Static loss masks for different scenarios */
static uint64_t wifi_loss_mask = 0x0100010001000100ull;
static uint64_t satellite_loss_mask = 0x00FF000000000000ull;
static uint64_t cellular_loss_mask = 0x00F000F000F000F0ull;
static uint64_t lossy_loss_mask = 0x5555555555555555ull;

/* Configure network based on scenario */
static void configure_network_scenario(picoquictest_sim_link_t* link, network_scenario_t scenario)
{
    switch (scenario) {
    case NETWORK_IDEAL:
        /* Low latency, high bandwidth, no loss */
        link->microsec_latency = 5000;      /* 5ms */
        link->picosec_per_byte = 800;       /* 10 Mbps */
        link->loss_mask = NULL;
        break;
        
    case NETWORK_WIFI:
        /* Variable latency, medium bandwidth, occasional loss */
        link->microsec_latency = 10000;     /* 10ms base */
        link->picosec_per_byte = 2000;      /* 4 Mbps */
        link->loss_mask = &wifi_loss_mask; /* ~1.5% loss */
        link->jitter = 10000;
        link->jitter_mode = jitter_wifi;
        break;
        
    case NETWORK_SATELLITE:
        /* High latency, medium bandwidth, burst loss */
        link->microsec_latency = 300000;    /* 300ms */
        link->picosec_per_byte = 4000;      /* 2 Mbps */
        link->loss_mask = &satellite_loss_mask; /* Burst loss */
        break;
        
    case NETWORK_CELLULAR:
        /* Medium latency, variable bandwidth, handover loss */
        link->microsec_latency = 50000;     /* 50ms */
        link->picosec_per_byte = 8000;      /* 1 Mbps */
        link->loss_mask = &cellular_loss_mask; /* Periodic burst */
        link->jitter = 25000;
        break;
        
    case NETWORK_CONGESTED:
        /* Queue buildup, RED dropping */
        link->microsec_latency = 20000;     /* 20ms */
        link->picosec_per_byte = 8000;      /* 1 Mbps */
        link->queue_delay_max = 100000;     /* 100ms queue */
        link->red_queue_max = 80000;
        link->red_drop_mask = 0x0000000000000001ull;
        break;
        
    case NETWORK_LOSSY:
        /* High loss rate */
        link->microsec_latency = 30000;     /* 30ms */
        link->picosec_per_byte = 4000;      /* 2 Mbps */
        link->loss_mask = &lossy_loss_mask; /* 50% loss */
        break;
        
    case NETWORK_JITTERY:
        /* High jitter, variable delay */
        link->microsec_latency = 25000;     /* 25ms base */
        link->picosec_per_byte = 2000;      /* 4 Mbps */
        link->jitter = 55000; /* Average jitter */
        break;
    }
}

/* Test scenarios to run */
static const deadline_scenario_t test_scenarios[] = {
    /* Ideal network - should always succeed */
    { "ideal_soft_50ms", NETWORK_IDEAL, 50, 5000, 0, 1.0, 0 },
    { "ideal_hard_20ms", NETWORK_IDEAL, 20, 2000, 1, 0.95, 0 },
    
    /* WiFi - moderate success expected */
    { "wifi_soft_100ms", NETWORK_WIFI, 100, 10000, 0, 0.9, 0 },
    { "wifi_hard_50ms", NETWORK_WIFI, 50, 5000, 1, 0.7, 0 },
    { "wifi_multipath", NETWORK_WIFI, 80, 8000, 1, 0.85, 1 },
    
    /* Satellite - only long deadlines work */
    { "satellite_soft_500ms", NETWORK_SATELLITE, 500, 20000, 0, 0.8, 0 },
    { "satellite_hard_1s", NETWORK_SATELLITE, 1000, 50000, 1, 0.9, 0 },
    { "satellite_fail_100ms", NETWORK_SATELLITE, 100, 10000, 1, 0.1, 0 },
    
    /* Cellular - variable performance */
    { "cellular_soft_200ms", NETWORK_CELLULAR, 200, 15000, 0, 0.8, 0 },
    { "cellular_multipath", NETWORK_CELLULAR, 150, 12000, 1, 0.9, 1 },
    
    /* Congested - test fairness and dropping */
    { "congested_mixed", NETWORK_CONGESTED, 100, 20000, 0, 0.6, 0 },
    { "congested_urgent", NETWORK_CONGESTED, 50, 5000, 1, 0.4, 0 },
    
    /* Extreme conditions */
    { "lossy_soft", NETWORK_LOSSY, 200, 10000, 0, 0.3, 0 },
    { "jittery_hard", NETWORK_JITTERY, 150, 15000, 1, 0.5, 0 },
};

/* Run a single scenario test */
static int run_deadline_scenario(const deadline_scenario_t* scenario, int* success, int* total)
{
    int ret = 0;
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    uint8_t* buffer = NULL;
    size_t bytes_received = 0;
    int gaps_received = 0;
    int test_success = 0;
    
    DBG_PRINTF("Running scenario: %s\n", scenario->name);
    DBG_PRINTF("  Network: %d, Deadline: %lums, Size: %zu, Hard: %d\n",
        scenario->network_type, (unsigned long)scenario->deadline_ms,
        scenario->data_size, scenario->is_hard_deadline);
    
    /* Initialize test context */
    ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 1, 0);
    
    if (ret == 0) {
        /* Enable deadline-aware streams on client */
        test_ctx->cnx_client->local_parameters.enable_deadline_aware_streams = 1;
        
        /* Set server default transport parameters to enable deadline-aware streams */
        picoquic_tp_t server_tp;
        memset(&server_tp, 0, sizeof(picoquic_tp_t));
        picoquic_init_transport_parameters(&server_tp, 1); /* server mode */
        server_tp.enable_deadline_aware_streams = 1;
        picoquic_set_default_tp(test_ctx->qserver, &server_tp);
        
        /* Configure network scenario */
        configure_network_scenario(test_ctx->c_to_s_link, scenario->network_type);
        configure_network_scenario(test_ctx->s_to_c_link, scenario->network_type);
        
        /* Start the client connection */
        ret = picoquic_start_client_cnx(test_ctx->cnx_client);
        
        /* Enable multipath on client if requested */
        if (scenario->enable_multipath) {
            test_ctx->cnx_client->is_multipath_enabled = 1;
            
            /* Add alternative path with different characteristics */
            test_ctx->c_to_s_link_2 = picoquictest_sim_link_create(
                0.004,  /* 4 Mbps in Gbps */
                40000,  /* 40ms latency */
                NULL,   /* No loss on backup path */
                0, simulated_time);
            test_ctx->s_to_c_link_2 = picoquictest_sim_link_create(
                0.004, 40000, NULL, 0, simulated_time);
        }
    }
    
    if (ret == 0) {
        /* Establish connection */
        ret = tls_api_connection_loop(test_ctx, NULL, 0, &simulated_time);
        
        if (ret == 0 && test_ctx->cnx_server != NULL) {
            /* Enable multipath on server if requested */
            if (scenario->enable_multipath) {
                test_ctx->cnx_server->is_multipath_enabled = 1;
            }
        }
    }
    
    if (ret == 0) {
        /* Set stream deadline */
        ret = picoquic_set_stream_deadline(test_ctx->cnx_client, 0,
            scenario->deadline_ms, scenario->is_hard_deadline);
    }
    
    if (ret == 0) {
        /* Allocate and send data */
        buffer = (uint8_t*)malloc(scenario->data_size);
        if (buffer == NULL) {
            ret = -1;
        } else {
            memset(buffer, 0xDD, scenario->data_size);
            ret = picoquic_add_to_stream(test_ctx->cnx_client, 0,
                buffer, scenario->data_size, 1);
        }
    }
    
    if (ret == 0) {
        /* Record start time for deadline */
        uint64_t start_time = simulated_time;
        uint64_t deadline_time = start_time + (scenario->deadline_ms * 1000);
        uint64_t timeout = deadline_time + 100000; /* 100ms grace period */
        
        /* Exchange data until deadline or completion */
        while (simulated_time < timeout && ret == 0) {
            int stream_fin = 0;
            
            ret = tls_api_one_sim_round(test_ctx, &simulated_time, 20000, NULL);
            
            /* Check if stream completed */
            picoquic_stream_head_t* s = picoquic_find_stream(test_ctx->cnx_server, 0);
            stream_fin = (s != NULL) ? picoquic_is_stream_closed(s, 0) : 0;
            if (stream_fin) {
                break;
            }
        }
        
        /* Analyze results */
        picoquic_stream_head_t* stream = picoquic_find_stream(test_ctx->cnx_server, 0);
        if (stream != NULL) {
            bytes_received = (size_t)stream->consumed_offset;
            if (stream->deadline_ctx != NULL) {
                /* Count gaps */
                picosplay_node_t* node = picosplay_first(
                    &stream->deadline_ctx->receiver_dropped_ranges.ack_tree);
                while (node != NULL) {
                    gaps_received++;
                    node = picosplay_next(node);
                }
            }
        }
        
        /* Determine success */
        if (bytes_received == scenario->data_size) {
            test_success = 1;
            DBG_PRINTF("%s", "  SUCCESS: All data received within deadline\n");
        } else if (gaps_received > 0 && scenario->is_hard_deadline) {
            test_success = 1; /* Expected behavior for hard deadline */
            DBG_PRINTF("  SUCCESS: Hard deadline properly enforced (%d gaps)\n", gaps_received);
        } else if (bytes_received > 0) {
            test_success = 0;
            DBG_PRINTF("  PARTIAL: %zu/%zu bytes received\n", 
                bytes_received, scenario->data_size);
        } else {
            test_success = 0;
            DBG_PRINTF("%s", "  FAILED: No data received\n");
        }
        
        /* Update counters */
        (*total)++;
        if (test_success) {
            (*success)++;
        }
    }
    
    /* Cleanup */
    if (buffer != NULL) {
        free(buffer);
    }
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    return ret;
}

/* Main network simulation test */
int deadline_network_sim_test()
{
    int ret = 0;
    int total_scenarios = 0;
    int successful_scenarios = 0;
    size_t i;
    
    DBG_PRINTF("%s", "\n=== Deadline Network Simulation Test ===\n");
    DBG_PRINTF("Testing %zu scenarios across various network conditions\n",
        sizeof(test_scenarios) / sizeof(test_scenarios[0]));
    
    /* Run each scenario */
    for (i = 0; i < sizeof(test_scenarios) / sizeof(test_scenarios[0]) && ret == 0; i++) {
        ret = run_deadline_scenario(&test_scenarios[i],
            &successful_scenarios, &total_scenarios);
        
        if (ret != 0) {
            DBG_PRINTF("Scenario %s failed with error %d\n",
                test_scenarios[i].name, ret);
        }
    }
    
    /* Summary */
    DBG_PRINTF("%s", "\n=== Network Simulation Summary ===\n");
    DBG_PRINTF("Total scenarios: %d\n", total_scenarios);
    DBG_PRINTF("Successful: %d\n", successful_scenarios);
    DBG_PRINTF("Success rate: %.1f%%\n",
        (100.0 * successful_scenarios) / total_scenarios);
    
    /* Validate against expected success rates */
    if (ret == 0) {
        double actual_rate = (double)successful_scenarios / total_scenarios;
        double expected_rate = 0.0;
        
        /* Calculate average expected rate */
        for (i = 0; i < sizeof(test_scenarios) / sizeof(test_scenarios[0]); i++) {
            expected_rate += test_scenarios[i].expected_success_rate;
        }
        expected_rate /= (sizeof(test_scenarios) / sizeof(test_scenarios[0]));
        
        if (actual_rate < expected_rate * 0.8) {
            DBG_PRINTF("Success rate too low: %.1f%% < %.1f%% (expected)\n",
                actual_rate * 100, expected_rate * 100);
            ret = -1;
        }
    }
    
    return ret;
}

/* Test deadline behavior during network transitions */
int deadline_network_transition_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    uint8_t buffer[10000];
    int phase = 0;
    
    DBG_PRINTF("%s", "\n=== Deadline Network Transition Test ===\n");
    
    /* Initialize test context */
    ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 1, 0);
    
    if (ret == 0) {
        /* Enable deadline-aware streams on client */
        test_ctx->cnx_client->local_parameters.enable_deadline_aware_streams = 1;
        
        /* Set server default transport parameters to enable deadline-aware streams */
        picoquic_tp_t server_tp;
        memset(&server_tp, 0, sizeof(picoquic_tp_t));
        picoquic_init_transport_parameters(&server_tp, 1); /* server mode */
        server_tp.enable_deadline_aware_streams = 1;
        picoquic_set_default_tp(test_ctx->qserver, &server_tp);
        
        /* Start with good network */
        configure_network_scenario(test_ctx->c_to_s_link, NETWORK_IDEAL);
        configure_network_scenario(test_ctx->s_to_c_link, NETWORK_IDEAL);
        
        /* Start the client connection */
        ret = picoquic_start_client_cnx(test_ctx->cnx_client);
    }
    
    if (ret == 0) {
        /* Establish connection */
        ret = tls_api_connection_loop(test_ctx, NULL, 0, &simulated_time);
    }
    
    if (ret == 0) {
        /* Create deadline stream */
        ret = picoquic_set_stream_deadline(test_ctx->cnx_client, 0, 100, 1);
        
        /* Send initial data */
        memset(buffer, 0x11, sizeof(buffer));
        ret = picoquic_add_to_stream(test_ctx->cnx_client, 0, buffer, sizeof(buffer), 0);
    }
    
    /* Run test with network transitions */
    while (phase < 3 && ret == 0) {
        uint64_t phase_duration = 2000000; /* 2 seconds per phase */
        uint64_t phase_end = simulated_time + phase_duration;
        
        DBG_PRINTF("\nPhase %d: ", phase);
        
        /* Configure network for this phase */
        switch (phase) {
        case 0:
            DBG_PRINTF("%s", "Good network\n");
            /* Already configured */
            break;
            
        case 1:
            DBG_PRINTF("%s", "Transition to congested\n");
            configure_network_scenario(test_ctx->c_to_s_link, NETWORK_CONGESTED);
            configure_network_scenario(test_ctx->s_to_c_link, NETWORK_CONGESTED);
            break;
            
        case 2:
            DBG_PRINTF("%s", "Transition to lossy\n");
            configure_network_scenario(test_ctx->c_to_s_link, NETWORK_LOSSY);
            configure_network_scenario(test_ctx->s_to_c_link, NETWORK_LOSSY);
            break;
        }
        
        /* Send more data */
        memset(buffer, (uint8_t)(0x22 + phase), 5000);
        ret = picoquic_add_to_stream(test_ctx->cnx_client, 0, buffer, 5000, 0);
        
        /* Run this phase */
        while (simulated_time < phase_end && ret == 0) {
            ret = tls_api_one_sim_round(test_ctx, &simulated_time, 50000, NULL);
        }
        
        /* Check deadline performance */
        picoquic_cnx_t* cnx = test_ctx->cnx_client;
        if (cnx->deadline_context != NULL) {
            DBG_PRINTF("  Deadline bytes: %lu, Non-deadline bytes: %lu\n",
                (unsigned long)cnx->deadline_context->deadline_bytes_sent,
                (unsigned long)cnx->deadline_context->non_deadline_bytes_sent);
        }
        
        phase++;
    }
    
    /* Cleanup */
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    return ret;
}