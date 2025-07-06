/*
* Minimal reproducer for deadline ACK corruption bug
*/

#include "../picoquic/picoquic_internal.h"
#include "../picoquic/tls_api.h"
#include "picoquictest_internal.h"
#include <stdlib.h>
#include <string.h>

int deadline_minimal_repro_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    uint8_t buffer[100];
    
    /* Initialize test context */
    ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 1, 0);
    
    if (ret == 0) {
        /* Enable deadline-aware streams */
        test_ctx->cnx_client->local_parameters.enable_deadline_aware_streams = 1;
        
        /* Set server transport parameters */
        picoquic_tp_t server_tp;
        memset(&server_tp, 0, sizeof(server_tp));
        picoquic_init_transport_parameters(&server_tp, 1);
        server_tp.enable_deadline_aware_streams = 1;
        picoquic_set_default_tp(test_ctx->qserver, &server_tp);
        
        /* Start connection */
        ret = picoquic_start_client_cnx(test_ctx->cnx_client);
    }
    
    /* Complete handshake */
    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, NULL, 0, &simulated_time);
    }
    
    /* Set deadline on stream 8 */
    if (ret == 0) {
        ret = picoquic_set_stream_deadline(test_ctx->cnx_client, 8, 50, 1);
        DBG_PRINTF("Set deadline: ret=%d\n", ret);
    }
    
    /* Send SMALL amount of data */
    if (ret == 0) {
        memset(buffer, 0xAB, sizeof(buffer));
        ret = picoquic_add_to_stream(test_ctx->cnx_client, 8, buffer, sizeof(buffer), 1);
        DBG_PRINTF("Added data: ret=%d\n", ret);
    }
    
    /* Run ONE simulation round */
    if (ret == 0) {
        int was_active = 0;
        ret = tls_api_one_sim_round(test_ctx, &simulated_time, 0, &was_active);
        DBG_PRINTF("Sim round: ret=%d, active=%d\n", ret, was_active);
    }
    
    /* Cleanup */
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    return ret;
}