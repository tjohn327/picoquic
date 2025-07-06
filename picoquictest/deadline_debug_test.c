/*
* Debug test to isolate ACK gap error with deadline streams
*/

#include "../picoquic/picoquic_internal.h"
#include "../picoquic/tls_api.h"
#include "picoquictest_internal.h"
#include <stdlib.h>
#include <string.h>

/* Simple connection test focusing on ACK generation */
int deadline_debug_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    uint8_t buffer[100];
    
    DBG_PRINTF("%s", "\n=== Deadline Debug Test ===\n");
    
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
        if (ret == 0) {
            DBG_PRINTF("%s", "Connection established\n");
        }
    }
    
    /* Set deadline on stream 4 instead of stream 0 */
    if (ret == 0) {
        ret = picoquic_set_stream_deadline(test_ctx->cnx_client, 4, 30, 1);
        DBG_PRINTF("Set deadline on stream 4: ret=%d\n", ret);
    }
    
    /* Send 5000 bytes on stream 4 */
    if (ret == 0) {
        uint8_t large_buffer[5000];
        memset(large_buffer, 0xCD, sizeof(large_buffer));
        ret = picoquic_add_to_stream(test_ctx->cnx_client, 4, large_buffer, sizeof(large_buffer), 1);
        DBG_PRINTF("Added 5000 bytes to stream 4: ret=%d\n", ret);
    }
    
    /* Run longer simulation like the failing test */
    if (ret == 0) {
        uint64_t start_time = simulated_time;
        uint64_t end_time = start_time + 200000; /* 200ms like failing test */
        int nb_rounds = 0;
        int inactive_count = 0;
        
        while (simulated_time < end_time && ret == 0 && nb_rounds < 1000) {
            int was_active = 0;
            ret = tls_api_one_sim_round(test_ctx, &simulated_time, 0, &was_active);
            nb_rounds++;
            
            if (!was_active) {
                inactive_count++;
                if (inactive_count > 20) {
                    break;
                }
            } else {
                inactive_count = 0;
            }
            
            if (nb_rounds % 10 == 0) {
                DBG_PRINTF("Round %d, time=%lu\n", nb_rounds, (unsigned long)simulated_time);
            }
        }
        
        DBG_PRINTF("Simulation completed at time %lu after %d rounds\n", 
            (unsigned long)simulated_time, nb_rounds);
    }
    
    /* Cleanup */
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    if (ret == 0) {
        DBG_PRINTF("%s", "\n=== Debug Test PASSED ===\n");
    } else {
        DBG_PRINTF("\n=== Debug Test FAILED (ret=%d) ===\n", ret);
    }
    
    return ret;
}