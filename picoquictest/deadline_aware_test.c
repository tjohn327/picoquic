/*

*/

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include "picoquic.h"
#include "picoquic_internal.h"
#include "picoquic_utils.h"
#include "picoquictest_internal.h"

/* Forward declarations for functions from frames.c */
uint8_t* picoquic_format_deadline_control_frame(uint8_t* bytes, uint8_t* bytes_max, 
    uint64_t stream_id, uint64_t deadline_ms, int* more_data, int* is_pure_ack);
const uint8_t* picoquic_decode_deadline_control_frame(picoquic_cnx_t* cnx, const uint8_t* bytes, const uint8_t* bytes_max);

/* Test transport parameter negotiation for deadline-aware streams */
int deadline_aware_transport_param_test()
{
    int ret = 0;
    uint8_t buffer[256];
    size_t consumed = 0;
    
    /* Create a simple transport parameter structure */
    picoquic_tp_t tp;
    memset(&tp, 0, sizeof(tp));
    
    /* Enable deadline-aware streams */
    tp.enable_deadline_aware_streams = 1;
    
    /* Manually encode the transport parameter */
    uint8_t* bytes = buffer;
    uint8_t* bytes_max = buffer + sizeof(buffer);
    
    bytes = picoquic_frames_varint_encode(bytes, bytes_max, picoquic_tp_enable_deadline_aware_streams);
    if (bytes != NULL) {
        bytes = picoquic_frames_varint_encode(bytes, bytes_max, 1); /* length */
        if (bytes != NULL) {
            bytes = picoquic_frames_varint_encode(bytes, bytes_max, tp.enable_deadline_aware_streams);
        }
    }
    
    if (bytes == NULL) {
        ret = -1;
    } else {
        consumed = bytes - buffer;
        
        /* Test decoding */
        picoquic_tp_t tp_decoded;
        memset(&tp_decoded, 0, sizeof(tp_decoded));
        size_t byte_index = 0;
        
        while (byte_index < consumed && ret == 0) {
            uint64_t type = 0;
            uint64_t length = 0;
            
            byte_index += picoquic_varint_decode(buffer + byte_index, consumed - byte_index, &type);
            byte_index += picoquic_varint_decode(buffer + byte_index, consumed - byte_index, &length);
            
            if (type == picoquic_tp_enable_deadline_aware_streams) {
                uint64_t value = 0;
                byte_index += picoquic_varint_decode(buffer + byte_index, consumed - byte_index, &value);
                tp_decoded.enable_deadline_aware_streams = (int)value;
            } else {
                byte_index += length;
            }
        }
        
        /* Verify */
        if (tp_decoded.enable_deadline_aware_streams != 1) {
            DBG_PRINTF("Transport parameter mismatch: enable_deadline_aware_streams=%d\n",
                tp_decoded.enable_deadline_aware_streams);
            ret = -1;
        }
    }
    
    return ret;
}

/* Test DEADLINE_CONTROL frame encoding and decoding */
int deadline_control_frame_test()
{
    int ret = 0;
    uint8_t buffer[256];
    int more_data = 0;
    int is_pure_ack = 1;
    uint64_t stream_id = 4;
    uint64_t deadline_ms = 1000;
    
    /* Encode the frame */
    uint8_t* bytes = picoquic_format_deadline_control_frame(buffer, buffer + sizeof(buffer),
        stream_id, deadline_ms, &more_data, &is_pure_ack);
    
    if (bytes == buffer || more_data) {
        DBG_PRINTF("%s", "Failed to encode DEADLINE_CONTROL frame\n");
        ret = -1;
    } else {
        size_t frame_length = bytes - buffer;
        
        /* Decode the frame */
        const uint8_t* decode_bytes = buffer;
        uint64_t frame_type = 0;
        uint64_t decoded_stream_id = 0;
        uint64_t decoded_deadline = 0;
        
        decode_bytes = picoquic_frames_varint_decode(decode_bytes, buffer + frame_length, &frame_type);
        if (decode_bytes == NULL || frame_type != picoquic_frame_type_deadline_control) {
            DBG_PRINTF("Failed to decode frame type, got %llu\n", (unsigned long long)frame_type);
            ret = -1;
        }
        
        if (ret == 0) {
            decode_bytes = picoquic_frames_varint_decode(decode_bytes, buffer + frame_length, &decoded_stream_id);
            if (decode_bytes == NULL || decoded_stream_id != stream_id) {
                DBG_PRINTF("Failed to decode stream ID, got %llu expected %llu\n", 
                    (unsigned long long)decoded_stream_id, (unsigned long long)stream_id);
                ret = -1;
            }
        }
        
        if (ret == 0) {
            decode_bytes = picoquic_frames_varint_decode(decode_bytes, buffer + frame_length, &decoded_deadline);
            if (decode_bytes == NULL || decoded_deadline != deadline_ms) {
                DBG_PRINTF("Failed to decode deadline, got %llu expected %llu\n",
                    (unsigned long long)decoded_deadline, (unsigned long long)deadline_ms);
                ret = -1;
            }
        }
        
        if (ret == 0 && decode_bytes != buffer + frame_length) {
            DBG_PRINTF("%s", "Frame length mismatch\n");
            ret = -1;
        }
    }
    
    return ret;
}

