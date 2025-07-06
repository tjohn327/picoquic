/*
* Simple test to verify per-chunk deadline calculation
*/

#include "../picoquic/picoquic_internal.h"
#include "../picoquictest/picoquictest_internal.h"
#include <stdlib.h>
#include <string.h>

int deadline_per_chunk_verify_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    
    /* Create minimal QUIC context */
    picoquic_quic_t* quic = picoquic_create(8, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 
        simulated_time, &simulated_time, NULL, NULL, 0);
    
    if (quic == NULL) {
        DBG_PRINTF("%s", "Failed to create QUIC context\n");
        return -1;
    }
    
    /* Create connection */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    
    picoquic_cnx_t* cnx = picoquic_create_cnx(quic, picoquic_null_connection_id, picoquic_null_connection_id,
        (struct sockaddr*)&addr, simulated_time, 0, "test-sni", "test-alpn", 1);
    
    if (cnx == NULL) {
        DBG_PRINTF("%s", "Failed to create connection\n");
        picoquic_free(quic);
        return -1;
    }
    
    /* Enable deadline support */
    cnx->local_parameters.enable_deadline_aware_streams = 1;
    cnx->remote_parameters.enable_deadline_aware_streams = 1;
    cnx->cnx_state = picoquic_state_ready;
    
    /* Initialize deadline context */
    picoquic_init_deadline_context(cnx);
    
    DBG_PRINTF("%s", "\n=== Per-Chunk Deadline Calculation Test ===\n");
    
    /* Create stream and set deadline */
    picoquic_stream_head_t* stream = picoquic_create_stream(cnx, 4);
    if (stream == NULL) {
        DBG_PRINTF("%s", "Failed to create stream\n");
        ret = -1;
    } else {
        /* Set 100ms deadline */
        ret = picoquic_set_stream_deadline(cnx, 4, 100, 1);
        if (ret == 0) {
            DBG_PRINTF("%s", "Stream 4: Set 100ms hard deadline\n\n");
            
            /* Test chunk additions at different times */
            
            /* Chunk 1 at time 0 */
            simulated_time = 0;
            uint8_t data1[100];
            memset(data1, 'A', 100);
            ret = picoquic_add_to_stream(cnx, 4, data1, 100, 0);
            
            if (ret == 0 && stream->send_queue) {
                DBG_PRINTF("Chunk 1: Added at %lu us, deadline = %lu us\n",
                    (unsigned long)stream->send_queue->enqueue_time,
                    (unsigned long)stream->send_queue->chunk_deadline);
                
                /* Verify deadline = enqueue_time + 100ms */
                uint64_t expected = stream->send_queue->enqueue_time + 100000;
                if (stream->send_queue->chunk_deadline != expected) {
                    DBG_PRINTF("ERROR: Expected deadline %lu, got %lu\n",
                        (unsigned long)expected,
                        (unsigned long)stream->send_queue->chunk_deadline);
                    ret = -1;
                }
            }
            
            /* Chunk 2 at time 50ms */
            simulated_time = 50000;
            uint8_t data2[100];
            memset(data2, 'B', 100);
            ret = picoquic_add_to_stream(cnx, 4, data2, 100, 0);
            
            if (ret == 0 && stream->send_queue && stream->send_queue->next_stream_data) {
                DBG_PRINTF("Chunk 2: Added at %lu us, deadline = %lu us\n",
                    (unsigned long)stream->send_queue->next_stream_data->enqueue_time,
                    (unsigned long)stream->send_queue->next_stream_data->chunk_deadline);
                
                /* Verify different deadlines */
                if (stream->send_queue->chunk_deadline == 
                    stream->send_queue->next_stream_data->chunk_deadline) {
                    DBG_PRINTF("%s", "ERROR: Chunks have same deadline!\n");
                    ret = -1;
                } else {
                    DBG_PRINTF("%s", "\nSUCCESS: Chunks have different deadlines based on enqueue time\n");
                    DBG_PRINTF("  Chunk 1 deadline: %lu us\n", 
                        (unsigned long)stream->send_queue->chunk_deadline);
                    DBG_PRINTF("  Chunk 2 deadline: %lu us\n", 
                        (unsigned long)stream->send_queue->next_stream_data->chunk_deadline);
                    DBG_PRINTF("  Difference: %lu us (expected ~50000 us)\n",
                        (unsigned long)(stream->send_queue->next_stream_data->chunk_deadline - 
                                      stream->send_queue->chunk_deadline));
                }
            }
        }
    }
    
    /* Cleanup */
    if (cnx->deadline_context) {
        free(cnx->deadline_context);
        cnx->deadline_context = NULL;
    }
    picoquic_delete_cnx(cnx);
    picoquic_free(quic);
    
    return ret;
}