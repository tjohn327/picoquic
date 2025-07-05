/*
* Test of deadline-aware streams partial reliability feature
*/

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "../picoquic/picoquic_internal.h"
#include "picoquictest.h"

/* Forward declarations for internal functions */
int picoquic_queue_network_input(picoquic_quic_t * quic, picosplay_tree_t* tree, uint64_t consumed_offset,
    uint64_t frame_data_offset, const uint8_t* bytes, size_t length, int is_last_frame, 
    picoquic_stream_data_node_t* received_data, int* new_data_available);
void picoquic_stream_data_callback(picoquic_cnx_t* cnx, picoquic_stream_head_t* stream);

/* Test address */
static struct sockaddr_in test_addr = {
    .sin_family = AF_INET,
    .sin_port = 0x1234,
    .sin_addr = { 0 }
};

typedef struct st_test_partial_reliability_ctx_t {
    int nb_gaps_received;
    uint64_t gap_offsets[10];
    uint64_t gap_lengths[10];
    int nb_data_received;
    uint64_t data_offsets[10];
    size_t data_lengths[10];
} test_partial_reliability_ctx_t;

/* Test callback that tracks gaps and data */
static int test_partial_reliability_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t event, void* callback_ctx, void* stream_ctx)
{
    test_partial_reliability_ctx_t* ctx = (test_partial_reliability_ctx_t*)callback_ctx;
    
    switch (event) {
    case picoquic_callback_stream_gap:
        if (ctx->nb_gaps_received < 10) {
            ctx->gap_offsets[ctx->nb_gaps_received] = stream_id;
            /* Gap length is passed as a uint64_t */
            if (bytes != NULL && length == sizeof(uint64_t)) {
                ctx->gap_lengths[ctx->nb_gaps_received] = *((uint64_t*)bytes);
            } else {
                ctx->gap_lengths[ctx->nb_gaps_received] = length;
            }
            ctx->nb_gaps_received++;
        }
        break;
    case picoquic_callback_stream_data:
        if (ctx->nb_data_received < 10) {
            picoquic_stream_head_t* stream = (picoquic_stream_head_t*)stream_ctx;
            if (stream != NULL) {
                ctx->data_offsets[ctx->nb_data_received] = stream->consumed_offset - length;
            }
            ctx->data_lengths[ctx->nb_data_received] = length;
            ctx->nb_data_received++;
        }
        break;
    default:
        break;
    }
    
    return 0;
}

