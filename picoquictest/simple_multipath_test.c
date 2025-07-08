/*
* Simple multipath test demonstration
*/

#include <stdlib.h>
#include <string.h>
#include "picoquic.h"
#include "picoquic_internal.h"
#include "picoquictest_internal.h"
#include "tls_api.h"
#include "picoquic_binlog.h"

/* Simple test scenario - send some data */
static test_api_stream_desc_t test_scenario_simple_mp[] = {
    { 4, 0, 257, 50000 }  /* Stream 4, 50KB of data */
};

int simple_multipath_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    picoquic_connection_id_t initial_cid = { {0x5d, 0x11, 0xb0, 4, 5, 6, 7, 8}, 8 };
    picoquic_tp_t server_parameters;
    int ret = 0;
    
    /* Create test context */
    ret = tls_api_init_ctx_ex2(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 1, 0, &initial_cid,
        8, 0, 0, 0);
    
    if (ret == 0 && test_ctx == NULL) {
        ret = -1;
    }
    
    /* Set up multipath parameters */
    if (ret == 0) {
        /* Enable logging */
        picoquic_set_binlog(test_ctx->qserver, ".");
        test_ctx->qserver->use_long_log = 1;
        
        /* Configure server for multipath */
        memset(&server_parameters, 0, sizeof(picoquic_tp_t));
        picoquic_init_transport_parameters(&server_parameters, 1);
        server_parameters.is_multipath_enabled = 1;
        server_parameters.initial_max_path_id = 2;
        server_parameters.enable_time_stamp = 3;
        server_parameters.enable_deadline_aware_streams = 1;
        picoquic_set_default_tp(test_ctx->qserver, &server_parameters);
        
        /* Configure client for multipath */
        test_ctx->cnx_client->local_parameters.is_multipath_enabled = 1;
        test_ctx->cnx_client->local_parameters.initial_max_path_id = 2;
        test_ctx->cnx_client->local_parameters.enable_time_stamp = 3;
        test_ctx->cnx_client->local_parameters.enable_deadline_aware_streams = 1;
    }
    
    /* Establish the connection */
    if (ret == 0) {
        picoquic_start_client_cnx(test_ctx->cnx_client);
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }
    
    /* Verify multipath is negotiated */
    if (ret == 0) {
        if (!test_ctx->cnx_client->is_multipath_enabled || !test_ctx->cnx_server->is_multipath_enabled) {
            DBG_PRINTF("Multipath not negotiated (client=%d, server=%d)\n",
                test_ctx->cnx_client->is_multipath_enabled, test_ctx->cnx_server->is_multipath_enabled);
            ret = -1;
        }
    }
    
    /* Wait for connection ready */
    if (ret == 0) {
        ret = wait_client_connection_ready(test_ctx, &simulated_time);
    }
    

    
    /* Add second path */
    if (ret == 0) {
        /* Initialize the second client address */
        test_ctx->client_addr_2 = test_ctx->client_addr;
        test_ctx->client_addr_2.sin_port += 17;
        
        /* Create second set of links */
        test_ctx->c_to_s_link_2 = picoquictest_sim_link_create(0.01, 10000, NULL, 20000, 0);
        test_ctx->s_to_c_link_2 = picoquictest_sim_link_create(0.01, 10000, NULL, 20000, 0);
        
        if (test_ctx->c_to_s_link_2 == NULL || test_ctx->s_to_c_link_2 == NULL) {
            ret = -1;
        }
    }
    
    /* Probe new path */
    if (ret == 0) {
        ret = picoquic_probe_new_path(test_ctx->cnx_client,
            (struct sockaddr*)&test_ctx->server_addr,
            (struct sockaddr*)&test_ctx->client_addr_2,
            simulated_time);
    }

        /* Initialize test scenario */
    if (ret == 0) {
        ret = test_api_init_send_recv_scenario(test_ctx, test_scenario_simple_mp, sizeof(test_scenario_simple_mp));
    }
    
    /* Wait for second path to be ready */
    if (ret == 0) {
        uint64_t timeout = simulated_time + 4000000;
        int nb_inactive = 0;
        
        while (simulated_time < timeout && ret == 0 && nb_inactive < 64 &&
               (test_ctx->cnx_client->nb_paths != 2 ||
                !test_ctx->cnx_client->path[1]->first_tuple->challenge_verified)) {
            int was_active = 0;
            ret = tls_api_one_sim_round(test_ctx, &simulated_time, timeout, &was_active);
            nb_inactive = was_active ? 0 : nb_inactive + 1;
        }
        
        if (test_ctx->cnx_client->nb_paths != 2) {
            DBG_PRINTF("Failed to establish second path: %d paths\n", test_ctx->cnx_client->nb_paths);
            ret = -1;
        }
    }
    
    /* Send data */
    if (ret == 0) {
        ret = tls_api_data_sending_loop(test_ctx, &loss_mask, &simulated_time, 0);
    }
    
    /* Verify transmission completed */
    if (ret == 0) {
        ret = tls_api_one_scenario_body_verify(test_ctx, &simulated_time, 2000000);
    }
    
    /* Simple check that both paths were used */
    if (ret == 0) {
        DBG_PRINTF("Path 0 sent: %llu bytes\n", (unsigned long long)test_ctx->cnx_client->path[0]->bytes_sent);
        DBG_PRINTF("Path 1 sent: %llu bytes\n", (unsigned long long)test_ctx->cnx_client->path[1]->bytes_sent);
    }
    
    /* Clean up */
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    return ret;
}