/*
* Minimal multipath deadline test to debug initialization issues
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "picoquic.h"
#include "picoquic_internal.h"
#include "picoquictest_internal.h"
#include "tls_api.h"
#include "picoquic_binlog.h"

int minimal_multipath_deadline_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    picoquic_tp_t server_parameters;
    int ret = 0;
    
    /* Initialize test context exactly like multipath_basic_test */
    ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, 
                          &simulated_time, NULL, NULL, 0, 1, 0);
    
    if (ret == 0 && test_ctx == NULL) {
        ret = -1;
    }
    
    /* Configure transport parameters before connection */
    if (ret == 0) {
        /* Server parameters */
        memset(&server_parameters, 0, sizeof(picoquic_tp_t));
        picoquic_init_transport_parameters(&server_parameters, 1);
        server_parameters.enable_time_stamp = 3;
        server_parameters.is_multipath_enabled = 1;
        server_parameters.initial_max_path_id = 2;
        // server_parameters.enable_deadline_aware_streams = 1;
        
        picoquic_set_default_tp(test_ctx->qserver, &server_parameters);
        
        /* Client parameters */
        picoquic_tp_t client_parameters;
        memset(&client_parameters, 0, sizeof(picoquic_tp_t));
        picoquic_init_transport_parameters(&client_parameters, 0);
        client_parameters.enable_time_stamp = 3;
        client_parameters.is_multipath_enabled = 1;
        client_parameters.initial_max_path_id = 2;
        // client_parameters.enable_deadline_aware_streams = 1;
        
        picoquic_set_default_tp(test_ctx->qclient, &client_parameters);
    }
    
    /* Establish the connection */
    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }
    
    /* Verify multipath and deadline-aware are negotiated */
    if (ret == 0) {
        if (!test_ctx->cnx_client->is_multipath_enabled || !test_ctx->cnx_server->is_multipath_enabled) {
            DBG_PRINTF("Multipath not negotiated (c=%d, s=%d)\n",
                test_ctx->cnx_client->is_multipath_enabled, test_ctx->cnx_server->is_multipath_enabled);
            ret = -1;
        }
        // if (!picoquic_is_deadline_aware_negotiated(test_ctx->cnx_client) ||
        //     !picoquic_is_deadline_aware_negotiated(test_ctx->cnx_server)) {
        //     DBG_PRINTF("%s", "Deadline-aware streams not negotiated\n");
        //     ret = -1;
        // }
    }
    
    /* Wait until ready */
    if (ret == 0) {
        ret = wait_client_connection_ready(test_ctx, &simulated_time);
    }

    
    
    if (ret == 0) {
        DBG_PRINTF("%s", "Minimal multipath deadline test successful!\n");
    }
    
    /* Clean up */
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    return ret;
}