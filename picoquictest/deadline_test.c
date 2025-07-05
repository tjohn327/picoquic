/*

*/

#include <stdlib.h>
#include <string.h>
#include "picoquic_internal.h"
#include "picoquic_utils.h"
#include "../picoquictest/picoquictest_internal.h"
#ifdef _WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif

/*
 * Test DEADLINE_CONTROL frame formatting
 * Since this is a private frame type, we test formatting only
 */
int deadline_frame_format_test()
{
    int ret = 0;
    uint8_t buffer[64];
    uint8_t* bytes = buffer;
    uint8_t* bytes_max = buffer + sizeof(buffer);
    
    uint64_t stream_id = 0x1234;
    uint64_t deadline_ms = 5000;
    
    /* Format the frame */
    bytes = picoquic_format_deadline_control_frame(bytes, bytes_max, stream_id, deadline_ms);
    
    if (bytes == NULL) {
        DBG_PRINTF("%s", "Failed to format DEADLINE_CONTROL frame");
        ret = -1;
    } else {
        size_t frame_length = bytes - buffer;
        
        /* Verify frame header */
        if (buffer[0] != (uint8_t)picoquic_frame_type_deadline_control) {
            DBG_PRINTF("Incorrect frame type: %02x, expected %02x", 
                buffer[0], (uint8_t)picoquic_frame_type_deadline_control);
            ret = -1;
        } else {
            /* Manually verify the encoding */
            const uint8_t* decode_bytes = buffer + 1; /* Skip frame type */
            uint64_t decoded_stream_id = 0;
            uint64_t decoded_deadline = 0;
            
            /* Decode stream ID */
            decode_bytes = picoquic_frames_varint_decode(decode_bytes, bytes, &decoded_stream_id);
            if (decode_bytes == NULL) {
                DBG_PRINTF("%s", "Failed to decode stream ID");
                ret = -1;
            } else if (decoded_stream_id != stream_id) {
                DBG_PRINTF("Stream ID mismatch: %llu != %llu",
                    (unsigned long long)decoded_stream_id,
                    (unsigned long long)stream_id);
                ret = -1;
            } else {
                /* Decode deadline */
                decode_bytes = picoquic_frames_varint_decode(decode_bytes, bytes, &decoded_deadline);
                if (decode_bytes == NULL) {
                    DBG_PRINTF("%s", "Failed to decode deadline");
                    ret = -1;
                } else if (decoded_deadline != deadline_ms) {
                    DBG_PRINTF("Deadline mismatch: %llu != %llu",
                        (unsigned long long)decoded_deadline,
                        (unsigned long long)deadline_ms);
                    ret = -1;
                } else if (decode_bytes != bytes) {
                    DBG_PRINTF("Decode didn't consume all bytes: %zu remaining",
                        (size_t)(bytes - decode_bytes));
                    ret = -1;
                } else {
                    DBG_PRINTF("Successfully formatted DEADLINE_CONTROL frame (%zu bytes)", frame_length);
                }
            }
        }
    }
    
    return ret;
}

/*
 * Test stream deadline API
 */