/* Test 1: Basic partial reliability - sender drops data, receiver gets gap notification */
int deadline_partial_reliability_basic_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    picoquic_quic_t* quic = picoquic_create(8, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, simulated_time, &simulated_time, NULL, NULL, 0);
    
    if (quic == NULL) {
        return -1;
    }
    
    /* Create connection */
    picoquic_cnx_t* cnx = picoquic_create_cnx(quic, 
        picoquic_null_connection_id, picoquic_null_connection_id,
        (struct sockaddr*)&test_addr, simulated_time, 0, "test-sni", "test-alpn", 1);
    
    if (cnx == NULL) {
        ret = -1;
    } else {
        test_partial_reliability_ctx_t test_ctx = { 0 };
        picoquic_set_callback(cnx, test_partial_reliability_callback, &test_ctx);
        
        /* Enable deadline-aware streams */
        cnx->local_parameters.enable_deadline_aware_streams = 1;
        cnx->remote_parameters.enable_deadline_aware_streams = 1;
        picoquic_init_deadline_context(cnx);
        
        /* Create a stream and set a deadline */
        uint64_t stream_id = 0;
        picoquic_stream_head_t* stream = picoquic_create_stream(cnx, stream_id);
        
        if (stream == NULL) {
            ret = -1;
        } else {
            /* Initialize deadline context */
            picoquic_deadline_stream_init(stream);
            if (stream->deadline_ctx == NULL) {
                DBG_PRINTF("%s", "Error: failed to initialize deadline context\n");
                ret = -1;
            }
            
            /* Prepare test data */
            uint8_t data2[1000];
            uint8_t data3[1000];
            memset(data2, 'B', sizeof(data2));
            memset(data3, 'C', sizeof(data3));
            
            /* Test gap handling */
            if (ret == 0) {
                /* Record dropped range 0-1000 (first chunk) */
                picoquic_update_sack_list(&stream->deadline_ctx->receiver_dropped_ranges,
                    0, 1000, simulated_time);
                
                /* Simulate receiving data after the gap (chunks 2 and 3) */
                int new_data_available = 0;
                ret = picoquic_queue_network_input(quic, &stream->stream_data_tree,
                    stream->consumed_offset, 1000, data2, sizeof(data2), 0, NULL,
                    &new_data_available);
                if (new_data_available) {
                    stream->is_output_stream = 1;
                }
                
                if (ret == 0) {
                    new_data_available = 0;
                    ret = picoquic_queue_network_input(quic, &stream->stream_data_tree,
                        stream->consumed_offset, 2000, data3, sizeof(data3), 1, NULL,
                        &new_data_available);
                    if (new_data_available) {
                        stream->is_output_stream = 1;
                    }
                }
                
                /* Call stream data callback - should skip gap and deliver data */
                if (ret == 0) {
                    /* Mark stream as having data to output */
                    stream->is_output_stream = 1;
                    picoquic_stream_data_callback(cnx, stream);
                    
                    /* Verify gap notification was sent */
                    if (test_ctx.nb_gaps_received != 1) {
                        DBG_PRINTF("Error: expected 1 gap, got %d\n", test_ctx.nb_gaps_received);
                        ret = -1;
                    } else if (test_ctx.gap_lengths[0] != 1000) {
                        DBG_PRINTF("Error: expected gap length 1000, got %lu\n", 
                            test_ctx.gap_lengths[0]);
                        ret = -1;
                    }
                    
                    /* Verify data was delivered after gap */
                    if (ret == 0 && test_ctx.nb_data_received != 2) {
                        DBG_PRINTF("Error: expected 2 data callbacks, got %d\n", 
                            test_ctx.nb_data_received);
                        ret = -1;
                    }
                    
                    /* Verify consumed offset advanced past the gap */
                    if (ret == 0 && stream->consumed_offset != 3000) {
                        DBG_PRINTF("Error: expected consumed_offset 3000, got %lu\n",
                            stream->consumed_offset);
                        ret = -1;
                    }
                }
            }
        }
    }
    
    picoquic_free(quic);
    return ret;
}

