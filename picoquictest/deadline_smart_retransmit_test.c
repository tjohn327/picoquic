/*
* Test smart retransmission for deadline-aware streams
*/

#include "../picoquic/picoquic_internal.h"
#include "../picoquic/picoquic.h"
#include "picoquictest_internal.h"
#include <string.h>
#include <stdlib.h>

/* Helper function to simulate packet loss */
static void simulate_packet_loss(picoquic_packet_t* packet)
{
    /* Mark packet as lost by setting it to be older than 3 other packets */
    packet->delivered_prior = 3;
    packet->delivered_time_prior = 0;
}

/* Test smart retransmission with deadline awareness */
int deadline_smart_retransmit_test()
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
    
    /* Create multiple paths */
    if (ret == 0) {
        cnx->nb_paths = 2;
        cnx->path = (picoquic_path_t**)malloc(sizeof(picoquic_path_t*) * 2);
        if (cnx->path == NULL) {
            DBG_PRINTF("%s", "Failed to allocate path array\n");
            ret = -1;
        } else {
            for (int i = 0; i < 2; i++) {
                cnx->path[i] = (picoquic_path_t*)malloc(sizeof(picoquic_path_t));
                if (cnx->path[i] == NULL) {
                    DBG_PRINTF("Failed to allocate path %d\n", i);
                    ret = -1;
                    break;
                }
                memset(cnx->path[i], 0, sizeof(picoquic_path_t));
                cnx->path[i]->cnx = cnx;
                /* Initialize first tuple for path validation */
                cnx->path[i]->first_tuple = (picoquic_tuple_t*)malloc(sizeof(picoquic_tuple_t));
                if (cnx->path[i]->first_tuple != NULL) {
                    memset(cnx->path[i]->first_tuple, 0, sizeof(picoquic_tuple_t));
                    cnx->path[i]->first_tuple->challenge_verified = 1;
                }
                cnx->path[i]->unique_path_id = i;
            }
        }
    }
    
    if (ret == 0) {
        /* Path 0: Slow path (50ms RTT) */
        cnx->path[0]->smoothed_rtt = 50000;
        cnx->path[0]->rtt_min = 45000;
        cnx->path[0]->bandwidth_estimate = 1000000;  /* 1 Mbps */
        cnx->path[0]->cwin = 15000;
        cnx->path[0]->bytes_in_transit = 5000;
        cnx->path[0]->rtt_is_initialized = 1;
        
        /* Path 1: Fast path (10ms RTT) */
        cnx->path[1]->smoothed_rtt = 10000;
        cnx->path[1]->rtt_min = 8000;
        cnx->path[1]->bandwidth_estimate = 10000000;  /* 10 Mbps */
        cnx->path[1]->cwin = 30000;
        cnx->path[1]->bytes_in_transit = 5000;
        cnx->path[1]->rtt_is_initialized = 1;
    }
    
    /* Test 1: Skip retransmission for expired hard deadline */
    if (ret == 0) {
        /* Create a stream with expired deadline */
        picoquic_stream_head_t* stream = picoquic_create_stream(cnx, 4);  /* Use stream 4 instead of 0 */
        if (stream == NULL) {
            DBG_PRINTF("%s", "Failed to create stream\n");
            ret = -1;
        } else {
            /* Set deadline that has already expired */
            ret = picoquic_set_stream_deadline(cnx, 4, 10, 1); /* 10ms hard deadline */
            /* Simulate time advancement so deadline is expired */
            simulated_time = 20000; /* 20ms - deadline should have expired at 10ms */
            stream->deadline_ctx->absolute_deadline = 10000; /* Expired 10ms ago */
            
            /* Create a test packet */
            picoquic_packet_t* packet = picoquic_create_packet(quic);
            if (packet == NULL) {
                DBG_PRINTF("%s", "Failed to create packet\n");
                ret = -1;
            } else {
                packet->data_repeat_stream_id = 4;
                packet->send_path = cnx->path[0];
                picoquic_update_packet_deadline_info(cnx, packet, 0); /* Use original time when packet was created */
                
                /* Test skip retransmit function */
                if (!picoquic_should_skip_packet_retransmit(cnx, packet, simulated_time)) {
                    DBG_PRINTF("%s", "Test 1 failed: Should skip retransmit for expired hard deadline\n");
                    ret = -1;
                } else {
                    DBG_PRINTF("%s", "Test 1 passed: Correctly skipped expired hard deadline\n");
                }
                
                picoquic_recycle_packet(quic, packet);
            }
        }
    }
    
    /* Test 2: Don't skip retransmission for soft deadline */
    if (ret == 0) {
        picoquic_stream_head_t* stream = picoquic_find_stream(cnx, 4);
        if (stream != NULL) {
            /* Change to soft deadline */
            stream->deadline_ctx->deadline_type = 0; /* Soft deadline */
            
            picoquic_packet_t* packet = picoquic_create_packet(quic);
            if (packet == NULL) {
                DBG_PRINTF("%s", "Failed to create packet for test 2\n");
                ret = -1;
            } else {
                packet->data_repeat_stream_id = 4;
                packet->send_path = cnx->path[0];
                picoquic_update_packet_deadline_info(cnx, packet, 0); /* Use original time when packet was created */
                
                /* Should not skip soft deadline even if expired */
                if (picoquic_should_skip_packet_retransmit(cnx, packet, simulated_time)) {
                    DBG_PRINTF("%s", "Test 2 failed: Should not skip soft deadline\n");
                    ret = -1;
                } else {
                    DBG_PRINTF("%s", "Test 2 passed: Correctly retransmits soft deadline\n");
                }
                
                picoquic_recycle_packet(quic, packet);
            }
        }
    }
    
    /* Test 3: Path selection for retransmission */
    if (ret == 0) {
        /* Create stream with future deadline */
        picoquic_stream_head_t* stream = picoquic_find_stream(cnx, 4);
        if (stream != NULL) {
            stream->deadline_ctx->deadline_type = 1; /* Hard deadline */
            stream->deadline_ctx->absolute_deadline = simulated_time + 30000; /* 30ms in future */
            
            /* Add some data to stream */
            uint8_t buffer[100];
            memset(buffer, 0x42, sizeof(buffer));
            picoquic_add_to_stream(cnx, 4, buffer, sizeof(buffer), 0);
            
            picoquic_packet_t* packet = picoquic_create_packet(quic);
            if (packet == NULL) {
                DBG_PRINTF("%s", "Failed to create packet for test 3\n");
                ret = -1;
            } else {
                packet->data_repeat_stream_id = 4;
                packet->send_path = cnx->path[0]; /* Originally sent on slow path */
                picoquic_update_packet_deadline_info(cnx, packet, simulated_time);
                
                /* Select path for retransmit should choose faster path */
                picoquic_path_t* selected = picoquic_select_path_for_retransmit(cnx, packet, simulated_time);
                if (selected != cnx->path[1]) {
                    DBG_PRINTF("Test 3 failed: Should select fast path for retransmit, got path %d\n",
                        selected ? (int)selected->unique_path_id : -1);
                    ret = -1;
                } else {
                    DBG_PRINTF("%s", "Test 3 passed: Correctly selected fast path for retransmit\n");
                }
                
                picoquic_recycle_packet(quic, packet);
            }
        }
    }
    
    /* Test 4: Non-deadline packet uses original path */
    if (ret == 0) {
        /* Create non-deadline stream */
        picoquic_stream_head_t* stream = picoquic_create_stream(cnx, 8);  /* Different stream ID */
        if (stream == NULL) {
            DBG_PRINTF("%s", "Failed to create non-deadline stream\n");
            ret = -1;
        } else {
            picoquic_packet_t* packet = picoquic_create_packet(quic);
            if (packet == NULL) {
                DBG_PRINTF("%s", "Failed to create packet for test 4\n");
                ret = -1;
            } else {
                packet->data_repeat_stream_id = 8;
                packet->send_path = cnx->path[0];
                packet->contains_deadline_data = 0;
                
                /* Should use original path for non-deadline */
                picoquic_path_t* selected = picoquic_select_path_for_retransmit(cnx, packet, simulated_time);
                if (selected != cnx->path[0]) {
                    DBG_PRINTF("Test 4 failed: Should use original path for non-deadline, got path %d\n",
                        selected ? (int)selected->unique_path_id : -1);
                    ret = -1;
                } else {
                    DBG_PRINTF("%s", "Test 4 passed: Correctly used original path for non-deadline\n");
                }
                
                picoquic_recycle_packet(quic, packet);
            }
        }
    }
    
    /* Cleanup */
    if (cnx != NULL) {
        picoquic_delete_cnx(cnx);
    }
    
    if (quic != NULL) {
        picoquic_free(quic);
    }
    
    return ret;
}