int deadline_stream_api_test()
{
    int ret = 0;
    uint64_t current_time = 1000000; /* 1 second */
    
    /* Create a minimal connection context for testing */
    picoquic_quic_t* quic = picoquic_create(8, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, current_time, &current_time, NULL, NULL, 0);
    if (quic == NULL) {
        return -1;
    }
    
    picoquic_cnx_t* cnx = picoquic_create_cnx(quic, 
        picoquic_null_connection_id, picoquic_null_connection_id,
        NULL, current_time, 0, NULL, NULL, 1);
    if (cnx == NULL) {
        picoquic_free(quic);
        return -1;
    }
    
    /* Set connection state to ready */
    cnx->cnx_state = picoquic_state_ready;
    
    /* Test setting a soft deadline */
    uint64_t stream_id = 4;
    uint64_t deadline_ms = 3000;
    
    ret = picoquic_set_stream_deadline(cnx, stream_id, deadline_ms, 0);
    
    if (ret != 0) {
        DBG_PRINTF("Failed to set stream deadline, ret=%d", ret);
    } else {
        /* Verify the deadline was set */
        picoquic_stream_head_t* stream = picoquic_find_stream(cnx, stream_id);
        if (stream == NULL) {
            DBG_PRINTF("Stream %llu not found", (unsigned long long)stream_id);
            ret = -1;
        } else if (stream->deadline_ctx == NULL) {
            DBG_PRINTF("%s", "Deadline context not initialized");
            ret = -1;
        } else {
            if (stream->deadline_ctx->deadline_ms != deadline_ms) {
                DBG_PRINTF("Deadline mismatch: %llu != %llu",
                    (unsigned long long)stream->deadline_ctx->deadline_ms,
                    (unsigned long long)deadline_ms);
                ret = -1;
            }
            if (stream->deadline_ctx->deadline_type != 0) {
                DBG_PRINTF("Expected soft deadline (0), got %d", 
                    stream->deadline_ctx->deadline_type);
                ret = -1;
            }
            if (stream->deadline_ctx->deadline_enabled != 1) {
                DBG_PRINTF("%s", "Deadline not enabled");
                ret = -1;
            }
            
            /* Verify absolute deadline calculation */
            uint64_t expected_absolute = current_time + (deadline_ms * 1000);
            if (stream->deadline_ctx->absolute_deadline != expected_absolute) {
                DBG_PRINTF("Absolute deadline mismatch: %llu != %llu",
                    (unsigned long long)stream->deadline_ctx->absolute_deadline,
                    (unsigned long long)expected_absolute);
                ret = -1;
            }
        }
    }
    
    /* Test setting a hard deadline on another stream */
    if (ret == 0) {
        stream_id = 8;
        deadline_ms = 1500;
        
        ret = picoquic_set_stream_deadline(cnx, stream_id, deadline_ms, 1);
        
        if (ret != 0) {
            DBG_PRINTF("Failed to set hard deadline, ret=%d", ret);
        } else {
            picoquic_stream_head_t* stream = picoquic_find_stream(cnx, stream_id);
            if (stream == NULL) {
                DBG_PRINTF("Stream %llu not found", (unsigned long long)stream_id);
                ret = -1;
            } else if (stream->deadline_ctx == NULL) {
                DBG_PRINTF("%s", "Deadline context not initialized for hard deadline");
                ret = -1;
            } else {
                if (stream->deadline_ctx->deadline_type != 1) {
                    DBG_PRINTF("Expected hard deadline (1), got %d", 
                        stream->deadline_ctx->deadline_type);
                    ret = -1;
                }
            }
        }
    }
    
    /* Test that a DEADLINE_CONTROL frame is queued when deadline is set */
    if (ret == 0) {
        /* Check that misc frames were queued (2 frames for 2 streams) */
        int frame_count = 0;
        picoquic_misc_frame_header_t* misc_frame = cnx->first_misc_frame;
        while (misc_frame != NULL) {
            frame_count++;
            misc_frame = misc_frame->next_misc_frame;
        }
        
        if (frame_count != 2) {
            DBG_PRINTF("Expected 2 DEADLINE_CONTROL frames queued, found %d", frame_count);
            ret = -1;
        }
    }
    
    /* Cleanup */
    picoquic_delete_cnx(cnx);
    picoquic_free(quic);
    
    return ret;
}

/*
 * Test transport parameter encoding/decoding
 */
