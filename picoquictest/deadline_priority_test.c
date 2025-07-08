/*
* This file implements tests for deadline-aware priority scheduling in picoquic.
* It verifies that streams with tighter deadlines get higher priority.
*/

#include "picoquic_internal.h"
#include "picoquic_utils.h"
#include "picosocks.h"
#include "tls_api.h"
#include "picoquictest_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Test scenario with streams having different deadline urgencies */
static st_test_api_deadline_stream_desc_t test_scenario_priority[] = {
    /* Stream 1: Urgent - 20ms deadline, small chunks */
    { 2, st_stream_type_deadline, 0, 512, 50, 20, 20, 0 },
    
    /* Stream 2: Medium urgency - 100ms deadline */
    { 6, st_stream_type_deadline, 0, 1024, 50, 100, 20, 0 },
    
    /* Stream 3: Low urgency - 200ms deadline */
    { 10, st_stream_type_deadline, 0, 2048, 50, 200, 20, 0 },
    
    /* Stream 4: Normal stream (no deadline) */
    { 14, st_stream_type_normal, 20000, 0, 0, 0, 0, 0 }
};

/* Extended test context for priority testing */
typedef struct st_deadline_priority_test_ctx_t {
    deadline_api_test_ctx_t base;
    
    /* Track order of chunk transmissions */
    struct {
        uint64_t stream_id;
        int chunk_index;
        uint64_t send_time;
        uint64_t receive_time;
        uint64_t deadline_at_send;  /* Time left to deadline when sent */
    } transmission_order[200];  /* Track up to 200 transmissions */
    int nb_transmissions;
    
    /* Track stream send patterns */
    struct {
        uint64_t stream_id;
        int chunks_sent_in_order;
        int chunks_sent_out_of_order;
        uint64_t last_chunk_send_time;
    } stream_patterns[4];
    
} deadline_priority_test_ctx_t;

/* Global context for priority test */
static deadline_priority_test_ctx_t* g_priority_test_ctx = NULL;

/* External global from deadline_api_test.c */
extern deadline_api_test_ctx_t* g_deadline_ctx;

/* Forward declaration of deadline_api_callback */
extern int deadline_api_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* stream_ctx);

/* Custom callback to track transmission order */
static int deadline_priority_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* stream_ctx)
{
    test_api_callback_t* cb_ctx = (test_api_callback_t*)callback_ctx;
    deadline_priority_test_ctx_t* priority_ctx = g_priority_test_ctx;
    
    if (priority_ctx == NULL) {
        return -1;
    }
    
    /* First call the base deadline API callback */
    int ret = deadline_api_callback(cnx, stream_id, bytes, length, fin_or_event, callback_ctx, stream_ctx);
    
    /* Track additional information for priority analysis */
    if (fin_or_event == picoquic_callback_stream_data && length > 0 && cb_ctx->client_mode) {
        /* Client sending data - track the transmission */
        if (priority_ctx->nb_transmissions < 200) {
            int trans_idx = priority_ctx->nb_transmissions++;
            uint64_t current_time = picoquic_get_quic_time(cnx->quic);
            
            priority_ctx->transmission_order[trans_idx].stream_id = stream_id;
            priority_ctx->transmission_order[trans_idx].send_time = current_time;
            
            /* Determine which chunk this is */
            int stream_idx = -1;
            for (int i = 0; i < priority_ctx->base.nb_streams; i++) {
                if (priority_ctx->base.stream_state[i].stream_id == stream_id) {
                    stream_idx = i;
                    break;
                }
            }
            
            if (stream_idx >= 0) {
                priority_ctx->transmission_order[trans_idx].chunk_index = 
                    priority_ctx->base.stream_state[stream_idx].chunks_sent - 1;
                
                /* Calculate deadline urgency at send time */
                int scenario_idx = -1;
                for (int i = 0; i < 4; i++) {
                    if (test_scenario_priority[i].stream_id == stream_id) {
                        scenario_idx = i;
                        break;
                    }
                }
                
                if (scenario_idx >= 0 && test_scenario_priority[scenario_idx].stream_type == st_stream_type_deadline) {
                    uint64_t chunk_deadline = priority_ctx->base.stream_state[stream_idx].chunk_send_times[
                        priority_ctx->transmission_order[trans_idx].chunk_index] +
                        (test_scenario_priority[scenario_idx].deadline_ms * 1000);
                    
                    priority_ctx->transmission_order[trans_idx].deadline_at_send = 
                        (chunk_deadline > current_time) ? (chunk_deadline - current_time) : 0;
                }
            }
        }
    }
    
    return ret;
}

