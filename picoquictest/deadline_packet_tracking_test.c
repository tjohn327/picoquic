/*
* Test that packets correctly track deadline information for smart retransmission
*/

#include "../picoquic/picoquic_internal.h"
#include "../picoquic/picoquic.h"
#include "picoquictest_internal.h"
#include <string.h>
#include <stdlib.h>

/* Test that packets track deadline information when stream data is added */
int deadline_packet_tracking_test()
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
    
    /* Enable deadline awareness */
    if (ret == 0) {
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
    
    /* Create streams with different deadlines */
    picoquic_stream_head_t* stream1 = NULL;
    picoquic_stream_head_t* stream2 = NULL;
    picoquic_stream_head_t* stream3 = NULL;
    
    if (ret == 0) {
        /* Stream 1: 50ms deadline */
        stream1 = picoquic_create_stream(cnx, 4);  /* Use stream 4 instead of 0 */
        if (stream1 == NULL) {
            DBG_PRINTF("%s", "Failed to create stream 1\n");
            ret = -1;
        } else {
            ret = picoquic_set_stream_deadline(cnx, 4, 50, 1); /* 50ms hard deadline */
            if (ret != 0) {
                DBG_PRINTF("%s", "Failed to set deadline for stream 1\n");
            }
        }
    }
    
    if (ret == 0) {
        /* Stream 2: 100ms deadline */
        stream2 = picoquic_create_stream(cnx, 8);  /* Use stream 8 */
        if (stream2 == NULL) {
            DBG_PRINTF("%s", "Failed to create stream 2\n");
            ret = -1;
        } else {
            ret = picoquic_set_stream_deadline(cnx, 8, 100, 0); /* 100ms soft deadline */
            if (ret != 0) {
                DBG_PRINTF("%s", "Failed to set deadline for stream 2\n");
            }
        }
    }
    
    if (ret == 0) {
        /* Stream 3: No deadline */
        stream3 = picoquic_create_stream(cnx, 12);  /* Use stream 12 */
        if (stream3 == NULL) {
            DBG_PRINTF("%s", "Failed to create stream 3\n");
            ret = -1;
        }
    }
    
    /* Add data to streams */
    if (ret == 0) {
        uint8_t buffer[100];
        memset(buffer, 0x42, sizeof(buffer));
        
        ret = picoquic_add_to_stream(cnx, 4, buffer, sizeof(buffer), 0);  /* Stream 1 */
        if (ret == 0) {
            ret = picoquic_add_to_stream(cnx, 8, buffer, sizeof(buffer), 0);  /* Stream 2 */
        }
        if (ret == 0) {
            ret = picoquic_add_to_stream(cnx, 12, buffer, sizeof(buffer), 0);  /* Stream 3 */
        }
        
        if (ret != 0) {
            DBG_PRINTF("%s", "Failed to add data to streams\n");
        }
    }
    
    /* Test 1: Packet with deadline stream data */
    if (ret == 0) {
        /* Create a test packet */
        picoquic_packet_t* packet = picoquic_create_packet(quic);
        if (packet == NULL) {
            DBG_PRINTF("%s", "Failed to create packet\n");
            ret = -1;
        } else {
            /* Simulate adding stream 1 data to packet */
            packet->data_repeat_stream_id = 4;  /* Use stream 4 instead of 0 */
            
            /* Update deadline info */
            picoquic_update_packet_deadline_info(cnx, packet, simulated_time);
            
            /* Check that deadline was tracked */
            if (!packet->contains_deadline_data) {
                DBG_PRINTF("%s", "Test 1 failed: Packet should contain deadline data\n");
                ret = -1;
            } else if (packet->earliest_deadline != stream1->deadline_ctx->absolute_deadline) {
                DBG_PRINTF("Test 1 failed: Expected deadline %lu, got %lu\n",
                    stream1->deadline_ctx->absolute_deadline, packet->earliest_deadline);
                ret = -1;
            } else {
                DBG_PRINTF("%s", "Test 1 passed: Packet correctly tracks deadline\n");
            }
            
            picoquic_recycle_packet(quic, packet);
        }
    }
    
    /* Test 2: Packet with non-deadline stream data */
    if (ret == 0) {
        picoquic_packet_t* packet = picoquic_create_packet(quic);
        if (packet == NULL) {
            DBG_PRINTF("%s", "Failed to create packet for test 2\n");
            ret = -1;
        } else {
            /* Simulate adding stream 3 data (no deadline) */
            packet->data_repeat_stream_id = 12;  /* Stream 3 has no deadline */
            
            /* Update deadline info */
            picoquic_update_packet_deadline_info(cnx, packet, simulated_time);
            
            /* Check that no deadline was tracked */
            if (packet->contains_deadline_data) {
                DBG_PRINTF("%s", "Test 2 failed: Packet should not contain deadline data\n");
                ret = -1;
            } else if (packet->earliest_deadline != UINT64_MAX) {
                DBG_PRINTF("Test 2 failed: Expected no deadline (UINT64_MAX), got %lu\n",
                    packet->earliest_deadline);
                ret = -1;
            } else {
                DBG_PRINTF("%s", "Test 2 passed: Packet correctly has no deadline\n");
            }
            
            picoquic_recycle_packet(quic, packet);
        }
    }
    
    /* Test 3: Packet with soft deadline stream */
    if (ret == 0) {
        picoquic_packet_t* packet = picoquic_create_packet(quic);
        if (packet == NULL) {
            DBG_PRINTF("%s", "Failed to create packet for test 3\n");
            ret = -1;
        } else {
            /* Simulate adding stream 2 data (soft deadline) */
            packet->data_repeat_stream_id = 8;  /* Stream 2 has 100ms soft deadline */
            
            /* Update deadline info */
            picoquic_update_packet_deadline_info(cnx, packet, simulated_time);
            
            /* Check that deadline was tracked (soft deadlines are tracked too) */
            if (!packet->contains_deadline_data) {
                DBG_PRINTF("%s", "Test 3 failed: Packet should contain deadline data for soft deadline\n");
                ret = -1;
            } else if (packet->earliest_deadline != stream2->deadline_ctx->absolute_deadline) {
                DBG_PRINTF("Test 3 failed: Expected deadline %lu, got %lu\n",
                    stream2->deadline_ctx->absolute_deadline, packet->earliest_deadline);
                ret = -1;
            } else {
                DBG_PRINTF("%s", "Test 3 passed: Packet correctly tracks soft deadline\n");
            }
            
            picoquic_recycle_packet(quic, packet);
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