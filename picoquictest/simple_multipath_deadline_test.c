/*
 * Simple test to verify multipath works with DMTP negotiated
 * This is a minimal version to debug the multipath establishment issue
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "picoquic.h"
#include "picoquic_internal.h"
#include "picoquictest_internal.h"
#include "tls_api.h"

/* External function declarations */
extern int wait_multipath_ready(picoquic_test_tls_api_ctx_t* test_ctx, uint64_t* simulated_time);
extern void multipath_init_params(picoquic_tp_t *test_parameters, int enable_time_stamp);

static test_api_stream_desc_t test_scenario_simple[] = {
    { 4, 0, 257, 10000 }
};

int simple_multipath_deadline_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    picoquic_connection_id_t initial_cid = { {0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4}, 8 };
    picoquic_tp_t server_parameters;
    int ret;
    
    /* Create context with delayed initialization */
    ret = tls_api_init_ctx_ex2(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 1, 0, &initial_cid,
        8, 0, 0, 0);
        
    if (ret == 0 && test_ctx == NULL) {
        ret = -1;
    }
    
    /* Configure basic multipath with DMTP */
    if (ret == 0) {
        test_ctx->c_to_s_link->queue_delay_max = 2 * test_ctx->c_to_s_link->microsec_latency;
        test_ctx->s_to_c_link->queue_delay_max = 2 * test_ctx->s_to_c_link->microsec_latency;
        
        /* Set server transport parameters */
        memset(&server_parameters, 0, sizeof(picoquic_tp_t));
        picoquic_init_transport_parameters(&server_parameters, 1);
        server_parameters.enable_time_stamp = 3;
        server_parameters.is_multipath_enabled = 1;
        server_parameters.initial_max_path_id = 2;
        server_parameters.enable_deadline_aware_streams = 1; /* Enable DMTP */
        
        picoquic_set_default_tp(test_ctx->qserver, &server_parameters);
        
        /* Set client transport parameters */
        test_ctx->cnx_client->local_parameters.enable_time_stamp = 3;
        test_ctx->cnx_client->local_parameters.is_multipath_enabled = 1;
        test_ctx->cnx_client->local_parameters.initial_max_path_id = 2;
        test_ctx->cnx_client->local_parameters.enable_deadline_aware_streams = 1; /* Enable DMTP */
        
        /* Enable logging for debugging */
        picoquic_set_binlog(test_ctx->qserver, ".");
        test_ctx->qserver->use_long_log = 1;
        picoquic_set_binlog(test_ctx->qclient, ".");
        test_ctx->qclient->use_long_log = 1;
        binlog_new_connection(test_ctx->cnx_client);
    }
    
    /* Establish connection */
    if (ret == 0) {
        picoquic_start_client_cnx(test_ctx->cnx_client);
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 2 * test_ctx->s_to_c_link->microsec_latency, &simulated_time);
    }
    
    /* Verify multipath and DMTP are negotiated */
    if (ret == 0) {
        if (!test_ctx->cnx_client->is_multipath_enabled || !test_ctx->cnx_server->is_multipath_enabled) {
            DBG_PRINTF("Multipath not negotiated (c=%d, s=%d)\n",
                test_ctx->cnx_client->is_multipath_enabled, test_ctx->cnx_server->is_multipath_enabled);
            ret = -1;
        }
        if (!picoquic_is_deadline_aware_negotiated(test_ctx->cnx_client) ||
            !picoquic_is_deadline_aware_negotiated(test_ctx->cnx_server)) {
            DBG_PRINTF("%s", "Deadline-aware streams not negotiated\n");
            ret = -1;
        }
    }
    
    /* Wait until ready */
    if (ret == 0) {
        ret = wait_client_connection_ready(test_ctx, &simulated_time);
        DBG_PRINTF("After wait_ready: client state = %d\n", test_ctx->cnx_client->cnx_state);
    }
    
    /* Initialize scenario to trigger state transition */
    if (ret == 0) {
        ret = test_api_init_send_recv_scenario(test_ctx, test_scenario_simple, sizeof(test_scenario_simple));
    }
    
    /* Add second link pair */
    if (ret == 0) {
        test_ctx->client_addr_2 = test_ctx->client_addr;
        test_ctx->client_addr_2.sin_port += 17;
        
        test_ctx->c_to_s_link_2 = picoquictest_sim_link_create(0.01, 10000000, NULL, 20000, 0);
        test_ctx->s_to_c_link_2 = picoquictest_sim_link_create(0.01, 10000000, NULL, 20000, 0);
        
        if (test_ctx->c_to_s_link_2 == NULL || test_ctx->s_to_c_link_2 == NULL) {
            ret = -1;
        }
    }
    
    /* Probe new path */
    if (ret == 0) {
        DBG_PRINTF("Before probe: client state = %d, paths = %d\n", 
            test_ctx->cnx_client->cnx_state, test_ctx->cnx_client->nb_paths);
        ret = picoquic_probe_new_path(test_ctx->cnx_client, (struct sockaddr*) & test_ctx->server_addr,
            (struct sockaddr*) & test_ctx->client_addr_2, simulated_time);
        DBG_PRINTF("After probe: ret = %d, client paths = %d\n", ret, test_ctx->cnx_client->nb_paths);
    }
    
    /* Wait for multipath to be ready */
    if (ret == 0) {
        ret = wait_multipath_ready(test_ctx, &simulated_time);
    }
    
    /* Clean up */
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    return ret;
}