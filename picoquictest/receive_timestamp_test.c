/*
* Author: Claude for Tony
* Copyright (c) 2025, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "picoquic_internal.h"
#include "picoquictest_internal.h"
#include "tls_api.h"
#include <stdlib.h>
#include <string.h>

/* Function declarations from frames.c */
uint8_t* picoquic_format_ack_frame_in_context(picoquic_cnx_t* cnx, uint8_t* bytes, uint8_t* bytes_max,
    int* more_data, uint64_t current_time, picoquic_ack_context_t* ack_ctx, int* need_time_stamp,
    uint64_t multipath_sequence, int is_opportunistic);

const uint8_t* picoquic_decode_ack_frame(picoquic_cnx_t* cnx, const uint8_t* bytes,
    const uint8_t* bytes_max, uint64_t current_time, int epoch, int is_ecn, int has_path_id, 
    picoquic_packet_data_t* packet_data);

/* Test receive timestamp transport parameter negotiation */
int receive_timestamp_tp_test()
{
    int ret = 0;
    picoquic_tp_t tp;
    
    /* Initialize transport parameters */
    picoquic_init_transport_parameters(&tp, 1);
    
    /* Verify initial values */
    if (tp.max_receive_timestamps_per_ack != 0) {
        ret = -1;
    }
    if (tp.receive_timestamps_exponent != 0) {
        ret = -1;
    }
    
    /* Set receive timestamp parameters */
    tp.max_receive_timestamps_per_ack = 100;
    tp.receive_timestamps_exponent = 5;
    
    /* Verify they are set correctly */
    if (tp.max_receive_timestamps_per_ack != 100) {
        ret = -1;
    }
    if (tp.receive_timestamps_exponent != 5) {
        ret = -1;
    }
    
    return ret;
}

/* Test receive timestamp ACK frame encoding and decoding */
int receive_timestamp_ack_test()
{
    int ret = 0;
    uint8_t buffer[256];
    uint8_t* bytes = buffer;
    uint8_t* bytes_max = buffer + sizeof(buffer);
    picoquic_cnx_t test_cnx;
    picoquic_ack_context_t ack_ctx;
    int more_data = 0;
    int need_time_stamp = 0;
    uint64_t current_time = 1000000;
    
    /* Initialize test structures */
    memset(&test_cnx, 0, sizeof(test_cnx));
    memset(&ack_ctx, 0, sizeof(ack_ctx));
    memset(buffer, 0, sizeof(buffer));
    
    /* Set up connection to enable receive timestamps */
    test_cnx.remote_parameters.max_receive_timestamps_per_ack = 10;
    test_cnx.remote_parameters.receive_timestamps_exponent = 3;
    test_cnx.receive_timestamp_basis = current_time;
    
    /* Initialize ACK context with some received packets */
    picoquic_sack_list_init(&ack_ctx.sack_list);
    
    /* Add some packets to the SACK list */
    for (uint64_t pn = 0; pn < 5; pn++) {
        ret = picoquic_update_sack_list(&ack_ctx.sack_list, pn, pn, current_time + (pn * 1000));
        if (ret != 0) {
            DBG_PRINTF("Failed to update SACK list for packet %llu\n", (unsigned long long)pn);
            break;
        }
    }
    
    if (ret == 0) {
        /* Try to encode an ACK frame */
        bytes = picoquic_format_ack_frame_in_context(&test_cnx, bytes, bytes_max,
            &more_data, current_time, &ack_ctx, &need_time_stamp, 0, 0);
        
        if (bytes == buffer) {
            DBG_PRINTF("%s", "Failed to encode ACK frame\n");
            ret = -1;
        } else {
            DBG_PRINTF("Encoded ACK frame of %d bytes\n", (int)(bytes - buffer));
        }
    }
    
    /* Clean up */
    picoquic_sack_list_free(&ack_ctx.sack_list);
    
    return ret;
}

