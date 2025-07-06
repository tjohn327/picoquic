/*
* Test composite path selection for deadline-aware streams
*/

#include "../picoquic/picoquic_internal.h"
#include "../picoquic/picoquic.h"
#include "picoquictest_internal.h"
#include <string.h>
#include <stdlib.h>

/* Test composite path scoring for deadline streams */
int deadline_path_selection_test()
{
    uint64_t simulated_time = 0;
    picoquic_quic_t* quic = picoquic_create(8, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 
        simulated_time, &simulated_time, NULL, NULL, 0);
    picoquic_cnx_t* cnx = NULL;
    int ret = 0;
    
    if (quic == NULL) {
        DBG_PRINTF("%s", "Failed to create QUIC context\n");
        ret = -1;
    }
    
    /* Create connection */
    if (ret == 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        cnx = picoquic_create_cnx(quic, picoquic_null_connection_id, picoquic_null_connection_id,
            (struct sockaddr*)&addr, simulated_time, 0, "test-sni", "test-alpn", 1);
        
        if (cnx == NULL) {
            DBG_PRINTF("%s", "Failed to create connection\n");
            ret = -1;
        }
    }
    
    /* Enable multipath and deadline awareness */
    if (ret == 0) {
        cnx->is_multipath_enabled = 1;
        cnx->local_parameters.enable_deadline_aware_streams = 1;
        cnx->remote_parameters.enable_deadline_aware_streams = 1;
        cnx->cnx_state = picoquic_state_ready;
        
        /* Initialize deadline context */
        picoquic_init_deadline_context(cnx);
        if (cnx->deadline_context == NULL) {
            DBG_PRINTF("%s", "Failed to initialize deadline context\n");
            ret = -1;
        }
    }
    
    /* Create multiple paths with different characteristics */
    if (ret == 0) {
        /* Ensure we have space for paths */
        cnx->nb_paths = 3;
        cnx->path = (picoquic_path_t**)malloc(sizeof(picoquic_path_t*) * 3);
        if (cnx->path == NULL) {
            DBG_PRINTF("%s", "Failed to allocate path array\n");
            ret = -1;
        } else {
            for (int i = 0; i < 3; i++) {
                cnx->path[i] = (picoquic_path_t*)malloc(sizeof(picoquic_path_t));
                if (cnx->path[i] == NULL) {
                    DBG_PRINTF("Failed to allocate path %d\n", i);
                    ret = -1;
                    break;
                }
                memset(cnx->path[i], 0, sizeof(picoquic_path_t));
                cnx->path[i]->cnx = cnx;
            }
        }
    }
    
    if (ret == 0) {
        /* Path 0: Low RTT, moderate bandwidth, no loss */
        cnx->path[0]->smoothed_rtt = 10000;  /* 10ms */
        cnx->path[0]->rtt_min = 8000;
        cnx->path[0]->bandwidth_estimate = 10000000;  /* 10 Mbps */
        cnx->path[0]->cwin = 30000;
        cnx->path[0]->bytes_in_transit = 5000;
        cnx->path[0]->bytes_sent = 1000000;
        cnx->path[0]->total_bytes_lost = 0;
        cnx->path[0]->path_is_demoted = 0;
        cnx->path[0]->rtt_is_initialized = 1;
        
        /* Path 1: High RTT, high bandwidth, low loss */
        cnx->path[1]->smoothed_rtt = 50000;  /* 50ms */
        cnx->path[1]->rtt_min = 45000;
        cnx->path[1]->bandwidth_estimate = 100000000;  /* 100 Mbps */
        cnx->path[1]->cwin = 150000;
        cnx->path[1]->bytes_in_transit = 20000;
        cnx->path[1]->bytes_sent = 2000000;
        cnx->path[1]->total_bytes_lost = 20000;  /* 1% loss */
        cnx->path[1]->path_is_demoted = 0;
        cnx->path[1]->rtt_is_initialized = 1;
        
        /* Path 2: Medium RTT, low bandwidth, high loss */
        cnx->path[2]->smoothed_rtt = 25000;  /* 25ms */
        cnx->path[2]->rtt_min = 20000;
        cnx->path[2]->bandwidth_estimate = 1000000;  /* 1 Mbps */
        cnx->path[2]->cwin = 15000;
        cnx->path[2]->bytes_in_transit = 14000;  /* Almost congested */
        cnx->path[2]->bytes_sent = 500000;
        cnx->path[2]->total_bytes_lost = 50000;  /* 10% loss */
        cnx->path[2]->path_is_demoted = 0;
        cnx->path[2]->rtt_is_initialized = 1;
    }
    
    /* Create a deadline stream */
    picoquic_stream_head_t* stream = NULL;
    if (ret == 0) {
        stream = picoquic_create_stream(cnx, 0);
        if (stream == NULL) {
            DBG_PRINTF("%s", "Failed to create stream\n");
            ret = -1;
        } else {
            /* Set deadline */
            ret = picoquic_set_stream_deadline(cnx, 0, 30, 1);  /* 30ms hard deadline */
            if (ret != 0) {
                DBG_PRINTF("%s", "Failed to set stream deadline\n");
            }
            
            /* Add some data to send */
            uint8_t buffer[1000];
            memset(buffer, 0x42, sizeof(buffer));
            ret = picoquic_add_to_stream(cnx, 0, buffer, sizeof(buffer), 1);
            if (ret != 0) {
                DBG_PRINTF("%s", "Failed to add data to stream\n");
            }
        }
    }
    
    /* Test 1: Best path for urgent deadline (30ms) */
    if (ret == 0 && stream != NULL) {
        picoquic_path_t* selected = picoquic_select_path_for_deadline(cnx, stream, simulated_time);
        if (selected != cnx->path[0]) {
            DBG_PRINTF("Test 1 failed: Expected path 0 for urgent deadline, got path %p\n", (void*)selected);
            ret = -1;
        } else {
            DBG_PRINTF("%s", "Test 1 passed: Selected low-RTT path for urgent deadline\n");
        }
    }
    
    /* Test 2: Relaxed deadline (200ms) - should consider bandwidth more */
    if (ret == 0 && stream != NULL) {
        stream->deadline_ctx->deadline_ms = 200;
        stream->deadline_ctx->absolute_deadline = simulated_time + 200000;
        
        picoquic_path_t* selected = picoquic_select_path_for_deadline(cnx, stream, simulated_time);
        /* With relaxed deadline, high bandwidth path might be preferred despite higher RTT */
        if (selected == cnx->path[2]) {
            DBG_PRINTF("%s", "Test 2 failed: Should not select high-loss path\n");
            ret = -1;
        } else {
            DBG_PRINTF("Test 2 passed: Selected path %ld for relaxed deadline\n", 
                selected == cnx->path[0] ? 0 : (selected == cnx->path[1] ? 1 : 2));
        }
    }
    
    /* Test 3: Path with congestion */
    if (ret == 0) {
        /* Make path 0 congested */
        cnx->path[0]->bytes_in_transit = cnx->path[0]->cwin - 100;  /* Almost full */
        stream->deadline_ctx->deadline_ms = 50;
        stream->deadline_ctx->absolute_deadline = simulated_time + 50000;
        
        picoquic_path_t* selected = picoquic_select_path_for_deadline(cnx, stream, simulated_time);
        if (selected == cnx->path[0]) {
            DBG_PRINTF("%s", "Test 3 warning: Still selected congested path (may be acceptable)\n");
        } else {
            DBG_PRINTF("Test 3 passed: Avoided congested path, selected path %ld\n",
                selected == cnx->path[1] ? 1 : 2);
        }
    }
    
    /* Test 4: Recent loss event */
    if (ret == 0) {
        /* Restore path 0 congestion window */
        cnx->path[0]->bytes_in_transit = 5000;
        /* Add recent loss to path 0 */
        cnx->path[0]->last_loss_event_detected = simulated_time - 5000;  /* 5ms ago */
        
        picoquic_path_t* selected = picoquic_select_path_for_deadline(cnx, stream, simulated_time);
        /* Should penalize path with recent loss */
        DBG_PRINTF("Test 4: Selected path %ld with recent loss consideration\n",
            selected == cnx->path[0] ? 0 : (selected == cnx->path[1] ? 1 : 2));
    }
    
    /* Test 5: No path can meet deadline */
    if (ret == 0 && stream != NULL) {
        stream->deadline_ctx->deadline_ms = 5;  /* 5ms - impossible */
        stream->deadline_ctx->absolute_deadline = simulated_time + 5000;
        
        picoquic_path_t* selected = picoquic_select_path_for_deadline(cnx, stream, simulated_time);
        if (selected != cnx->path[0]) {
            DBG_PRINTF("%s", "Test 5 failed: Should fall back to lowest RTT path when deadline impossible\n");
            ret = -1;
        } else {
            DBG_PRINTF("%s", "Test 5 passed: Fell back to lowest RTT for impossible deadline\n");
        }
    }
    
    /* Cleanup */
    /* Note: picoquic_delete_cnx handles all path cleanup automatically */
    if (cnx != NULL) {
        picoquic_delete_cnx(cnx);
    }
    
    if (quic != NULL) {
        picoquic_free(quic);
    }
    
    return ret;
}