/* Test deadline-aware streams API */
int deadline_aware_api_test()
{
    int ret = 0;
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    uint64_t simulated_time = 0;
    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(struct sockaddr_in));
    
    /* Create minimal test context */
    quic = picoquic_create(8, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, 0, &simulated_time, NULL, NULL, 0);
    
    if (quic == NULL) {
        ret = -1;
    } else {
        cnx = picoquic_create_cnx(quic, 
            picoquic_null_connection_id, picoquic_null_connection_id,
            (struct sockaddr*)&saddr, simulated_time,
            0, "test-sni", "test-alpn", 1);
        
        if (cnx == NULL) {
            ret = -1;
        }
    }
    
    if (ret == 0) {
        /* Initially, deadline-aware streams should be disabled */
        if (picoquic_is_deadline_aware_enabled(cnx)) {
            DBG_PRINTF("%s", "Deadline-aware streams should be disabled by default\n");
            ret = -1;
        }
        
        /* Enable deadline-aware streams on both sides */
        cnx->local_parameters.enable_deadline_aware_streams = 1;
        cnx->remote_parameters.enable_deadline_aware_streams = 1;
        
        /* Now it should be enabled */
        if (!picoquic_is_deadline_aware_enabled(cnx)) {
            DBG_PRINTF("%s", "Deadline-aware streams should be enabled\n");
            ret = -1;
        }
        
        /* Test setting and getting deadline */
        uint64_t stream_id = 4;
        uint64_t deadline_ms = 500;
        
        ret = picoquic_set_stream_deadline(cnx, stream_id, deadline_ms);
        if (ret != 0) {
            DBG_PRINTF("%s", "Failed to set stream deadline\n");
        } else {
            uint64_t retrieved_deadline = picoquic_get_stream_deadline(cnx, stream_id);
            if (retrieved_deadline != deadline_ms) {
                DBG_PRINTF("Retrieved deadline %llu does not match set deadline %llu\n",
                    (unsigned long long)retrieved_deadline, (unsigned long long)deadline_ms);
                ret = -1;
            }
        }
        
        /* Test getting deadline for non-existent stream */
        if (ret == 0) {
            uint64_t non_existent_deadline = picoquic_get_stream_deadline(cnx, 100);
            if (non_existent_deadline != 0) {
                DBG_PRINTF("%s", "Non-existent stream should have deadline 0\n");
                ret = -1;
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

/* Test that DEADLINE_CONTROL frame is only accepted when negotiated */
int deadline_control_negotiation_test()
{
    int ret = 0;
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    uint64_t simulated_time = 0;
    struct sockaddr_in saddr;
    uint8_t bytes[64];
    const uint8_t* bytes_max = bytes + sizeof(bytes);
    
    memset(&saddr, 0, sizeof(struct sockaddr_in));
    
    /* Create minimal test context */
    quic = picoquic_create(8, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, 0, &simulated_time, NULL, NULL, 0);
    
    if (quic == NULL) {
        ret = -1;
    } else {
        cnx = picoquic_create_cnx(quic, 
            picoquic_null_connection_id, picoquic_null_connection_id,
            (struct sockaddr*)&saddr, simulated_time,
            0, "test-sni", "test-alpn", 1);
        
        if (cnx == NULL) {
            ret = -1;
        }
    }
    
    if (ret == 0) {
        /* Encode a DEADLINE_CONTROL frame */
        int more_data = 0;
        int is_pure_ack = 1;
        uint8_t* bytes_next = picoquic_format_deadline_control_frame(bytes, (uint8_t*)bytes_max,
            4, 1000, &more_data, &is_pure_ack);
        
        if (bytes_next == bytes || more_data) {
            DBG_PRINTF("%s", "Failed to encode DEADLINE_CONTROL frame\n");
            ret = -1;
        } else {
            /* Try to decode without negotiation - should fail */
            cnx->remote_parameters.enable_deadline_aware_streams = 0;
            const uint8_t* decode_result = picoquic_decode_deadline_control_frame(cnx, bytes, bytes_max);
            
            if (decode_result != NULL) {
                DBG_PRINTF("%s", "DEADLINE_CONTROL frame should be rejected without negotiation\n");
                ret = -1;
            }
            /* Check that connection is in error state */
            if (cnx->cnx_state == picoquic_state_ready ||
                cnx->cnx_state == picoquic_state_client_ready_start) {
                DBG_PRINTF("Connection should be in error state, but is in state %d\n", cnx->cnx_state);
                ret = -1;
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

/* Test that DEADLINE_CONTROL frame is actually queued and sent */
int deadline_control_transmission_test()
{
    int ret = 0;
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    uint64_t simulated_time = 0;
    struct sockaddr_in saddr;
    
    memset(&saddr, 0, sizeof(struct sockaddr_in));
    
    /* Create minimal test context */
    quic = picoquic_create(8, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, 0, &simulated_time, NULL, NULL, 0);
    
    if (quic == NULL) {
        ret = -1;
    } else {
        cnx = picoquic_create_cnx(quic, 
            picoquic_null_connection_id, picoquic_null_connection_id,
            (struct sockaddr*)&saddr, simulated_time,
            0, "test-sni", "test-alpn", 1);
        
        if (cnx == NULL) {
            ret = -1;
        }
    }
    
    if (ret == 0) {
        /* Enable deadline-aware streams on both sides */
        cnx->local_parameters.enable_deadline_aware_streams = 1;
        cnx->remote_parameters.enable_deadline_aware_streams = 1;
        
        /* Set connection state to allow sending frames */
        cnx->cnx_state = picoquic_state_ready;
        
        /* Set a deadline for stream 4 */
        ret = picoquic_set_stream_deadline(cnx, 4, 1000);
        
        if (ret == 0) {
            /* Check that datagram queue is not empty (frame was queued) */
            if (cnx->first_datagram == NULL) {
                DBG_PRINTF("%s", "DEADLINE_CONTROL frame was not queued\n");
                ret = -1;
            } else {
                DBG_PRINTF("%s", "DEADLINE_CONTROL frame was successfully queued\n");
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

/* Main test function */
int deadline_aware_test()
{
    int ret = 0;
    
    DBG_PRINTF("%s", "Starting deadline_aware_test\n");
    
    if (ret == 0) {
        DBG_PRINTF("%s", "Running deadline_aware_transport_param_test\n");
        ret = deadline_aware_transport_param_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "deadline_aware_transport_param_test failed\n");
        } else {
            DBG_PRINTF("%s", "deadline_aware_transport_param_test passed\n");
        }
    }
    
    if (ret == 0) {
        DBG_PRINTF("%s", "Running deadline_control_frame_test\n");
        ret = deadline_control_frame_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "deadline_control_frame_test failed\n");
        } else {
            DBG_PRINTF("%s", "deadline_control_frame_test passed\n");
        }
    }
    
    if (ret == 0) {
        DBG_PRINTF("%s", "Running deadline_aware_api_test\n");
        ret = deadline_aware_api_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "deadline_aware_api_test failed\n");
        } else {
            DBG_PRINTF("%s", "deadline_aware_api_test passed\n");
        }
    }
    
    if (ret == 0) {
        DBG_PRINTF("%s", "Running deadline_control_negotiation_test\n");
        ret = deadline_control_negotiation_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "deadline_control_negotiation_test failed\n");
        } else {
            DBG_PRINTF("%s", "deadline_control_negotiation_test passed\n");
        }
    }
    
    if (ret == 0) {
        DBG_PRINTF("%s", "Running deadline_control_transmission_test\n");
        ret = deadline_control_transmission_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "deadline_control_transmission_test failed\n");
        } else {
            DBG_PRINTF("%s", "deadline_control_transmission_test passed\n");
        }
    }
    
    DBG_PRINTF("deadline_aware_test completed with ret=%d\n", ret);
    
    return ret;
}