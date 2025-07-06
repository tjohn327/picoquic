/*
* Simple test to debug deadline functionality
*/

#include "../picoquic/picoquic_internal.h"
#include "../picoquic/tls_api.h"
#include "picoquictest_internal.h"
#include <stdlib.h>
#include <string.h>

int deadline_simple_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    
    DBG_PRINTF("%s", "Starting simple deadline test\n");
    
    /* Initialize test context */
    ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 1, 0);
    
    if (ret == 0) {
        DBG_PRINTF("%s", "Test context initialized\n");
        
        /* Enable deadline-aware streams on client connection */
        test_ctx->cnx_client->local_parameters.enable_deadline_aware_streams = 1;
        
        /* Set server default transport parameters to enable deadline-aware streams */
        picoquic_tp_t server_tp;
        memset(&server_tp, 0, sizeof(picoquic_tp_t));
        picoquic_init_transport_parameters(&server_tp, 1); /* server mode */
        server_tp.enable_deadline_aware_streams = 1;
        picoquic_set_default_tp(test_ctx->qserver, &server_tp);
        
        /* Start the client connection */
        ret = picoquic_start_client_cnx(test_ctx->cnx_client);
        
        DBG_PRINTF("%s", "Deadline streams parameters set\n");
    }
    
    if (ret == 0) {
        /* Establish connection */
        DBG_PRINTF("%s", "Establishing connection...\n");
        ret = tls_api_connection_loop(test_ctx, NULL, 0, &simulated_time);
        DBG_PRINTF("Connection established, ret=%d\n", ret);
    }
    
    if (ret == 0) {
        /* Check if deadline context was created */
        if (test_ctx->cnx_client->deadline_context != NULL) {
            DBG_PRINTF("%s", "Client deadline context created\n");
        } else {
            DBG_PRINTF("%s", "Client deadline context NOT created\n");
        }
        
        if (test_ctx->cnx_server->deadline_context != NULL) {
            DBG_PRINTF("%s", "Server deadline context created\n");
        } else {
            DBG_PRINTF("%s", "Server deadline context NOT created\n");
        }
    }
    
    if (ret == 0) {
        /* Try to set a deadline */
        DBG_PRINTF("%s", "Setting deadline on stream 0\n");
        ret = picoquic_set_stream_deadline(test_ctx->cnx_client, 0, 100, 1);
        DBG_PRINTF("Set deadline result: %d\n", ret);
    }
    
    if (ret == 0) {
        /* Send some data */
        uint8_t buffer[1000];
        memset(buffer, 0xAB, sizeof(buffer));
        DBG_PRINTF("%s", "Sending data\n");
        ret = picoquic_add_to_stream(test_ctx->cnx_client, 0, buffer, sizeof(buffer), 1);
        DBG_PRINTF("Send data result: %d\n", ret);
    }
    
    if (ret == 0) {
        /* Run a few simulation rounds */
        DBG_PRINTF("%s", "Running simulation\n");
        for (int i = 0; i < 5 && ret == 0; i++) {
            ret = tls_api_one_sim_round(test_ctx, &simulated_time, 10000, NULL);
            DBG_PRINTF("Sim round %d, time=%lu\n", i, (unsigned long)simulated_time);
        }
    }
    
    /* Cleanup */
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    DBG_PRINTF("Simple test finished, ret=%d\n", ret);
    
    return ret;
}