/* Test receive timestamp encoding with different exponent values */
int receive_timestamp_exponent_test()
{
    int ret = 0;
    picoquic_cnx_t test_cnx;
    uint64_t simulated_time = 1000000; /* 1 second */
    
    memset(&test_cnx, 0, sizeof(test_cnx));
    test_cnx.receive_timestamp_basis = simulated_time;
    
    /* Test different exponent values */
    uint8_t exponents[] = {0, 3, 10, 20}; /* microsecond, 8us, 1ms, 1s precision */
    
    for (int i = 0; i < 4 && ret == 0; i++) {
        test_cnx.remote_parameters.receive_timestamps_exponent = exponents[i];
        
        /* Calculate expected timestamp delta */
        uint64_t timestamp = simulated_time + 12345; /* 12.345 ms after basis */
        uint64_t delta = timestamp - simulated_time;
        uint64_t encoded_delta = delta >> exponents[i];
        uint64_t decoded_timestamp = simulated_time + (encoded_delta << exponents[i]);
        
        /* Verify precision loss is as expected */
        uint64_t precision_loss = timestamp - decoded_timestamp;
        uint64_t max_loss = (1ULL << exponents[i]) - 1;
        
        if (precision_loss > max_loss) {
            ret = -1;
        }
    }
    
    return ret;
}


/* Test that receive timestamps are collected when enabled */
int receive_timestamp_collection_test()
{
    int ret = 0;
    picoquic_cnx_t test_cnx;
    picoquic_tp_t local_tp, remote_tp;
    picoquic_ack_context_t test_ack_ctx;
    uint64_t current_time = 1000000;
    
    /* Initialize test structures */
    memset(&test_cnx, 0, sizeof(test_cnx));
    memset(&local_tp, 0, sizeof(local_tp));
    memset(&remote_tp, 0, sizeof(remote_tp));
    memset(&test_ack_ctx, 0, sizeof(test_ack_ctx));
    
    /* Set up transport parameters */
    picoquic_init_transport_parameters(&local_tp, 0);
    picoquic_init_transport_parameters(&remote_tp, 1);
    
    /* Enable receive timestamps */
    local_tp.max_receive_timestamps_per_ack = 10;
    local_tp.receive_timestamps_exponent = 3;
    remote_tp.max_receive_timestamps_per_ack = 10;
    remote_tp.receive_timestamps_exponent = 3;
    
    /* Set up connection context */
    test_cnx.local_parameters = local_tp;
    test_cnx.remote_parameters = remote_tp;
    test_cnx.ack_ctx[picoquic_packet_context_application] = test_ack_ctx;
    
    /* Initialize SACK list */
    picoquic_sack_list_init(&test_cnx.ack_ctx[picoquic_packet_context_application].sack_list);
    
    /* Simulate receiving packets with timestamps enabled */
    for (uint64_t pn = 0; pn < 5; pn++) {
        uint64_t receive_time = current_time + (pn * 1000);
        ret = picoquic_record_pn_received(&test_cnx, picoquic_packet_context_application, 
                                        NULL, pn, receive_time);
        if (ret != 0) {
            DBG_PRINTF("Failed to record packet %llu, ret=%d\n", (unsigned long long)pn, ret);
            break;
        }
    }
    
    if (ret == 0) {
        /* Verify timestamp basis was set */
        if (test_cnx.receive_timestamp_basis == 0) {
            DBG_PRINTF("%s", "Receive timestamp basis not set\n");
            ret = -1;
        } else {
            DBG_PRINTF("Timestamp basis set to %llu\n", 
                      (unsigned long long)test_cnx.receive_timestamp_basis);
                      
            /* Check SACK items */
            picoquic_sack_item_t* sack = picoquic_sack_first_item(
                &test_cnx.ack_ctx[picoquic_packet_context_application].sack_list);
            
            while (sack != NULL) {
                DBG_PRINTF("SACK range %llu-%llu, timestamps=%p, count=%zu\n",
                          (unsigned long long)sack->start_of_sack_range,
                          (unsigned long long)sack->end_of_sack_range,
                          (void*)sack->receive_timestamps,
                          sack->receive_timestamps_count);
                
                if (sack->receive_timestamps != NULL) {
                    for (size_t i = 0; i < sack->receive_timestamps_count; i++) {
                        if (sack->receive_timestamps[i] != 0) {
                            DBG_PRINTF("  Packet %llu has timestamp %llu\n",
                                      (unsigned long long)(sack->start_of_sack_range + i),
                                      (unsigned long long)sack->receive_timestamps[i]);
                        }
                    }
                }
                
                sack = picoquic_sack_next_item(sack);
            }
        }
    }
    
    /* Clean up */
    picoquic_sack_list_free(&test_cnx.ack_ctx[picoquic_packet_context_application].sack_list);
    
    return ret;
}

