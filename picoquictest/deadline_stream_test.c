/*
* Simple test for deadline-aware stream creation and parameter verification
*/

#include <string.h>
#include <stdlib.h>
#include "picoquic_internal.h"
#include "picoquictest_internal.h"
#include "tls_api.h"

/*
 * Test basic deadline stream creation and parameter setting
 */
int deadline_stream_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = 0;
    
    /* Create test context with simulated time */
    ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, 
                          &simulated_time, NULL, NULL, 0, 0, 0);

    // test_ctx->cnx_client->local_parameters.enable_deadline_aware_streams = 1;
    // test_ctx->cnx_client->enable_deadline_aware_streams = 1;
    // test_ctx->cnx_server->local_parameters.enable_deadline_aware_streams = 1;
    // test_ctx->cnx_server->enable_deadline_aware_streams = 1;
    
    /* Establish connection */
    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }
    
    if (ret == 0) {
        ret = wait_client_connection_ready(test_ctx, &simulated_time);
    }



    
    /* Test 1: Verify deadline stream creation fails without negotiation */
    if (ret == 0) {
        uint64_t stream_id = picoquic_create_deadline_stream(
            test_ctx->cnx_client, 100, 50000, 20);
        
        if (stream_id != UINT64_MAX) {
            DBG_PRINTF("%s:%u:%s: Stream creation should fail without negotiation\n", 
                       __FILE__, __LINE__, __func__);
            ret = -1;
        }
    }
    
    /* Test 2: Enable deadline-aware streams and verify creation succeeds */
    if (ret == 0) {

        test_ctx->cnx_client->local_parameters.enable_deadline_aware_streams = 1;
        test_ctx->cnx_client->remote_parameters.enable_deadline_aware_streams = 1;
        test_ctx->cnx_client->enable_deadline_aware_streams = 1;
        test_ctx->cnx_server->local_parameters.enable_deadline_aware_streams = 1;
        test_ctx->cnx_server->remote_parameters.enable_deadline_aware_streams = 1;
        test_ctx->cnx_server->enable_deadline_aware_streams = 1;
        
        uint64_t stream_id = picoquic_create_deadline_stream(
            test_ctx->cnx_client, 100, 50000, 20);
        
        if (stream_id == UINT64_MAX) {
            DBG_PRINTF("%s:%u:%s: Failed to create deadline stream\n", 
                       __FILE__, __LINE__, __func__);
            ret = -1;
        }
        else {
            /* Verify stream parameters */
            picoquic_stream_head_t* stream = picoquic_find_stream(
                test_ctx->cnx_client, stream_id);
            
            if (stream == NULL) {
                DBG_PRINTF("%s:%u:%s: Stream not found\n", 
                           __FILE__, __LINE__, __func__);
                ret = -1;
            }
            else if (stream->deadline_ms != 100) {
                DBG_PRINTF("%s:%u:%s: Deadline not set correctly: %llu != 100\n", 
                           __FILE__, __LINE__, __func__, 
                           (unsigned long long)stream->deadline_ms);
                ret = -1;
            }
            else if (stream->expiry_threshold_abs != 50000) {
                DBG_PRINTF("%s:%u:%s: Absolute threshold not set correctly: %llu != 50000\n", 
                           __FILE__, __LINE__, __func__, 
                           (unsigned long long)stream->expiry_threshold_abs);
                ret = -1;
            }
            else if (stream->expiry_threshold_pct != 20) {
                DBG_PRINTF("%s:%u:%s: Percentage threshold not set correctly: %u != 20\n", 
                           __FILE__, __LINE__, __func__, stream->expiry_threshold_pct);
                ret = -1;
            }
            else if (stream->expired_bytes != 0) {
                DBG_PRINTF("%s:%u:%s: Expired bytes should be 0 initially\n", 
                           __FILE__, __LINE__, __func__);
                ret = -1;
            }
            else {
                DBG_PRINTF("%s:%u:%s: Deadline stream created successfully with correct parameters\n", 
                           __FILE__, __LINE__, __func__);
            }
        }
    }
    
    /* Test 3: Verify enqueue_time is set when adding data */
    if (ret == 0) {
        uint64_t stream_id = picoquic_create_deadline_stream(
            test_ctx->cnx_client, 200, 100000, 50);
        
        if (stream_id != UINT64_MAX) {
            uint8_t data[100];
            memset(data, 0x42, sizeof(data));
            
            uint64_t time_before = simulated_time;
            
            ret = picoquic_add_to_stream(test_ctx->cnx_client, stream_id, 
                                        data, sizeof(data), 0);
            
            if (ret == 0) {
                picoquic_stream_head_t* stream = picoquic_find_stream(
                    test_ctx->cnx_client, stream_id);
                
                if (stream && stream->send_queue) {
                    if (stream->send_queue->enqueue_time == 0) {
                        DBG_PRINTF("%s:%u:%s: Enqueue time not set\n", 
                                   __FILE__, __LINE__, __func__);
                        ret = -1;
                    }
                    else if (stream->send_queue->enqueue_time > time_before + 1000) {
                        DBG_PRINTF("%s:%u:%s: Enqueue time too far in future\n", 
                                   __FILE__, __LINE__, __func__);
                        ret = -1;
                    }
                    else {
                        DBG_PRINTF("%s:%u:%s: Enqueue time set correctly: %llu\n", 
                                   __FILE__, __LINE__, __func__,
                                   (unsigned long long)stream->send_queue->enqueue_time);
                    }
                }
            }
        }
    }
    
    /* Test 4: Verify multiple chunks have different enqueue times */
    if (ret == 0) {
        uint64_t stream_id = picoquic_create_deadline_stream(
            test_ctx->cnx_client, 300, 200000, 75);
        
        if (stream_id != UINT64_MAX) {
            uint8_t data1[50];
            uint8_t data2[60];
            memset(data1, 0x11, sizeof(data1));
            memset(data2, 0x22, sizeof(data2));
            
            /* Add first chunk */
            ret = picoquic_add_to_stream(test_ctx->cnx_client, stream_id, 
                                        data1, sizeof(data1), 0);
            
            /* Advance time */
            simulated_time += 50000; /* 50ms */
            
            /* Add second chunk */
            if (ret == 0) {
                ret = picoquic_add_to_stream(test_ctx->cnx_client, stream_id, 
                                            data2, sizeof(data2), 0);
            }
            
            if (ret == 0) {
                picoquic_stream_head_t* stream = picoquic_find_stream(
                    test_ctx->cnx_client, stream_id);
                
                if (stream && stream->send_queue && stream->send_queue->next_stream_data) {
                    uint64_t time1 = stream->send_queue->enqueue_time;
                    uint64_t time2 = stream->send_queue->next_stream_data->enqueue_time;
                    
                    if (time2 <= time1) {
                        DBG_PRINTF("%s:%u:%s: Second chunk should have later enqueue time\n", 
                                   __FILE__, __LINE__, __func__);
                        ret = -1;
                    }
                    else if (time2 - time1 < 40000 || time2 - time1 > 60000) {
                        DBG_PRINTF("%s:%u:%s: Time difference incorrect: %llu\n", 
                                   __FILE__, __LINE__, __func__,
                                   (unsigned long long)(time2 - time1));
                        ret = -1;
                    }
                    else {
                        DBG_PRINTF("%s:%u:%s: Multiple chunks have correct enqueue times\n", 
                                   __FILE__, __LINE__, __func__);
                    }
                }
            }
        }
    }
    
    /* Clean up */
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    return ret;
}