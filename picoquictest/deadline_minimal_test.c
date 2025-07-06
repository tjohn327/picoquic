/*
* Minimal test for deadline functionality debugging
*/

#include "../picoquic/picoquic_internal.h"
#include "../picoquic/tls_api.h"
#include "picoquictest_internal.h"
#include <stdlib.h>
#include <string.h>

int deadline_minimal_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    
    DBG_PRINTF("%s", "Starting minimal deadline test\n");
    
    /* Initialize test context */
    ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 1, 0);
    
    if (ret == 0) {
        /* Enable deadline-aware streams on client */
        test_ctx->cnx_client->local_parameters.enable_deadline_aware_streams = 1;
        
        /* Set server default transport parameters */
        picoquic_tp_t server_tp;
        memset(&server_tp, 0, sizeof(picoquic_tp_t));
        picoquic_init_transport_parameters(&server_tp, 1);
        server_tp.enable_deadline_aware_streams = 1;
        picoquic_set_default_tp(test_ctx->qserver, &server_tp);
        
        /* Start the client connection */
        ret = picoquic_start_client_cnx(test_ctx->cnx_client);
    }
    
    if (ret == 0) {
        /* Establish connection */
        ret = tls_api_connection_loop(test_ctx, NULL, 0, &simulated_time);
        if (ret == 0) {
            DBG_PRINTF("%s", "Connection established\n");
        }
    }
    
    if (ret == 0) {
        /* Check deadline context */
        if (test_ctx->cnx_client->deadline_context != NULL) {
            DBG_PRINTF("%s", "Client deadline context OK\n");
        }
        if (test_ctx->cnx_server && test_ctx->cnx_server->deadline_context != NULL) {
            DBG_PRINTF("%s", "Server deadline context OK\n");
        }
    }
    
    if (ret == 0) {
        /* Set deadline without sending data */
        ret = picoquic_set_stream_deadline(test_ctx->cnx_client, 0, 50, 1);
        DBG_PRINTF("Set deadline result: %d\n", ret);
    }
    
    if (ret == 0) {
        /* Send just 100 bytes */
        uint8_t buffer[100];
        memset(buffer, 0xAB, sizeof(buffer));
        ret = picoquic_add_to_stream(test_ctx->cnx_client, 0, buffer, sizeof(buffer), 1);
        DBG_PRINTF("Send result: %d\n", ret);
    }
    
    if (ret == 0) {
        /* Run just a few rounds */
        for (int i = 0; i < 10 && ret == 0; i++) {
            int was_active = 0;
            ret = tls_api_one_sim_round(test_ctx, &simulated_time, 0, &was_active);
            if (!was_active) {
                break;
            }
        }
    }
    
    /* Cleanup */
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    DBG_PRINTF("Minimal test finished, ret=%d\n", ret);
    
    return ret;
}