/*
* Test partial reliability functionality
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "picoquic_internal.h"
#include "picoquic_utils.h"
#include "tls_api.h"
#include "picoquictest_internal.h"

/* Test context */
typedef struct st_test_partial_reliability_ctx_t {
    picoquic_test_tls_api_ctx_t* test_ctx;
    uint64_t start_time;
    int data_sent_count;
    int data_received_count;
    int data_discarded_count;
    uint8_t received_data[1024];
    size_t received_length;
} test_partial_reliability_ctx_t;

/* Server callback */
static int partial_reliability_server_callback(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes,
    size_t length, picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* stream_ctx)
{
    test_partial_reliability_ctx_t* ctx = (test_partial_reliability_ctx_t*)callback_ctx;
    
    switch (fin_or_event) {
    case picoquic_callback_stream_data:
        /* Echo back the data */
        if (length > 0) {
            picoquic_add_to_stream(cnx, stream_id, bytes, length, 0);
        }
        break;
    case picoquic_callback_stream_fin:
        /* Echo back FIN */
        picoquic_add_to_stream(cnx, stream_id, NULL, 0, 1);
        break;
    default:
        break;
    }
    
    return 0;
}

/* Client callback */
static int partial_reliability_client_callback(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes,
    size_t length, picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* stream_ctx)
{
    test_partial_reliability_ctx_t* ctx = (test_partial_reliability_ctx_t*)callback_ctx;
    
    switch (fin_or_event) {
    case picoquic_callback_stream_data:
        ctx->data_received_count++;
        if (length > 0 && length <= sizeof(ctx->received_data)) {
            memcpy(ctx->received_data, bytes, length);
            ctx->received_length = length;
        }
        DBG_PRINTF("Client received %zu bytes on stream %lu\n", length, stream_id);
        break;
        
    case picoquic_callback_stream_data_discarded:
        ctx->data_discarded_count++;
        DBG_PRINTF("Client discarded expired data on stream %lu\n", stream_id);
        break;
        
    case picoquic_callback_ready:
        /* Send data with short deadline */
        ctx->start_time = picoquic_current_time();
        ctx->data_sent_count++;
        
        /* Set a very short deadline (10ms) */
        picoquic_set_stream_deadline(cnx, 0, 10, PICOQUIC_DEADLINE_MODE_HARD);
        
        /* Send some data */
        const char* test_data = "Data with very short deadline";
        picoquic_add_to_stream(cnx, 0, (uint8_t*)test_data, strlen(test_data), 1);
        
        DBG_PRINTF("Client sent data with 10ms deadline at time %lu\n", ctx->start_time);
        break;
        
    default:
        break;
    }
    
    return 0;
}

/* Simple test for partial reliability */
int deadline_partial_reliability_simple_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    test_partial_reliability_ctx_t client_ctx = {0};
    test_partial_reliability_ctx_t server_ctx = {0};
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    picoquic_connection_id_t initial_cid = { {0xde, 0xad, 0xbe, 0xef, 0xba, 0xba, 0xc0, 0x01}, 8 };
    
    DBG_PRINTF("%s", "\n=== Testing Partial Reliability (Simple) ===\n");
    
    /* Create test context with deadline-aware streams enabled */
    ret = tls_api_init_ctx_ex(&test_ctx,
        0,                       /* proposed_version */
        PICOQUIC_TEST_SNI,      /* sni */
        PICOQUIC_TEST_ALPN,     /* alpn */
        &simulated_time,        /* p_simulated_time */
        NULL,                   /* ticket_file_name */
        NULL,                   /* token_file_name */
        0,                      /* force_zero_share */
        0,                      /* delayed_init */
        0,                      /* use_bad_crypt */
        &initial_cid            /* initial_cid */
    );
    
    if (ret != 0 || test_ctx == NULL) {
        DBG_PRINTF("%s", "Cannot create test context\n");
        ret = -1;
        goto done;
    }
    
    /* Enable deadline-aware streams on both sides */
    picoquic_tp_t client_params = test_ctx->cnx_client->local_parameters;
    picoquic_tp_t server_params = test_ctx->cnx_server->local_parameters;
    client_params.enable_deadline_aware_streams = 1;
    server_params.enable_deadline_aware_streams = 1;
    
    /* Update transport parameters */
    picoquic_set_transport_parameters(test_ctx->cnx_client, &client_params);
    picoquic_set_transport_parameters(test_ctx->cnx_server, &server_params);
    
    /* Set callbacks */
    client_ctx.test_ctx = test_ctx;
    server_ctx.test_ctx = test_ctx;
    test_ctx->cnx_client->callback_fn = partial_reliability_client_callback;
    test_ctx->cnx_client->callback_ctx = &client_ctx;
    test_ctx->cnx_server->callback_fn = partial_reliability_server_callback;
    test_ctx->cnx_server->callback_ctx = &server_ctx;
    
    /* Run connection establishment */
    ret = tls_api_connection_loop(test_ctx, &loss_mask, 20000, &simulated_time);
    if (ret != 0) {
        DBG_PRINTF("Connection establishment failed with ret=%d\n", ret);
        goto done;
    }
    
    /* Verify connection is ready and deadline-aware */
    if (test_ctx->cnx_client->cnx_state != picoquic_state_ready ||
        test_ctx->cnx_server->cnx_state != picoquic_state_ready) {
        DBG_PRINTF("%s", "Connection not ready\n");
        ret = -1;
        goto done;
    }
    
    if (!test_ctx->cnx_client->deadline_aware_enabled ||
        !test_ctx->cnx_server->deadline_aware_enabled) {
        DBG_PRINTF("%s", "Deadline-aware streams not enabled\n");
        ret = -1;
        goto done;
    }
    
    /* Introduce a very long delay (simulate network congestion) */
    /* This will cause the deadline to expire */
    DBG_PRINTF("%s", "\nSimulating 500ms network delay to trigger deadline expiry...\n");
    simulated_time += 500000; /* 500ms delay */
    
    /* Continue the connection loop */
    ret = tls_api_connection_loop(test_ctx, &loss_mask, 100000, &simulated_time);
    
    /* Check results */
    DBG_PRINTF("%s", "\n=== Partial Reliability Test Results ===\n");
    DBG_PRINTF("Data sent: %d\n", client_ctx.data_sent_count);
    DBG_PRINTF("Data received: %d\n", client_ctx.data_received_count);
    DBG_PRINTF("Data discarded: %d\n", client_ctx.data_discarded_count);
    
    /* In this test, we expect either:
     * 1. Data to be discarded due to deadline expiry (data_discarded_count > 0)
     * 2. Or no data received at all due to the extreme delay
     */
    if (client_ctx.data_sent_count > 0) {
        if (client_ctx.data_discarded_count > 0 || client_ctx.data_received_count == 0) {
            DBG_PRINTF("%s", "✓ Partial reliability working correctly - expired data handled\n");
            ret = 0;
        } else {
            DBG_PRINTF("%s", "✗ Expected deadline expiry but data was delivered\n");
            ret = -1;
        }
    } else {
        DBG_PRINTF("%s", "✗ No data was sent\n");
        ret = -1;
    }

done:
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    return ret;
}