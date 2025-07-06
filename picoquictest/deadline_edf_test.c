/*
* Author: Test
* Copyright (c) 2025
*/

#include <stdlib.h>
#include <string.h>
#include "picoquic.h"
#include "picoquic_internal.h"
#include "picoquictest_internal.h"
#include "../picoquic/tls_api.h"
#include "autoqlog.h"

/* Test EDF scheduling for deadline-aware streams */

int deadline_edf_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    picoquic_quic_t* quic = picoquic_create(8, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, simulated_time, &simulated_time, NULL, NULL, 0);
    
    if (quic == NULL) {
        DBG_PRINTF("%s", "Cannot create QUIC context\n");
        ret = -1;
    }

    if (ret == 0) {
        /* Create a connection */
        struct sockaddr_in saddr;
        memset(&saddr, 0, sizeof(struct sockaddr_in));
        saddr.sin_family = AF_INET;
        saddr.sin_port = 4433;
        
        picoquic_cnx_t* cnx = picoquic_create_cnx(quic, 
            picoquic_null_connection_id, picoquic_null_connection_id,
            (struct sockaddr*)&saddr, simulated_time,
            0, "test-sni", "test-alpn", 1);
            
        if (cnx == NULL) {
            DBG_PRINTF("%s", "Cannot create connection\n");
            ret = -1;
        } else {
            /* Enable deadline-aware streams on both sides */
            cnx->local_parameters.enable_deadline_aware_streams = 1;
            cnx->remote_parameters.enable_deadline_aware_streams = 1;
            
            /* Set flow control limits to allow data */
            cnx->maxdata_remote = 100000;
            cnx->maxdata_local = 100000;
            cnx->max_stream_id_bidir_remote = 100;
            cnx->max_stream_id_unidir_remote = 100;
            
            /* Initialize deadline context */
            picoquic_init_deadline_context(cnx);
            
            if (cnx->deadline_context == NULL) {
                DBG_PRINTF("%s", "Failed to initialize deadline context\n");
                ret = -1;
            } else {
                /* Create multiple streams with different deadlines */
                uint64_t stream_ids[] = {4, 8, 12};  /* Use streams 4+ to avoid reserved streams */
                uint64_t deadlines[] = {100, 50, 150}; /* ms */
                int is_hard[] = {1, 1, 0};
                
                for (int i = 0; i < 3 && ret == 0; i++) {
                    picoquic_stream_head_t* stream = picoquic_create_stream(cnx, stream_ids[i]);
                    if (stream == NULL) {
                        DBG_PRINTF("Failed to create stream %d\n", i);
                        ret = -1;
                    } else {
                        /* Set flow control for the stream */
                        stream->maxdata_remote = 10000;
                        stream->maxdata_local = 10000;
                        
                        /* Set deadline */
                        ret = picoquic_set_stream_deadline(cnx, stream_ids[i], deadlines[i], is_hard[i]);
                        if (ret != 0) {
                            DBG_PRINTF("Failed to set deadline for stream %d\n", i);
                        } else {
                            /* Add some data to the stream */
                            ret = picoquic_add_to_stream(cnx, stream_ids[i], 
                                (const uint8_t*)"test data", 9, 0);
                            if (ret != 0) {
                                DBG_PRINTF("Failed to add data to stream %d\n", i);
                            } else {
                                /* Mark stream as having data ready */
                                if (!stream->is_output_stream) {
                                    picoquic_insert_output_stream(cnx, stream);
                                }
                            }
                        }
                    }
                }
                
                if (ret == 0) {
                    /* Test EDF scheduling - should select stream with earliest deadline */
                    picoquic_stream_head_t* selected = picoquic_find_ready_stream_edf(cnx, cnx->path[0]);
                    
                    if (selected == NULL) {
                        DBG_PRINTF("%s", "EDF scheduler returned NULL\n");
                        ret = -1;
                    } else if (selected->stream_id != 8) {
                        /* Stream 8 has deadline of 50ms, should be selected first */
                        DBG_PRINTF("EDF selected stream %" PRIu64 " instead of stream 8\n", 
                            selected->stream_id);
                        ret = -1;
                    } else {
                        DBG_PRINTF("EDF correctly selected stream %" PRIu64 " with earliest deadline\n",
                            selected->stream_id);
                    }
                    
                    /* Test deadline expiration */
                    if (ret == 0) {
                        /* Advance time past first deadline */
                        simulated_time += 60 * 1000; /* 60ms */
                        picoquic_check_stream_deadlines(cnx, simulated_time);
                        
                        /* Stream 8 should have expired (hard deadline at 50ms) */
                        picoquic_stream_head_t* stream8 = picoquic_find_stream(cnx, 8);
                        if (stream8 != NULL && stream8->deadline_ctx != NULL) {
                            if (stream8->deadline_ctx->deadlines_missed != 1) {
                                DBG_PRINTF("Stream 8 should have missed deadline, but missed count is %" PRIu64 "\n",
                                    stream8->deadline_ctx->deadlines_missed);
                                ret = -1;
                            } else if (stream8->send_queue != NULL) {
                                DBG_PRINTF("%s", "Stream 8 data should be dropped but send_queue is not NULL\n");
                                ret = -1;
                            } else {
                                DBG_PRINTF("%s", "Stream 8 deadline correctly expired and data dropped\n");
                            }
                        }
                        
                        /* Now EDF should select stream 4 (deadline 100ms) */
                        if (ret == 0) {
                            selected = picoquic_find_ready_stream_edf(cnx, cnx->path[0]);
                            if (selected == NULL) {
                                DBG_PRINTF("%s", "EDF scheduler returned NULL after deadline expiry\n");
                                ret = -1;
                            } else if (selected->stream_id != 4) {
                                DBG_PRINTF("EDF selected stream %" PRIu64 " instead of stream 4\n",
                                    selected->stream_id);
                                ret = -1;
                            } else {
                                DBG_PRINTF("%s", "EDF correctly selected next earliest deadline stream\n");
                            }
                        }
                    }
                }
            }
            
            /* Clean up connection */
            picoquic_delete_cnx(cnx);
        }
    }
    
    if (quic != NULL) {
        picoquic_free(quic);
    }
    
    return ret;
}