/* Test 2: Multiple gaps in stream */
int deadline_partial_reliability_multiple_gaps_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    picoquic_quic_t* quic = picoquic_create(8, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, simulated_time, &simulated_time, NULL, NULL, 0);
    
    if (quic == NULL) {
        return -1;
    }
    
    picoquic_cnx_t* cnx = picoquic_create_cnx(quic, 
        picoquic_null_connection_id, picoquic_null_connection_id,
        (struct sockaddr*)&test_addr, simulated_time, 0, "test-sni", "test-alpn", 1);
    
    if (cnx == NULL) {
        ret = -1;
    } else {
        test_partial_reliability_ctx_t test_ctx = { 0 };
        picoquic_set_callback(cnx, test_partial_reliability_callback, &test_ctx);
        
        /* Enable deadline-aware streams */
        cnx->local_parameters.enable_deadline_aware_streams = 1;
        cnx->remote_parameters.enable_deadline_aware_streams = 1;
        picoquic_init_deadline_context(cnx);
        
        uint64_t stream_id = 0;
        picoquic_stream_head_t* stream = picoquic_create_stream(cnx, stream_id);
        
        if (stream != NULL) {
            picoquic_deadline_stream_init(stream);
            
            /* Simulate multiple dropped ranges: 0-100, 200-500, 700-1200 */
            picoquic_update_sack_list(&stream->deadline_ctx->receiver_dropped_ranges,
                0, 100, simulated_time);
            picoquic_update_sack_list(&stream->deadline_ctx->receiver_dropped_ranges,
                200, 500, simulated_time);
            picoquic_update_sack_list(&stream->deadline_ctx->receiver_dropped_ranges,
                700, 1200, simulated_time);
            
            /* Verify ranges were added */
            picosplay_node_t* node = picosplay_first(&stream->deadline_ctx->receiver_dropped_ranges.ack_tree);
            int range_count = 0;
            while (node != NULL) {
                picoquic_sack_item_t* sack = (picoquic_sack_item_t*)((char*)node - offsetof(struct st_picoquic_sack_item_t, node));
                range_count++;
                node = picosplay_next(node);
            }
            
            /* Add data chunks that span gaps */
            uint8_t data[2000];
            memset(data, 'X', sizeof(data));
            
            /* Queue data in chunks to simulate real network arrival */
            int new_data_available = 0;
            
            /* Queue data chunks between the gaps */
            /* First chunk: 100-200 (after first gap) */
            ret = picoquic_queue_network_input(quic, &stream->stream_data_tree,
                stream->consumed_offset, 100, data + 100, 100, 0, NULL,
                &new_data_available);
            
            /* Second chunk: 500-700 (after second gap) */
            if (ret == 0) {
                ret = picoquic_queue_network_input(quic, &stream->stream_data_tree,
                    stream->consumed_offset, 500, data + 500, 200, 0, NULL,
                    &new_data_available);
            }
            
            /* Third chunk: 1200-2000 (after third gap) */
            if (ret == 0) {
                ret = picoquic_queue_network_input(quic, &stream->stream_data_tree,
                    stream->consumed_offset, 1200, data + 1200, 800, 1, NULL,
                    &new_data_available);
            }
            
            if (new_data_available) {
                stream->is_output_stream = 1;
            }
            
            if (ret == 0) {
                /* Process stream data - should generate 3 gap notifications */
                stream->is_output_stream = 1;
                picoquic_stream_data_callback(cnx, stream);
                
                if (test_ctx.nb_gaps_received != 3) {
                    DBG_PRINTF("Error: expected 3 gaps, got %d\n", test_ctx.nb_gaps_received);
                    ret = -1;
                } else {
                    /* Verify gap lengths */
                    if (test_ctx.gap_lengths[0] != 100 ||
                        test_ctx.gap_lengths[1] != 300 ||
                        test_ctx.gap_lengths[2] != 500) {
                        DBG_PRINTF("Error: incorrect gap lengths: %lu, %lu, %lu\n",
                            test_ctx.gap_lengths[0], test_ctx.gap_lengths[1], 
                            test_ctx.gap_lengths[2]);
                        ret = -1;
                    }
                }
                
                /* Verify consumed offset skipped all gaps and consumed all data */
                if (ret == 0 && stream->consumed_offset != 2000) {
                    DBG_PRINTF("Error: expected consumed_offset 2000, got %lu\n",
                        stream->consumed_offset);
                    ret = -1;
                }
            }
        }
    }
    
    picoquic_free(quic);
    return ret;
}

