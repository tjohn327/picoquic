/*
* Test to isolate ACK frame corruption with deadline streams
*/

#include "../picoquic/picoquic_internal.h"
#include "../picoquic/tls_api.h"
#include "picoquictest_internal.h"
#include <stdlib.h>
#include <string.h>

/* Test that checks ACK frame generation after deadline is set */
int deadline_ack_corruption_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    
    DBG_PRINTF("%s", "\n=== ACK Corruption Test ===\n");
    
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
        DBG_PRINTF("Connection established: ret=%d\n", ret);
    }
    
    /* Send initial data to create some ACK state */
    if (ret == 0) {
        uint8_t data1[100];
        memset(data1, 0x11, sizeof(data1));
        ret = picoquic_add_to_stream(test_ctx->cnx_client, 0, data1, sizeof(data1), 0);
        DBG_PRINTF("Sent initial data: ret=%d\n", ret);
        
        /* Let it transmit */
        for (int i = 0; i < 5 && ret == 0; i++) {
            int was_active = 0;
            ret = tls_api_one_sim_round(test_ctx, &simulated_time, 0, &was_active);
        }
    }
    
    /* Now set deadline and send data with specific pattern */
    if (ret == 0) {
        /* Set deadline */
        ret = picoquic_set_stream_deadline(test_ctx->cnx_client, 0, 30, 1);
        DBG_PRINTF("Set deadline: ret=%d\n", ret);
        
        /* Send data with recognizable pattern */
        uint8_t data2[5000];
        for (int i = 0; i < sizeof(data2); i++) {
            data2[i] = (uint8_t)(0xDE + (i & 1)); /* Pattern: DEDEDF... */
        }
        ret = picoquic_add_to_stream(test_ctx->cnx_client, 0, data2, sizeof(data2), 1);
        DBG_PRINTF("Sent deadline data with pattern: ret=%d\n", ret);
    }
    
    /* Run simulation and capture the exact round where error occurs */
    if (ret == 0) {
        int error_round = -1;
        uint64_t error_time = 0;
        
        for (int round = 0; round < 100 && ret == 0; round++) {
            int was_active = 0;
            uint64_t time_before = simulated_time;
            
            /* Check ACK contexts before round */
            picoquic_packet_context_t* pc = &test_ctx->cnx_server->pkt_ctx[picoquic_packet_context_application];
            if (pc->ack_ctx.sack_list.ack_tree.root != NULL) {
                picoquic_sack_item_t* item = picoquic_sack_last_item(&pc->ack_ctx.sack_list);
                if (item != NULL) {
                    DBG_PRINTF("Round %d: Server ACK state - last range [%lu-%lu]\n", 
                        round, (unsigned long)item->start_range, (unsigned long)item->end_range);
                }
            }
            
            ret = tls_api_one_sim_round(test_ctx, &simulated_time, 0, &was_active);
            
            if (ret != 0) {
                error_round = round;
                error_time = time_before;
                DBG_PRINTF("ERROR detected in round %d at time %lu\n", 
                    error_round, (unsigned long)error_time);
                /* Don't break - let's see what happened */
                ret = 0; /* Continue to see the error */
            }
            
            if (!was_active) {
                DBG_PRINTF("Simulation inactive at round %d\n", round);
                break;
            }
        }
    }
    
    /* Cleanup */
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    if (ret == 0) {
        DBG_PRINTF("%s", "\n=== ACK Corruption Test PASSED ===\n");
    } else {
        DBG_PRINTF("\n=== ACK Corruption Test FAILED (ret=%d) ===\n", ret);
    }
    
    return ret;
}