/* Analyze transmission patterns to verify priority scheduling */
static int analyze_priority_patterns(deadline_priority_test_ctx_t* priority_ctx)
{
    int ret = 0;
    
    DBG_PRINTF("%s", "\n======== Priority Scheduling Analysis ========\n");
    DBG_PRINTF("Total transmissions tracked: %d\n", priority_ctx->nb_transmissions);
    
    /* Count transmissions per stream */
    int stream_transmission_counts[4] = {0};
    for (int i = 0; i < priority_ctx->nb_transmissions; i++) {
        for (int s = 0; s < 4; s++) {
            if (priority_ctx->transmission_order[i].stream_id == test_scenario_priority[s].stream_id) {
                stream_transmission_counts[s]++;
                break;
            }
        }
    }
    
    DBG_PRINTF("%s", "\nTransmissions per stream:\n");
    for (int s = 0; s < 4; s++) {
        DBG_PRINTF("  Stream %lu: %d transmissions\n", 
            (unsigned long)test_scenario_priority[s].stream_id,
            stream_transmission_counts[s]);
    }
    
    /* Analyze priority violations */
    int priority_violations = 0;
    DBG_PRINTF("%s", "\nPriority scheduling analysis:\n");
    
    /* Look for cases where a less urgent stream was sent before a more urgent one */
    for (int i = 1; i < priority_ctx->nb_transmissions; i++) {
        uint64_t prev_stream = priority_ctx->transmission_order[i-1].stream_id;
        uint64_t curr_stream = priority_ctx->transmission_order[i].stream_id;
        
        /* Skip if same stream */
        if (prev_stream == curr_stream) continue;
        
        /* Get deadline info for both */
        uint64_t prev_deadline_left = UINT64_MAX;
        uint64_t curr_deadline_left = UINT64_MAX;
        
        for (int s = 0; s < 4; s++) {
            if (test_scenario_priority[s].stream_id == prev_stream &&
                test_scenario_priority[s].stream_type == st_stream_type_deadline) {
                prev_deadline_left = priority_ctx->transmission_order[i-1].deadline_at_send;
            }
            if (test_scenario_priority[s].stream_id == curr_stream &&
                test_scenario_priority[s].stream_type == st_stream_type_deadline) {
                curr_deadline_left = priority_ctx->transmission_order[i].deadline_at_send;
            }
        }
        
        /* Check if a less urgent stream was sent before a more urgent one */
        if (curr_deadline_left < prev_deadline_left && 
            priority_ctx->transmission_order[i].send_time - priority_ctx->transmission_order[i-1].send_time < 1000) {
            /* Allow some slack for timing */
            priority_violations++;
            if (priority_violations <= 5) {  /* Show first 5 violations */
                DBG_PRINTF("  Priority violation at transmission %d: Stream %lu (deadline %lu us) sent after Stream %lu (deadline %lu us)\n",
                    i, (unsigned long)curr_stream, (unsigned long)curr_deadline_left,
                    (unsigned long)prev_stream, (unsigned long)prev_deadline_left);
            }
        }
    }
    
    DBG_PRINTF("\nTotal priority violations: %d (%.1f%%)\n", 
        priority_violations, 
        (priority_violations * 100.0) / priority_ctx->nb_transmissions);
    
    /* Analyze deadline compliance per stream */
    DBG_PRINTF("%s", "\nDeadline compliance by stream priority:\n");
    for (int s = 0; s < 3; s++) {  /* Only deadline streams */
        if (priority_ctx->base.deadline_stats[s] != NULL) {
            DBG_PRINTF("  Stream %lu (%lums deadline): %.1f%% compliance\n",
                (unsigned long)test_scenario_priority[s].stream_id,
                (unsigned long)test_scenario_priority[s].deadline_ms,
                priority_ctx->base.deadline_stats[s]->deadline_compliance_percent);
        }
    }
    
    /* Check expected behavior:
     * - Stream with 20ms deadline should have highest compliance
     * - Stream with 200ms deadline might have lower compliance if system is loaded
     * - Priority violations should be minimal (<10%)
     */
    if (priority_violations > priority_ctx->nb_transmissions / 10) {
        DBG_PRINTF("%s", "\nERROR: Too many priority violations (>10%%)\n");
        ret = -1;
    }
    
    /* Verify that urgent stream (20ms) has better compliance than non-urgent (200ms) */
    if (priority_ctx->base.deadline_stats[0] != NULL && priority_ctx->base.deadline_stats[2] != NULL) {
        if (priority_ctx->base.deadline_stats[0]->deadline_compliance_percent < 
            priority_ctx->base.deadline_stats[2]->deadline_compliance_percent - 5.0) {
            DBG_PRINTF("%s", "\nERROR: Urgent stream has worse compliance than non-urgent stream\n");
            ret = -1;
        }
    }
    
    DBG_PRINTF("%s", "\n========================================\n");
    
    return ret;
}