/* Test 3: STREAM_DATA_DROPPED frame parsing and handling */
int deadline_stream_data_dropped_frame_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    picoquic_quic_t* quic = picoquic_create(8, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, simulated_time, &simulated_time, NULL, NULL, 0);
    
    if (quic == NULL) {
        return -1;
    }
    
    picoquic_cnx_t* cnx = picoquic_create_cnx(quic, 
        picoquic_null_connection_id, picoquic_null_connection_id,
        (struct sockaddr*)&test_addr, simulated_time, 0, "test-sni", "test-alpn", 1);
    
    if (cnx == NULL) {
        ret = -1;
    } else {
        /* Enable deadline-aware streams */
        cnx->local_parameters.enable_deadline_aware_streams = 1;
        cnx->remote_parameters.enable_deadline_aware_streams = 1;
        picoquic_init_deadline_context(cnx);
        
        /* Set connection state to ready and configure stream limits */
        cnx->cnx_state = picoquic_state_ready;
        cnx->max_stream_id_bidir_remote = 100;
        cnx->max_stream_id_unidir_remote = 103;
        
        /* Create a frame buffer */
        uint8_t frame_buffer[64];
        uint8_t* bytes = frame_buffer;
        uint8_t* bytes_max = frame_buffer + sizeof(frame_buffer);
        
        /* Encode STREAM_DATA_DROPPED frame */
        /* Use stream ID 1 (server-initiated bidirectional) since cnx is client */
        uint64_t stream_id = 1;
        uint64_t offset = 1000;
        uint64_t length = 500;
        
        /* Encode frame type as varint */
        bytes = picoquic_frames_varint_encode(bytes, bytes_max, picoquic_frame_type_stream_data_dropped);
        bytes = picoquic_frames_varint_encode(bytes, bytes_max, stream_id);
        bytes = picoquic_frames_varint_encode(bytes, bytes_max, offset);
        bytes = picoquic_frames_varint_encode(bytes, bytes_max, length);
        
        size_t frame_length = bytes - frame_buffer;
        
        /* Parse the frame - we need to skip past the frame type first */
        /* First, skip the frame type varint */
        const uint8_t* parse_start = frame_buffer;
        uint64_t frame_type_check;
        parse_start = picoquic_frames_varint_decode(parse_start, frame_buffer + frame_length, &frame_type_check);
        if (parse_start == NULL) {
            DBG_PRINTF("%s", "Error: failed to decode frame type\n");
            ret = -1;
        } else {
            const uint8_t* parsed = picoquic_parse_stream_data_dropped_frame(cnx,
                parse_start, frame_buffer + frame_length, simulated_time);
            
            if (parsed == NULL) {
                DBG_PRINTF("%s", "Error: failed to parse STREAM_DATA_DROPPED frame\n");
                ret = -1;
            } else if (parsed != frame_buffer + frame_length) {
                DBG_PRINTF("%s", "Error: parsing stopped at wrong position\n");
                ret = -1;
            } else {
                /* Verify the stream was created and dropped range recorded */
                picoquic_stream_head_t* stream = picoquic_find_stream(cnx, stream_id);
                if (stream == NULL) {
                    DBG_PRINTF("%s", "Error: stream not created\n");
                    ret = -1;
                } else if (stream->deadline_ctx == NULL) {
                    DBG_PRINTF("%s", "Error: deadline context not initialized\n");
                    ret = -1;
                } else {
                    /* Check if dropped range was recorded */
                    picosplay_node_t* node = picosplay_first(&stream->deadline_ctx->receiver_dropped_ranges.ack_tree);
                    if (node == NULL) {
                        DBG_PRINTF("%s", "Error: no dropped ranges recorded\n");
                        ret = -1;
                    } else {
                        picoquic_sack_item_t* sack = (picoquic_sack_item_t*)((char*)node - 
                            offsetof(struct st_picoquic_sack_item_t, node));
                        if (sack->start_of_sack_range != offset || 
                            sack->end_of_sack_range != offset + length) {
                            DBG_PRINTF("Error: incorrect dropped range: %lu-%lu\n",
                                sack->start_of_sack_range, sack->end_of_sack_range);
                            ret = -1;
                        }
                    }
                }
            }
        }
    }
    
    picoquic_free(quic);
    return ret;
}

/* Main test function */
int deadline_partial_reliability_test()
{
    int ret = 0;
    
    /* Test 1: Basic partial reliability */
    if (ret == 0) {
        ret = deadline_partial_reliability_basic_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "Basic partial reliability test failed\n");
        }
    }
    
    /* Test 2: Multiple gaps */
    if (ret == 0) {
        ret = deadline_partial_reliability_multiple_gaps_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "Multiple gaps test failed\n");
        }
    }
    
    /* Test 3: STREAM_DATA_DROPPED frame */
    if (ret == 0) {
        ret = deadline_stream_data_dropped_frame_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "STREAM_DATA_DROPPED frame test failed\n");
        }
    }
    
    return ret;
}