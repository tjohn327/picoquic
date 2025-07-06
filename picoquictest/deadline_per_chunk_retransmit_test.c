/*
* Test smart retransmission with per-chunk deadlines
*/

#include "../picoquic/picoquic_internal.h"
#include "../picoquictest/picoquictest_internal.h"
#include <stdlib.h>
#include <string.h>

/* Test that smart retransmission correctly handles per-chunk deadlines */
int deadline_per_chunk_retransmit_test()
{
    uint64_t simulated_time = 0;
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    int ret = 0;
    
    /* Create QUIC context */
    quic = picoquic_create(8, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 
        simulated_time, &simulated_time, NULL, NULL, 0);
    if (quic == NULL) {
        DBG_PRINTF("%s", "Failed to create QUIC context\n");
        return -1;
    }
    
    /* Create connection */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    cnx = picoquic_create_cnx(quic, picoquic_null_connection_id, picoquic_null_connection_id,
        (struct sockaddr*)&addr, simulated_time, 0, "test-sni", "test-alpn", 1);
    
    if (cnx == NULL) {
        DBG_PRINTF("%s", "Failed to create connection\n");
        picoquic_free(quic);
        return -1;
    }
    
    /* Enable deadline awareness */
    cnx->local_parameters.enable_deadline_aware_streams = 1;
    cnx->remote_parameters.enable_deadline_aware_streams = 1;
    cnx->cnx_state = picoquic_state_ready;
    
    /* Initialize deadline context */
    picoquic_init_deadline_context(cnx);
    if (cnx->deadline_context == NULL) {
        DBG_PRINTF("%s", "Failed to initialize deadline context\n");
        ret = -1;
    }
    
    /* Test 1: Packet with mixed expired/valid chunks */
    if (ret == 0) {
        DBG_PRINTF("%s", "\nTest 1: Packet with data from multiple chunks\n");
        
        /* Create stream with deadline */
        picoquic_stream_head_t* stream = picoquic_create_stream(cnx, 4);
        if (stream == NULL) {
            DBG_PRINTF("%s", "Failed to create stream\n");
            ret = -1;
        } else {
            /* Set stream deadline */
            ret = picoquic_set_stream_deadline(cnx, 4, 100, 1); /* 100ms hard deadline */
            
            if (ret == 0) {
                /* Add first chunk with short deadline */
                uint8_t data1[100];
                memset(data1, 'A', 100);
                ret = picoquic_add_to_stream(cnx, 4, data1, 100, 0);
                
                /* Manually set chunk deadline to be expired */
                if (stream->send_queue != NULL) {
                    stream->send_queue->chunk_deadline = 50000; /* 50ms deadline */
                }
                
                /* Advance time and add second chunk */
                simulated_time = 60000; /* 60ms */
                
                uint8_t data2[100];
                memset(data2, 'B', 100);
                ret = picoquic_add_to_stream(cnx, 4, data2, 100, 0);
                
                /* Second chunk should have deadline at 160ms (60ms + 100ms) */
                
                /* Create packet that spans both chunks */
                picoquic_packet_t* packet = picoquic_create_packet(quic);
                if (packet == NULL) {
                    DBG_PRINTF("%s", "Failed to create packet\n");
                    ret = -1;
                } else {
                    packet->data_repeat_stream_id = 4;
                    packet->data_repeat_stream_offset = 0;
                    packet->data_repeat_stream_data_length = 200; /* Spans both chunks */
                    
                    /* Update deadline info */
                    picoquic_update_packet_deadline_info(cnx, packet, simulated_time);
                    
                    /* The earliest deadline should be from the first chunk (50ms) */
                    DBG_PRINTF("Packet earliest deadline: %lu, current time: %lu\n",
                        (unsigned long)packet->earliest_deadline, (unsigned long)simulated_time);
                    
                    /* Should skip retransmit since earliest chunk deadline has expired */
                    if (!picoquic_should_skip_packet_retransmit(cnx, packet, simulated_time)) {
                        DBG_PRINTF("%s", "ERROR: Should skip retransmit for packet with expired chunk\n");
                        ret = -1;
                    } else {
                        DBG_PRINTF("%s", "SUCCESS: Correctly skipped retransmit for expired chunk\n");
                    }
                    
                    picoquic_recycle_packet(quic, packet);
                }
            }
        }
    }
    
    /* Test 2: Packet with only non-expired chunks */
    if (ret == 0) {
        DBG_PRINTF("%s", "\nTest 2: Packet with only non-expired chunks\n");
        
        /* Create new packet for second chunk only */
        picoquic_packet_t* packet = picoquic_create_packet(quic);
        if (packet == NULL) {
            DBG_PRINTF("%s", "Failed to create packet\n");
            ret = -1;
        } else {
            packet->data_repeat_stream_id = 4;
            packet->data_repeat_stream_offset = 100; /* Second chunk only */
            packet->data_repeat_stream_data_length = 100;
            
            /* Update deadline info */
            picoquic_update_packet_deadline_info(cnx, packet, simulated_time);
            
            DBG_PRINTF("Packet earliest deadline: %lu, current time: %lu\n",
                (unsigned long)packet->earliest_deadline, (unsigned long)simulated_time);
            
            /* Should NOT skip since second chunk deadline hasn't expired */
            if (picoquic_should_skip_packet_retransmit(cnx, packet, simulated_time)) {
                DBG_PRINTF("%s", "ERROR: Should NOT skip retransmit for non-expired chunk\n");
                ret = -1;
            } else {
                DBG_PRINTF("%s", "SUCCESS: Correctly retransmitting non-expired chunk\n");
            }
            
            picoquic_recycle_packet(quic, packet);
        }
    }
    
    /* Cleanup */
    if (cnx != NULL) {
        if (cnx->deadline_context != NULL) {
            free(cnx->deadline_context);
            cnx->deadline_context = NULL;
        }
        picoquic_delete_cnx(cnx);
    }
    
    if (quic != NULL) {
        picoquic_free(quic);
    }
    
    return ret;
}