int deadline_transport_param_test()
{
    int ret = 0;
    uint8_t buffer[256];
    size_t length = 0;
    uint64_t current_time = 0;
    
    /* Create test contexts */
    picoquic_quic_t* quic = picoquic_create(8, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, current_time, &current_time, NULL, NULL, 0);
    if (quic == NULL) {
        return -1;
    }
    
    picoquic_cnx_t* cnx = picoquic_create_cnx(quic, 
        picoquic_null_connection_id, picoquic_null_connection_id,
        NULL, 0, 0, NULL, NULL, 1);
    if (cnx == NULL) {
        picoquic_free(quic);
        return -1;
    }
    
    /* Set deadline-aware streams enabled */
    cnx->local_parameters.enable_deadline_aware_streams = 1;
    
    /* Test that our parameter is included when encoding */
    uint8_t* bytes = buffer;
    uint8_t* bytes_max = buffer + sizeof(buffer);
    
    /* Encode using the actual transport parameter encoding logic */
    picoquic_tp_enum tp_type = (picoquic_tp_enum)picoquic_tp_enable_deadline_aware_streams;
    bytes = picoquic_transport_param_type_flag_encode(bytes, bytes_max, tp_type);
    
    if (bytes == NULL || bytes <= buffer) {
        DBG_PRINTF("%s", "Failed to encode deadline transport parameter");
        ret = -1;
    } else {
        length = bytes - buffer;
        
        /* Verify encoding - should be varint(param_id) + varint(0) */
        const uint8_t* decode_bytes = buffer;
        uint64_t param_id = 0;
        uint64_t param_length = 0;
        
        decode_bytes = picoquic_frames_varint_decode(decode_bytes, bytes, &param_id);
        if (decode_bytes == NULL) {
            DBG_PRINTF("%s", "Failed to decode parameter ID");
            ret = -1;
        } else if (param_id != picoquic_tp_enable_deadline_aware_streams) {
            DBG_PRINTF("Parameter ID mismatch: %llx != %llx",
                (unsigned long long)param_id,
                (unsigned long long)picoquic_tp_enable_deadline_aware_streams);
            ret = -1;
        } else {
            decode_bytes = picoquic_frames_varint_decode(decode_bytes, bytes, &param_length);
            if (decode_bytes == NULL) {
                DBG_PRINTF("%s", "Failed to decode parameter length");
                ret = -1;
            } else if (param_length != 0) {
                DBG_PRINTF("Expected length 0 for flag, got %llu",
                    (unsigned long long)param_length);
                ret = -1;
            } else {
                DBG_PRINTF("Successfully encoded deadline transport parameter (%zu bytes)", length);
            }
        }
    }
    
    /* Cleanup */
    picoquic_delete_cnx(cnx);
    picoquic_free(quic);
    
    return ret;
}

/*
 * Integration test - simple deadline parameter test
 * Tests that deadline API works and frames can be queued
 */