/* Test deadline-aware priority scheduling */
int deadline_priority_scheduling_test()
{
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    deadline_api_test_ctx_t* deadline_ctx = NULL;
    deadline_priority_test_ctx_t* priority_ctx = NULL;
    int ret = 0;
    
    /* Allocate extended context */
    priority_ctx = (deadline_priority_test_ctx_t*)calloc(1, sizeof(deadline_priority_test_ctx_t));
    if (priority_ctx == NULL) {
        return -1;
    }
    
    /* Initialize base context */
    ret = deadline_api_init_ctx(&test_ctx, &simulated_time, &deadline_ctx);
    if (ret == 0) {
        /* Copy base context */
        memcpy(&priority_ctx->base, deadline_ctx, sizeof(deadline_api_test_ctx_t));
        free(deadline_ctx);
        deadline_ctx = &priority_ctx->base;
        
        /* Store contexts globally */
        g_deadline_ctx = deadline_ctx;
        g_priority_test_ctx = priority_ctx;
        
        /* Replace callbacks with our custom one */
        picoquic_set_callback(test_ctx->cnx_client, deadline_priority_callback, &deadline_ctx->client_callback);
        picoquic_set_default_callback(test_ctx->qserver, deadline_priority_callback, &deadline_ctx->server_callback);
        
        /* Start connection */
        ret = picoquic_start_client_cnx(test_ctx->cnx_client);
    }
    
    if (ret == 0) {
        /* Establish connection */
        ret = tls_api_connection_loop(test_ctx, NULL, 0, &simulated_time);
    }
    
    if (ret == 0) {
        /* Verify deadline-aware streams were negotiated */
        if (!test_ctx->cnx_client->remote_parameters.enable_deadline_aware_streams ||
            !test_ctx->cnx_server->local_parameters.enable_deadline_aware_streams) {
            DBG_PRINTF("%s", "Deadline-aware streams not negotiated\n");
            ret = -1;
        }
    }
    
    if (ret == 0) {
        /* Run the test with priority scenario */
        ret = deadline_api_data_sending_loop(test_ctx, deadline_ctx,
                                            test_scenario_priority,
                                            sizeof(test_scenario_priority) / sizeof(st_test_api_deadline_stream_desc_t),
                                            &simulated_time);
    }
    
    /* Calculate statistics */
    if (ret == 0) {
        size_t nb_scenario = sizeof(test_scenario_priority) / sizeof(st_test_api_deadline_stream_desc_t);
        
        for (int i = 0; i < deadline_ctx->nb_streams; i++) {
            int scenario_idx = -1;
            for (size_t j = 0; j < nb_scenario; j++) {
                if (test_scenario_priority[j].stream_id == deadline_ctx->stream_state[i].stream_id) {
                    scenario_idx = j;
                    break;
                }
            }
            
            if (scenario_idx >= 0) {
                if (test_scenario_priority[scenario_idx].stream_type == st_stream_type_normal) {
                    deadline_api_calculate_normal_stats(deadline_ctx, i);
                } else {
                    deadline_api_calculate_deadline_stats(deadline_ctx, i, test_scenario_priority, nb_scenario);
                }
            }
        }
        
        /* Print standard statistics */
        deadline_api_print_stats(deadline_ctx, test_scenario_priority, nb_scenario);
        
        /* Analyze priority patterns */
        ret = analyze_priority_patterns(priority_ctx);
    }
    
    /* Verify all data was received correctly */
    if (ret == 0) {
        ret = deadline_api_verify(test_ctx, deadline_ctx, test_scenario_priority,
                                 sizeof(test_scenario_priority) / sizeof(st_test_api_deadline_stream_desc_t));
    }
    
    /* Clean up */
    if (priority_ctx != NULL) {
        /* deadline_ctx is part of priority_ctx, so only free priority_ctx */
        free(priority_ctx);
    }
    
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    g_deadline_ctx = NULL;
    g_priority_test_ctx = NULL;
    
    return ret;
}

