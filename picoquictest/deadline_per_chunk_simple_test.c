/*
* Simple test for per-chunk deadline functionality
*/

#include "../picoquic/picoquic_internal.h"
#include "../picoquictest/picoquictest_internal.h"
#include <stdlib.h>
#include <string.h>

int deadline_per_chunk_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = 0;
    
    /* Create test context */
    ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 1, 0);
    
    if (ret == 0 && test_ctx == NULL) {
        DBG_PRINTF("%s", "Test context is NULL\n");
        return -1;
    }
    
    if (ret == 0) {
        /* Initialize deadline contexts */
        test_ctx->cnx_client->deadline_context = (picoquic_deadline_context_t*)malloc(sizeof(picoquic_deadline_context_t));
        if (test_ctx->cnx_client->deadline_context != NULL) {
            memset(test_ctx->cnx_client->deadline_context, 0, sizeof(picoquic_deadline_context_t));
            test_ctx->cnx_client->deadline_context->deadline_scheduling_active = 1;
        }
        
        test_ctx->cnx_server->deadline_context = (picoquic_deadline_context_t*)malloc(sizeof(picoquic_deadline_context_t));
        if (test_ctx->cnx_server->deadline_context != NULL) {
            memset(test_ctx->cnx_server->deadline_context, 0, sizeof(picoquic_deadline_context_t));
            test_ctx->cnx_server->deadline_context->deadline_scheduling_active = 1;
        }
        
        /* Enable deadline support */
        test_ctx->cnx_client->local_parameters.enable_deadline_aware_streams = 1;
        test_ctx->cnx_client->remote_parameters.enable_deadline_aware_streams = 1;
        test_ctx->cnx_server->local_parameters.enable_deadline_aware_streams = 1;
        test_ctx->cnx_server->remote_parameters.enable_deadline_aware_streams = 1;
        
        /* Complete handshake */
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }
    
    if (ret == 0) {
        DBG_PRINTF("%s", "Handshake complete, creating stream\n");
        
        /* Create stream with ID 4 (first non-reserved stream) */
        picoquic_stream_head_t* stream = picoquic_create_stream(test_ctx->cnx_client, 4);
        if (stream == NULL) {
            DBG_PRINTF("%s", "Failed to create stream\n");
            ret = -1;
        } else {
            /* Set stream deadline - 100ms */
            ret = picoquic_set_stream_deadline(test_ctx->cnx_client, 4, 100, 1);
            if (ret != 0) {
                DBG_PRINTF("Failed to set stream deadline: %d\n", ret);
            }
        }
    }
    
    if (ret == 0) {
        DBG_PRINTF("%s", "Adding chunks at different times\n");
        
        /* Add first chunk at time 0 */
        uint8_t data1[100];
        memset(data1, 'A', 100);
        ret = picoquic_add_to_stream(test_ctx->cnx_client, 4, data1, 100, 0);
        
        if (ret == 0) {
            /* Advance time by 50ms */
            simulated_time += 50000;
            
            /* Add second chunk */
            uint8_t data2[100];
            memset(data2, 'B', 100);
            ret = picoquic_add_to_stream(test_ctx->cnx_client, 4, data2, 100, 1);
        }
    }
    
    /* Verify chunks have different deadlines */
    if (ret == 0) {
        picoquic_stream_head_t* stream = picoquic_find_stream(test_ctx->cnx_client, 4);
        if (stream == NULL || stream->send_queue == NULL) {
            DBG_PRINTF("%s", "Stream or send queue is NULL\n");
            ret = -1;
        } else {
            picoquic_stream_queue_node_t* chunk1 = stream->send_queue;
            picoquic_stream_queue_node_t* chunk2 = chunk1 ? chunk1->next_stream_data : NULL;
            
            if (chunk1 == NULL || chunk2 == NULL) {
                DBG_PRINTF("%s", "Missing chunks\n");
                ret = -1;
            } else {
                DBG_PRINTF("Chunk 1: enqueue_time=%lu, deadline=%lu\n",
                    (unsigned long)chunk1->enqueue_time, (unsigned long)chunk1->chunk_deadline);
                DBG_PRINTF("Chunk 2: enqueue_time=%lu, deadline=%lu\n", 
                    (unsigned long)chunk2->enqueue_time, (unsigned long)chunk2->chunk_deadline);
                
                /* Verify different enqueue times */
                if (chunk1->enqueue_time == chunk2->enqueue_time) {
                    DBG_PRINTF("%s", "ERROR: Chunks have same enqueue time\n");
                    ret = -1;
                }
                
                /* Verify different deadlines */
                if (chunk1->chunk_deadline == chunk2->chunk_deadline) {
                    DBG_PRINTF("%s", "ERROR: Chunks have same deadline\n");
                    ret = -1;
                } else {
                    DBG_PRINTF("%s", "SUCCESS: Chunks have different deadlines\n");
                }
            }
        }
    }
    
    /* Cleanup */
    if (test_ctx != NULL) {
        if (test_ctx->cnx_client && test_ctx->cnx_client->deadline_context) {
            free(test_ctx->cnx_client->deadline_context);
            test_ctx->cnx_client->deadline_context = NULL;
        }
        if (test_ctx->cnx_server && test_ctx->cnx_server->deadline_context) {
            free(test_ctx->cnx_server->deadline_context);
            test_ctx->cnx_server->deadline_context = NULL;
        }
        tls_api_delete_ctx(test_ctx);
    }
    
    return ret;
}