int deadline_integration_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    uint64_t stream_id = 4;
    
    /* Create a minimal QUIC context */
    picoquic_quic_t* quic = picoquic_create(8, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, simulated_time, &simulated_time, NULL, NULL, 0);
    if (quic == NULL) {
        return -1;
    }
    
    /* Create a connection */
    picoquic_cnx_t* cnx = picoquic_create_cnx(quic, 
        picoquic_null_connection_id, picoquic_null_connection_id,
        NULL, simulated_time, 0, NULL, PICOQUIC_TEST_ALPN, 1);
    if (cnx == NULL) {
        picoquic_free(quic);
        return -1;
    }
    
    /* Enable deadline-aware streams */
    cnx->local_parameters.enable_deadline_aware_streams = 1;
    /* Simulate that we received the parameter from peer */
    cnx->remote_parameters.enable_deadline_aware_streams = 1;
    /* Set connection state to ready to allow frame queuing */
    cnx->cnx_state = picoquic_state_ready;
    
    /* Test 1: Set a deadline on a stream */
    ret = picoquic_set_stream_deadline(cnx, stream_id, 1000, 0);
    if (ret != 0) {
        DBG_PRINTF("Failed to set deadline, ret=%d", ret);
    }
    
    /* Test 2: Verify the deadline was set */
    if (ret == 0) {
        picoquic_stream_head_t* stream = picoquic_find_stream(cnx, stream_id);
        if (stream == NULL) {
            DBG_PRINTF("Stream %llu not found", (unsigned long long)stream_id);
            ret = -1;
        } else if (stream->deadline_ctx == NULL) {
            DBG_PRINTF("%s", "Stream deadline context not initialized");
            ret = -1;
        } else if (stream->deadline_ctx->deadline_ms != 1000) {
            DBG_PRINTF("Deadline mismatch: %llu != 1000",
                (unsigned long long)stream->deadline_ctx->deadline_ms);
            ret = -1;
        }
    }
    
    /* Test 3: Verify DEADLINE_CONTROL frame was queued */
    if (ret == 0) {
        if (cnx->first_misc_frame == NULL) {
            DBG_PRINTF("%s", "No DEADLINE_CONTROL frame queued");
            ret = -1;
        } else {
            /* Get the frame data (which follows the header) */
            uint8_t* frame_data = ((uint8_t*)cnx->first_misc_frame) + sizeof(picoquic_misc_frame_header_t);
            uint8_t frame_type = frame_data[0];
            if (frame_type != (uint8_t)picoquic_frame_type_deadline_control) {
                DBG_PRINTF("Wrong frame type queued: %02x", frame_type);
                ret = -1;
            }
        }
    }
    
    /* Test 4: Verify we can parse our own frame */
    if (ret == 0 && cnx->first_misc_frame != NULL) {
        /* Create a test connection to parse into */
        picoquic_cnx_t* cnx2 = picoquic_create_cnx(quic, 
            picoquic_null_connection_id, picoquic_null_connection_id,
            NULL, simulated_time, 0, NULL, PICOQUIC_TEST_ALPN, 0);
        if (cnx2 != NULL) {
            cnx2->cnx_state = picoquic_state_ready;
            cnx2->remote_parameters.enable_deadline_aware_streams = 1;
            
            /* Get the frame data and parse it */
            uint8_t* frame_data = ((uint8_t*)cnx->first_misc_frame) + sizeof(picoquic_misc_frame_header_t);
            const uint8_t* bytes = frame_data;
            const uint8_t* bytes_max = bytes + cnx->first_misc_frame->length;
            
            bytes = picoquic_parse_deadline_control_frame(cnx2, bytes, bytes_max, simulated_time, 3);
            if (bytes == NULL) {
                DBG_PRINTF("%s", "Failed to parse DEADLINE_CONTROL frame");
                ret = -1;
            } else {
                /* Verify the deadline was received */
                picoquic_stream_head_t* stream2 = picoquic_find_stream(cnx2, stream_id);
                if (stream2 == NULL || stream2->deadline_ctx == NULL) {
                    DBG_PRINTF("%s", "Parsed frame didn't create deadline context");
                    ret = -1;
                } else if (stream2->deadline_ctx->deadline_ms != 1000) {
                    DBG_PRINTF("Parsed deadline mismatch: %llu != 1000",
                        (unsigned long long)stream2->deadline_ctx->deadline_ms);
                    ret = -1;
                }
            }
            
            picoquic_delete_cnx(cnx2);
        }
    }
    
    /* Clean up */
    picoquic_delete_cnx(cnx);
    picoquic_free(quic);
    
    return ret;
}

/*
 * Main test runner for deadline-aware streams
 */
int deadline_test()
{
    int ret = 0;
    
    /* Test transport parameter encoding */
    if (ret == 0) {
        ret = deadline_transport_param_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "Transport parameter test failed");
        }
    }
    
    /* Test frame formatting */
    if (ret == 0) {
        ret = deadline_frame_format_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "Frame format test failed");
        }
    }
    
    /* Test stream API */
    if (ret == 0) {
        ret = deadline_stream_api_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "Stream API test failed");
        }
    }
    
    /* Run integration test */
    if (ret == 0) {
        ret = deadline_integration_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "Integration test failed");
        }
    }
    
    DBG_PRINTF("%s", "Deadline test completed");
    
    return ret;
}