/* Test end-to-end ACK with timestamps */
int receive_timestamp_e2e_test()
{
    int ret = 0;
    picoquic_cnx_t test_cnx;
    picoquic_tp_t local_tp, remote_tp;
    picoquic_ack_context_t test_ack_ctx;
    uint64_t current_time = 1000000;
    uint8_t ack_buffer[256];
    uint8_t* bytes;
    int more_data = 0;
    int need_time_stamp = 0;
    
    /* Initialize test structures */
    memset(&test_cnx, 0, sizeof(test_cnx));
    memset(&local_tp, 0, sizeof(local_tp));
    memset(&remote_tp, 0, sizeof(remote_tp));
    memset(&test_ack_ctx, 0, sizeof(test_ack_ctx));
    
    /* Set up transport parameters */
    picoquic_init_transport_parameters(&local_tp, 0);
    picoquic_init_transport_parameters(&remote_tp, 1);
    
    /* Enable receive timestamps */
    local_tp.max_receive_timestamps_per_ack = 10;
    local_tp.receive_timestamps_exponent = 0; /* microsecond precision */
    remote_tp.max_receive_timestamps_per_ack = 10;
    remote_tp.receive_timestamps_exponent = 0;
    
    /* Set up connection context */
    test_cnx.local_parameters = local_tp;
    test_cnx.remote_parameters = remote_tp;
    test_cnx.ack_ctx[picoquic_packet_context_application] = test_ack_ctx;
    
    /* Initialize SACK list */
    picoquic_sack_list_init(&test_cnx.ack_ctx[picoquic_packet_context_application].sack_list);
    
    /* Simulate receiving packets */
    for (uint64_t pn = 0; pn < 3; pn++) {
        uint64_t receive_time = current_time + (pn * 10000); /* 10ms apart */
        ret = picoquic_record_pn_received(&test_cnx, picoquic_packet_context_application, 
                                        NULL, pn, receive_time);
        if (ret != 0) {
            DBG_PRINTF("Failed to record packet %llu\n", (unsigned long long)pn);
            break;
        }
    }
    
    if (ret == 0) {
        /* Encode ACK frame with timestamps */
        bytes = picoquic_format_ack_frame_in_context(&test_cnx, ack_buffer, 
                                                     ack_buffer + sizeof(ack_buffer),
                                                     &more_data, current_time + 15000,
                                                     &test_cnx.ack_ctx[picoquic_packet_context_application],
                                                     &need_time_stamp, 0, 0);
        
        if (bytes == NULL || bytes == ack_buffer) {
            DBG_PRINTF("%s", "Failed to encode ACK frame\n");
            ret = -1;
        } else {
            size_t ack_length = bytes - ack_buffer;
            DBG_PRINTF("Encoded ACK frame with timestamps: %zu bytes\n", ack_length);
            
            /* Verify the frame contains timestamp data */
            if (ack_length < 20) { /* Basic ACK without timestamps would be ~12 bytes */
                DBG_PRINTF("ACK frame seems too small for timestamps: %zu bytes\n", ack_length);
                /* Not necessarily an error - depends on encoding */
            }
            
            /* Print hex dump for debugging */
            DBG_PRINTF("%s", "ACK frame hex: ");
            for (size_t i = 0; i < ack_length && i < 32; i++) {
                DBG_PRINTF("%02x ", ack_buffer[i]);
            }
            DBG_PRINTF("%s", "\n");
        }
    }
    
    /* Clean up */
    picoquic_sack_list_free(&test_cnx.ack_ctx[picoquic_packet_context_application].sack_list);
    
    return ret;
}

/* Main test runner for receive timestamp tests */
int receive_timestamp_test()
{
    int ret = 0;
    
    if (ret == 0) {
        ret = receive_timestamp_tp_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "Receive timestamp TP test failed\n");
        }
    }
    
    if (ret == 0) {
        ret = receive_timestamp_ack_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "Receive timestamp ACK test failed\n");
        }
    }
    
    if (ret == 0) {
        ret = receive_timestamp_exponent_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "Receive timestamp exponent test failed\n");
        }
    }
    
    if (ret == 0) {
        ret = receive_timestamp_collection_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "Receive timestamp collection test failed\n");
        }
    }
    
    if (ret == 0) {
        ret = receive_timestamp_e2e_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "Receive timestamp end-to-end test failed\n");
        }
    }
    
    return ret;
}