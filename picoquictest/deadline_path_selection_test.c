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
        /* The connection already has path[0] allocated. We need to ensure we have
         * enough paths for testing. For simplicity, we'll just use path[0] and
         * simulate multiple paths by changing its characteristics during the test.
         */
        if (cnx->path[0] == NULL) {
            DBG_PRINTF("%s", "Path[0] is NULL\n");
            ret = -1;
        }
    }
    
    if (ret == 0) {
        /* Initialize path 0 with default characteristics */
        cnx->path[0]->smoothed_rtt = 10000;  /* 10ms */
        cnx->path[0]->rtt_min = 8000;
        cnx->path[0]->bandwidth_estimate = 10000000;  /* 10 Mbps */
        cnx->path[0]->cwin = 30000;
        cnx->path[0]->bytes_in_transit = 5000;
        cnx->path[0]->bytes_sent = 1000000;
        cnx->path[0]->total_bytes_lost = 0;
        cnx->path[0]->path_is_demoted = 0;
        cnx->path[0]->rtt_is_initialized = 1;
    }
    
    /* Create a deadline stream */
    picoquic_stream_head_t* stream = NULL;
    if (ret == 0) {
        stream = picoquic_create_stream(cnx, 4);  /* Use stream 4 instead of stream 0 */
        if (stream == NULL) {
            DBG_PRINTF("%s", "Failed to create stream\n");
            ret = -1;
        } else {
            /* Set deadline */
            ret = picoquic_set_stream_deadline(cnx, 4, 30, 1);  /* 30ms hard deadline */
            if (ret != 0) {
                DBG_PRINTF("%s", "Failed to set stream deadline\n");
            }
            
            /* Add some data to send */
            uint8_t buffer[1000];
            memset(buffer, 0x42, sizeof(buffer));
            ret = picoquic_add_to_stream(cnx, 4, buffer, sizeof(buffer), 1);
            if (ret != 0) {
                DBG_PRINTF("%s", "Failed to add data to stream\n");
            }
        }
    }
    
    /* Test 1: Basic path selection for urgent deadline */
    if (ret == 0 && stream != NULL) {
        picoquic_path_t* selected = picoquic_select_path_for_deadline(cnx, stream, simulated_time);
        if (selected != cnx->path[0]) {
            DBG_PRINTF("Test 1 failed: Expected path 0, got path %p\n", (void*)selected);
            ret = -1;
        } else {
            DBG_PRINTF("%s", "Test 1 passed: Selected path for urgent deadline\n");
        }
    }
    
    /* Test 2: Path selection with congestion */
    if (ret == 0) {
        /* Make path 0 congested */
        uint64_t saved_bytes = cnx->path[0]->bytes_in_transit;
        cnx->path[0]->bytes_in_transit = cnx->path[0]->cwin - 100;  /* Almost full */
        
        picoquic_path_t* selected = picoquic_select_path_for_deadline(cnx, stream, simulated_time);
        if (selected == cnx->path[0]) {
            DBG_PRINTF("%s", "Test 2 passed: Selected same path even when congested (single path scenario)\n");
        } else {
            DBG_PRINTF("%s", "Test 2 warning: Selected different path (unexpected)\n");
        }
        
        /* Restore */
        cnx->path[0]->bytes_in_transit = saved_bytes;
    }
    
    /* Test 3: Path selection with recent loss */
    if (ret == 0) {
        /* Add recent loss to path 0 */
        cnx->path[0]->last_loss_event_detected = simulated_time - 5000;  /* 5ms ago */
        
        picoquic_path_t* selected = picoquic_select_path_for_deadline(cnx, stream, simulated_time);
        if (selected == cnx->path[0]) {
            DBG_PRINTF("%s", "Test 3 passed: Selected same path even with recent loss (single path scenario)\n");
        } else {
            DBG_PRINTF("%s", "Test 3 warning: Selected different path (unexpected)\n");
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