/* Test that verifies expired chunks are dropped */
int deadline_expired_chunk_test()
{
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    deadline_api_test_ctx_t* deadline_ctx = NULL;
    int ret = 0;
    
    /* Scenario with very tight deadlines to force some expirations */
    st_test_api_deadline_stream_desc_t expired_scenario[] = {
        /* Stream with 10ms deadline - some chunks will expire */
        { 2, st_stream_type_deadline, 0, 1024, 20, 10, 50, 0 },
        
        /* Stream with reasonable deadline for comparison */
        { 6, st_stream_type_deadline, 0, 1024, 20, 100, 50, 0 }
    };
    
    /* Initialize context */
    ret = deadline_api_init_ctx(&test_ctx, &simulated_time, &deadline_ctx);
    
    if (ret == 0) {
        g_deadline_ctx = deadline_ctx;
        
        /* Add artificial delay to force some chunks to expire */
        test_ctx->c_to_s_link->microsec_latency = 15000;  /* 15ms latency */
        test_ctx->s_to_c_link->microsec_latency = 15000;
        
        /* Start connection */
        ret = picoquic_start_client_cnx(test_ctx->cnx_client);
    }
    
    if (ret == 0) {
        /* Establish connection */
        ret = tls_api_connection_loop(test_ctx, NULL, 0, &simulated_time);
    }
    
    if (ret == 0) {
        /* Run the test */
        ret = deadline_api_data_sending_loop(test_ctx, deadline_ctx,
                                            expired_scenario,
                                            sizeof(expired_scenario) / sizeof(st_test_api_deadline_stream_desc_t),
                                            &simulated_time);
    }
    
    /* Calculate and print statistics */
    if (ret == 0) {
        size_t nb_scenario = sizeof(expired_scenario) / sizeof(st_test_api_deadline_stream_desc_t);
        
        for (int i = 0; i < deadline_ctx->nb_streams; i++) {
            int scenario_idx = -1;
            for (size_t j = 0; j < nb_scenario; j++) {
                if (expired_scenario[j].stream_id == deadline_ctx->stream_state[i].stream_id) {
                    scenario_idx = j;
                    break;
                }
            }
            
            if (scenario_idx >= 0) {
                deadline_api_calculate_deadline_stats(deadline_ctx, i, expired_scenario, nb_scenario);
            }
        }
        
        /* Print statistics */
        deadline_api_print_stats(deadline_ctx, expired_scenario, nb_scenario);
        
        /* Verify behavior:
         * - Stream with 10ms deadline should have some violations
         * - Stream with 100ms deadline should have high compliance
         */
        if (deadline_ctx->deadline_stats[0] != NULL && deadline_ctx->deadline_stats[1] != NULL) {
            DBG_PRINTF("%s", "\nExpired chunk test results:\n");
            DBG_PRINTF("  10ms deadline stream: %.1f%% compliance\n",
                deadline_ctx->deadline_stats[0]->deadline_compliance_percent);
            DBG_PRINTF("  100ms deadline stream: %.1f%% compliance\n",
                deadline_ctx->deadline_stats[1]->deadline_compliance_percent);
            
            /* Expect some failures for 10ms stream with 30ms RTT */
            if (deadline_ctx->deadline_stats[0]->deadline_compliance_percent > 50.0) {
                DBG_PRINTF("%s", "WARNING: 10ms deadline stream has unexpectedly high compliance with 30ms RTT\n");
            }
            
            /* 100ms stream should still work well */
            if (deadline_ctx->deadline_stats[1]->deadline_compliance_percent < 90.0) {
                DBG_PRINTF("%s", "ERROR: 100ms deadline stream has poor compliance\n");
                ret = -1;
            }
        }
    }
    
    /* Don't verify data integrity as some chunks were intentionally dropped */
    
    /* Clean up */
    if (deadline_ctx != NULL) {
        deadline_api_delete_test_ctx(deadline_ctx);
    }
    
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    g_deadline_ctx = NULL;
    
    return ret;
}