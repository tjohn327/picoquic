/*
* Comprehensive integration test for deadline-aware multipath streams
*/

#include "../picoquic/picoquic_internal.h"
#include "../picoquic/picoquic.h"
#include "picoquictest_internal.h"
#include <string.h>
#include <stdlib.h>

/* Helper to simulate network conditions on a path */
static void simulate_path_conditions(picoquic_path_t* path, uint64_t rtt_us, uint64_t bandwidth_bps,
    double loss_rate, uint64_t bytes_in_transit)
{
    path->smoothed_rtt = rtt_us;
    path->rtt_min = rtt_us * 0.9;
    path->bandwidth_estimate = bandwidth_bps;
    path->cwin = (bandwidth_bps * rtt_us) / (8 * 1000000); /* BDP */
    path->bytes_in_transit = bytes_in_transit;
    path->rtt_is_initialized = 1;
    
    if (loss_rate > 0) {
        uint64_t total_sent = 1000000; /* 1MB sent */
        path->total_bytes_lost = (uint64_t)(total_sent * loss_rate);
        path->bytes_sent = total_sent;
    }
}

/* Test multipath deadline-aware streams with various scenarios */
int deadline_integration_test()
{
    uint64_t simulated_time = 0;
    picoquic_quic_t* quic = picoquic_create(8, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 
        simulated_time, &simulated_time, NULL, NULL, 0);
    picoquic_cnx_t* cnx = NULL;
    picoquic_stream_head_t* urgent_stream = NULL;
    picoquic_stream_head_t* moderate_stream = NULL;
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
        
        /* Set flow control limits */
        cnx->maxdata_remote = 100000000; /* 100MB */
        cnx->data_sent = 0;
        cnx->max_stream_id_bidir_remote = 1000;
        
        /* Initialize deadline context */
        picoquic_init_deadline_context(cnx);
        if (cnx->deadline_context == NULL) {
            DBG_PRINTF("%s", "Failed to initialize deadline context\n");
            ret = -1;
        }
        
        /* Configure fairness: 20% minimum for non-deadline, 20ms max starvation */
        picoquic_set_deadline_fairness_params(cnx, 0.2, 20000);
    }
    
    /* Create multiple paths with different characteristics */
    if (ret == 0) {
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
        /* Path 0: Fast, reliable (5G) */
        simulate_path_conditions(cnx->path[0], 10000, 50000000, 0.001, 5000); /* 10ms, 50Mbps, 0.1% loss */
        
        /* Path 1: Medium, moderate (WiFi) */
        simulate_path_conditions(cnx->path[1], 30000, 20000000, 0.01, 10000); /* 30ms, 20Mbps, 1% loss */
        
        /* Path 2: Slow, lossy (Cellular) */
        simulate_path_conditions(cnx->path[2], 100000, 5000000, 0.05, 20000); /* 100ms, 5Mbps, 5% loss */
    }
    
    /* Scenario 1: Urgent deadline stream selection */
    if (ret == 0) {
        DBG_PRINTF("%s", "\n=== Scenario 1: Urgent deadline stream path selection ===\n");
        
        /* Create stream with 20ms deadline */
        urgent_stream = picoquic_create_stream(cnx, 0);
        if (urgent_stream == NULL) {
            DBG_PRINTF("%s", "Failed to create urgent stream\n");
            ret = -1;
        } else {
            urgent_stream->maxdata_remote = 10000000;
            ret = picoquic_set_stream_deadline(cnx, 0, 20, 1); /* 20ms hard deadline */
            
            /* Add data */
            uint8_t buffer[1000];
            memset(buffer, 0x42, sizeof(buffer));
            ret |= picoquic_add_to_stream(cnx, 0, buffer, sizeof(buffer), 0);
            
            /* Select path - should choose fast path */
            picoquic_path_t* selected = picoquic_select_path_for_deadline(cnx, urgent_stream, simulated_time);
            if (selected != cnx->path[0]) {
                DBG_PRINTF("Scenario 1 failed: Expected fast path for urgent deadline, got path %d\n",
                    selected ? (int)selected->unique_path_id : -1);
                ret = -1;
            } else {
                DBG_PRINTF("%s", "Scenario 1 passed: Selected fast path for urgent deadline\n");
            }
        }
    }
    
    /* Scenario 2: Congestion-aware path switching */
    if (ret == 0) {
        DBG_PRINTF("%s", "\n=== Scenario 2: Congestion-aware path switching ===\n");
        
        /* Congest the fast path */
        cnx->path[0]->bytes_in_transit = cnx->path[0]->cwin - 100; /* Almost full */
        
        /* Create stream with 50ms deadline */
        moderate_stream = picoquic_create_stream(cnx, 4);
        if (moderate_stream == NULL) {
            DBG_PRINTF("%s", "Failed to create moderate stream\n");
            ret = -1;
        } else {
            moderate_stream->maxdata_remote = 10000000;
            ret = picoquic_set_stream_deadline(cnx, 4, 50, 1); /* 50ms hard deadline */
            
            /* Add data */
            uint8_t buffer[5000];
            memset(buffer, 0x43, sizeof(buffer));
            ret |= picoquic_add_to_stream(cnx, 4, buffer, sizeof(buffer), 0);
            
            /* Select path - should choose medium path due to congestion */
            picoquic_path_t* selected = picoquic_select_path_for_deadline(cnx, moderate_stream, simulated_time);
            if (selected != cnx->path[1]) {
                DBG_PRINTF("Scenario 2 failed: Expected medium path due to congestion, got path %d\n",
                    selected ? (int)selected->unique_path_id : -1);
                ret = -1;
            } else {
                DBG_PRINTF("%s", "Scenario 2 passed: Selected medium path due to congestion\n");
            }
        }
        
        /* Restore fast path */
        cnx->path[0]->bytes_in_transit = 5000;
    }
    
    /* Scenario 3: Smart retransmission with deadline expiry */
    if (ret == 0) {
        DBG_PRINTF("%s", "\n=== Scenario 3: Smart retransmission ===\n");
        
        /* Create a packet with expired deadline */
        picoquic_packet_t* packet = picoquic_create_packet(quic);
        if (packet == NULL) {
            DBG_PRINTF("%s", "Failed to create packet\n");
            ret = -1;
        } else {
            /* Set up packet with expired hard deadline */
            packet->data_repeat_stream_id = 0; /* Stream 0 has expired deadline */
            packet->send_path = cnx->path[2]; /* Originally sent on slow path */
            picoquic_update_packet_deadline_info(cnx, packet, simulated_time);
            
            /* Advance time past deadline */
            simulated_time = 25000; /* 25ms - deadline expired */
            urgent_stream->deadline_ctx->absolute_deadline = 20000; /* Expired 5ms ago */
            
            /* Check if retransmission should be skipped */
            if (!picoquic_should_skip_packet_retransmit(cnx, packet, simulated_time)) {
                DBG_PRINTF("%s", "Scenario 3 failed: Should skip retransmission for expired hard deadline\n");
                ret = -1;
            } else {
                DBG_PRINTF("%s", "Scenario 3 passed: Correctly skipped expired deadline retransmission\n");
            }
            
            picoquic_recycle_packet(quic, packet);
        }
    }
    
    /* Scenario 4: Fairness enforcement with mixed traffic */
    if (ret == 0) {
        DBG_PRINTF("%s", "\n=== Scenario 4: Fairness with mixed traffic ===\n");
        
        /* Create non-deadline streams */
        picoquic_stream_head_t* regular1 = picoquic_create_stream(cnx, 8);
        picoquic_stream_head_t* regular2 = picoquic_create_stream(cnx, 12);
        
        if (regular1 == NULL || regular2 == NULL) {
            DBG_PRINTF("%s", "Failed to create regular streams\n");
            ret = -1;
        } else {
            regular1->maxdata_remote = 10000000;
            regular2->maxdata_remote = 10000000;
            
            /* Add data to all streams */
            uint8_t buffer[1000];
            for (int i = 0; i < 10; i++) {
                picoquic_add_to_stream(cnx, 0, buffer, sizeof(buffer), 0);  /* Deadline */
                picoquic_add_to_stream(cnx, 4, buffer, sizeof(buffer), 0);  /* Deadline */
                picoquic_add_to_stream(cnx, 8, buffer, sizeof(buffer), 0);  /* Regular */
                picoquic_add_to_stream(cnx, 12, buffer, sizeof(buffer), 0); /* Regular */
            }
            
            /* Mark all as active */
            picoquic_insert_output_stream(cnx, urgent_stream);
            picoquic_insert_output_stream(cnx, moderate_stream);
            picoquic_insert_output_stream(cnx, regular1);
            picoquic_insert_output_stream(cnx, regular2);
            
            /* Simulate heavy deadline traffic */
            cnx->deadline_context->window_start_time = simulated_time;
            cnx->deadline_context->deadline_bytes_sent = 8500;  /* 85% to deadline */
            cnx->deadline_context->non_deadline_bytes_sent = 1500; /* 15% to regular */
            
            /* Next selection should prioritize regular streams for fairness */
            int regular_selected = 0;
            for (int i = 0; i < 5; i++) {
                picoquic_stream_head_t* selected = picoquic_find_ready_stream_edf(cnx, NULL);
                if (selected != NULL && (selected->deadline_ctx == NULL || 
                    !selected->deadline_ctx->deadline_enabled)) {
                    regular_selected++;
                }
                if (selected != NULL) {
                    selected->last_time_data_sent = simulated_time;
                }
                simulated_time += 1000;
            }
            
            if (regular_selected < 2) {
                DBG_PRINTF("Scenario 4 failed: Expected fairness enforcement, got %d regular selections\n",
                    regular_selected);
                ret = -1;
            } else {
                DBG_PRINTF("Scenario 4 passed: Fairness enforced with %d regular selections\n",
                    regular_selected);
            }
        }
    }
    
    /* Scenario 5: Deadline miss handling and partial reliability */
    if (ret == 0) {
        DBG_PRINTF("%s", "\n=== Scenario 5: Deadline miss and partial reliability ===\n");
        
        /* Create stream with immediate deadline */
        picoquic_stream_head_t* immediate = picoquic_create_stream(cnx, 16);
        if (immediate == NULL) {
            DBG_PRINTF("%s", "Failed to create immediate stream\n");
            ret = -1;
        } else {
            immediate->maxdata_remote = 10000000;
            ret = picoquic_set_stream_deadline(cnx, 16, 1, 1); /* 1ms hard deadline - impossible */
            
            /* Add data */
            uint8_t buffer[10000];
            memset(buffer, 0x44, sizeof(buffer));
            ret |= picoquic_add_to_stream(cnx, 16, buffer, sizeof(buffer), 0);
            
            /* Advance time to miss deadline */
            simulated_time += 2000; /* 2ms - deadline missed */
            
            /* Check deadlines - should drop data */
            picoquic_check_stream_deadlines(cnx, simulated_time);
            
            /* Verify data was dropped */
            if (immediate->deadline_ctx->bytes_dropped == 0) {
                DBG_PRINTF("%s", "Scenario 5 failed: Expected data to be dropped for missed deadline\n");
                ret = -1;
            } else if (immediate->deadline_ctx->dropped_ranges.ack_tree.root == NULL) {
                DBG_PRINTF("%s", "Scenario 5 failed: Expected dropped ranges to be recorded\n");
                ret = -1;
            } else {
                DBG_PRINTF("Scenario 5 passed: Data dropped (%lu bytes) and ranges recorded\n",
                    (unsigned long)immediate->deadline_ctx->bytes_dropped);
            }
        }
    }
    
    /* Summary */
    if (ret == 0) {
        DBG_PRINTF("%s", "\n=== All integration test scenarios passed ===\n");
        DBG_PRINTF("%s", "✓ Urgent deadline path selection\n");
        DBG_PRINTF("%s", "✓ Congestion-aware path switching\n");
        DBG_PRINTF("%s", "✓ Smart retransmission with deadline checking\n");
        DBG_PRINTF("%s", "✓ Fairness enforcement with mixed traffic\n");
        DBG_PRINTF("%s", "✓ Deadline miss handling and partial reliability\n");
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