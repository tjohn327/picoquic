/*
* End-to-end integration test for deadline-aware streams
* Tests the complete deadline functionality in realistic scenarios
*/

#include "../picoquic/picoquic_internal.h"
#include "../picoquic/tls_api.h"
#include "picoquictest_internal.h"
#include "../picoquic/picoquic.h"
#include "../picoquic/picoquic_utils.h"
#include <stdlib.h>
#include <string.h>

/* Static loss masks for E2E tests */
static uint64_t e2e_loss_mask_2percent = 0x0808080808080808ull;
static uint64_t e2e_loss_mask_1percent = 0x0404040404040404ull;

/* Test context for deadline E2E tests */
typedef struct st_deadline_e2e_ctx_t {
    picoquic_test_tls_api_ctx_t* tls_ctx;
    
    /* Stream tracking */
    uint64_t stream_id_deadline;
    uint64_t stream_id_regular;
    uint64_t stream_id_urgent;
    
    /* Data tracking */
    size_t deadline_bytes_sent;
    size_t deadline_bytes_received;
    size_t regular_bytes_sent;
    size_t regular_bytes_received;
    
    /* Deadline tracking */
    int deadline_misses;
    int gaps_received;
    uint64_t last_gap_offset;
    uint64_t last_gap_length;
    
    /* Test parameters */
    int enable_multipath;
    int enable_loss;
    int enable_congestion;
} deadline_e2e_ctx_t;

/* Callback for deadline test - tracks gaps and data */
static int deadline_e2e_callback(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes,
    size_t length, picoquic_call_back_event_t event, void* callback_ctx, void* stream_ctx)
{
    deadline_e2e_ctx_t* ctx = (deadline_e2e_ctx_t*)callback_ctx;
    
    switch (event) {
    case picoquic_callback_stream_data:
        if (stream_id == ctx->stream_id_deadline) {
            ctx->deadline_bytes_received += length;
        } else if (stream_id == ctx->stream_id_regular) {
            ctx->regular_bytes_received += length;
        }
        break;
        
    case picoquic_callback_stream_gap:
        ctx->gaps_received++;
        if (length >= sizeof(uint64_t)) {
            ctx->last_gap_length = *((uint64_t*)bytes);
            DBG_PRINTF("Gap received on stream %lu: %lu bytes\n", 
                (unsigned long)stream_id, (unsigned long)ctx->last_gap_length);
        }
        break;
        
    case picoquic_callback_stream_fin:
        DBG_PRINTF("Stream %lu finished\n", (unsigned long)stream_id);
        break;
        
    default:
        break;
    }
    
    return 0;
}

/* Initialize E2E test context */
static int deadline_e2e_init_ctx(deadline_e2e_ctx_t** p_ctx, uint64_t* simulated_time,
    int enable_multipath, int enable_loss, int enable_congestion)
{
    int ret = 0;
    deadline_e2e_ctx_t* ctx = (deadline_e2e_ctx_t*)malloc(sizeof(deadline_e2e_ctx_t));
    
    if (ctx == NULL) {
        return -1;
    }
    
    memset(ctx, 0, sizeof(deadline_e2e_ctx_t));
    ctx->enable_multipath = enable_multipath;
    ctx->enable_loss = enable_loss;
    ctx->enable_congestion = enable_congestion;
    
    /* Initialize TLS context */
    ret = tls_api_init_ctx(&ctx->tls_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, simulated_time, NULL, NULL, 0, 1, 0);
    
    if (ret == 0) {
        /* Enable deadline-aware streams on client */
        ctx->tls_ctx->cnx_client->local_parameters.enable_deadline_aware_streams = 1;
        
        /* Set server default transport parameters to enable deadline-aware streams */
        picoquic_tp_t server_tp;
        memset(&server_tp, 0, sizeof(picoquic_tp_t));
        picoquic_init_transport_parameters(&server_tp, 1); /* server mode */
        server_tp.enable_deadline_aware_streams = 1;
        picoquic_set_default_tp(ctx->tls_ctx->qserver, &server_tp);
        
        /* Set client callback */
        picoquic_set_callback(ctx->tls_ctx->cnx_client, deadline_e2e_callback, ctx);
        
        /* Server callback will be set after connection is established */
        
        /* Configure network conditions */
        if (enable_loss) {
            /* 2% loss rate */
            ctx->tls_ctx->c_to_s_link->loss_mask = &e2e_loss_mask_2percent;
            ctx->tls_ctx->s_to_c_link->loss_mask = &e2e_loss_mask_1percent;
        }
        
        if (enable_congestion) {
            /* Limited bandwidth - 1 Mbps */
            ctx->tls_ctx->c_to_s_link->picosec_per_byte = 8000; /* 1 Mbps */
            ctx->tls_ctx->s_to_c_link->picosec_per_byte = 8000;
            /* Add queue delay */
            ctx->tls_ctx->c_to_s_link->queue_delay_max = 50000; /* 50ms max queue */
        }
        
        if (enable_multipath) {
            /* Enable multipath on client connection */
            ctx->tls_ctx->cnx_client->is_multipath_enabled = 1;
            /* Server multipath will be enabled after connection establishment */
            
            /* Create second path with different characteristics */
            /* Path 2: Higher bandwidth but higher latency */
            ctx->tls_ctx->c_to_s_link_2 = picoquictest_sim_link_create(
                0.002,  /* 2 Mbps in Gbps */
                100000, /* 100ms latency */
                NULL, 0, *simulated_time);
            ctx->tls_ctx->s_to_c_link_2 = picoquictest_sim_link_create(
                0.002, 100000, NULL, 0, *simulated_time);
        }
        
        /* Start the client connection */
        ret = picoquic_start_client_cnx(ctx->tls_ctx->cnx_client);
        
        if (ret == 0) {
            *p_ctx = ctx;
        } else {
            free(ctx);
        }
    } else {
        free(ctx);
    }
    
    return ret;
}

/* Clean up E2E test context */
static void deadline_e2e_delete_ctx(deadline_e2e_ctx_t* ctx)
{
    if (ctx != NULL) {
        if (ctx->tls_ctx != NULL) {
            tls_api_delete_ctx(ctx->tls_ctx);
        }
        free(ctx);
    }
}

/* Test 1: Basic deadline stream with data dropping */
int deadline_e2e_basic_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    deadline_e2e_ctx_t* ctx = NULL;
    uint8_t buffer[10000];
    
    /* Initialize test context */
    ret = deadline_e2e_init_ctx(&ctx, &simulated_time, 0, 0, 0);
    
    if (ret == 0) {
        /* Establish connection */
        ret = tls_api_connection_loop(ctx->tls_ctx, NULL, 0, &simulated_time);
        
        if (ret == 0 && ctx->tls_ctx->cnx_server != NULL) {
            /* Set the server callback on the actual server connection */
            picoquic_set_callback(ctx->tls_ctx->cnx_server, deadline_e2e_callback, ctx);
        }
    }
    
    if (ret == 0) {
        /* Create streams */
        ctx->stream_id_deadline = 4;  /* Use stream 4 instead of 0 */
        ctx->stream_id_regular = 8;  /* Use stream 8 instead of 4 */
        
        /* Set deadline on stream 4 - 50ms hard deadline */
        ret = picoquic_set_stream_deadline(ctx->tls_ctx->cnx_client, 
            ctx->stream_id_deadline, 50, 1);
        
        /* Send data on both streams */
        memset(buffer, 0xDE, sizeof(buffer));
        ret = picoquic_add_to_stream(ctx->tls_ctx->cnx_client, 
            ctx->stream_id_deadline, buffer, sizeof(buffer), 0);
        ctx->deadline_bytes_sent = sizeof(buffer);
        
        memset(buffer, 0xAA, sizeof(buffer));
        ret |= picoquic_add_to_stream(ctx->tls_ctx->cnx_client,
            ctx->stream_id_regular, buffer, sizeof(buffer), 0);
        ctx->regular_bytes_sent = sizeof(buffer);
    }
    
    if (ret == 0) {
        /* Run data exchange with deadline checking */
        uint64_t deadline_time = simulated_time + 150000; /* 150ms total time */
        
        while (simulated_time < deadline_time && ret == 0) {
            int was_active = 0;
            ret = tls_api_one_sim_round(ctx->tls_ctx, &simulated_time, 10000, &was_active);
            
            /* Check if all data received */
            if (!was_active && ctx->regular_bytes_received == ctx->regular_bytes_sent) {
                break;
            }
        }
    }
    
    if (ret == 0) {
        /* Check results */
        if (ctx->gaps_received == 0) {
            DBG_PRINTF("%s", "Expected gaps from missed deadline\n");
            ret = -1;
        } else if (ctx->regular_bytes_received != ctx->regular_bytes_sent) {
            DBG_PRINTF("Regular stream incomplete: sent %zu, received %zu\n",
                ctx->regular_bytes_sent, ctx->regular_bytes_received);
            ret = -1;
        } else {
            DBG_PRINTF("Basic deadline test passed: %d gaps received\n", 
                ctx->gaps_received);
        }
    }
    
    /* Cleanup */
    deadline_e2e_delete_ctx(ctx);
    
    return ret;
}

/* Test 2: Multipath deadline routing */
int deadline_e2e_multipath_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    deadline_e2e_ctx_t* ctx = NULL;
    uint8_t buffer[5000];
    picoquic_path_t* selected_path;
    
    /* Initialize with multipath enabled */
    ret = deadline_e2e_init_ctx(&ctx, &simulated_time, 1, 0, 0);
    
    if (ret == 0) {
        /* Establish connection */
        ret = tls_api_connection_loop(ctx->tls_ctx, NULL, 0, &simulated_time);
        
        if (ret == 0 && ctx->tls_ctx->cnx_server != NULL) {
            /* Set the server callback on the actual server connection */
            picoquic_set_callback(ctx->tls_ctx->cnx_server, deadline_e2e_callback, ctx);
        }
    }
    
    if (ret == 0) {
        /* Wait for multipath to be established */
        simulated_time += 500000; /* 500ms */
        ret = tls_api_one_sim_round(ctx->tls_ctx, &simulated_time, 0, NULL);
        
        /* Create urgent deadline stream */
        ctx->stream_id_urgent = 4;  /* Use stream 4 instead of 0 */
        
        /* Set 20ms hard deadline - only fast path can meet this */
        ret = picoquic_set_stream_deadline(ctx->tls_ctx->cnx_client,
            ctx->stream_id_urgent, 20, 1);
        
        /* Create stream head for path selection test */
        picoquic_stream_head_t* stream = picoquic_find_stream(ctx->tls_ctx->cnx_client,
            ctx->stream_id_urgent);
        
        if (stream == NULL) {
            DBG_PRINTF("%s", "Failed to find stream\n");
            ret = -1;
        } else {
            /* Test path selection */
            selected_path = picoquic_select_path_for_deadline(ctx->tls_ctx->cnx_client,
                stream, simulated_time);
            
            if (selected_path == NULL) {
                DBG_PRINTF("%s", "No path selected for deadline stream\n");
                ret = -1;
            } else if (selected_path->smoothed_rtt > 50000) {
                DBG_PRINTF("Selected slow path (RTT=%lu) for urgent deadline\n",
                    (unsigned long)selected_path->smoothed_rtt);
                ret = -1;
            } else {
                DBG_PRINTF("%s", "Correctly selected fast path for urgent deadline\n");
            }
        }
    }
    
    if (ret == 0) {
        /* Send urgent data */
        memset(buffer, 0xFF, sizeof(buffer));
        ret = picoquic_add_to_stream(ctx->tls_ctx->cnx_client,
            ctx->stream_id_urgent, buffer, sizeof(buffer), 1);
        
        /* Exchange data */
        ret = tls_api_data_sending_loop(ctx->tls_ctx, &simulated_time, 0, 10000000);
    }
    
    /* Cleanup */
    deadline_e2e_delete_ctx(ctx);
    
    return ret;
}

/* Test 3: Fairness with mixed traffic under congestion */
int deadline_e2e_fairness_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    deadline_e2e_ctx_t* ctx = NULL;
    uint8_t buffer[1000];
    int i;
    size_t total_deadline_bytes = 0;
    size_t total_regular_bytes = 0;
    
    /* Initialize with congestion enabled */
    ret = deadline_e2e_init_ctx(&ctx, &simulated_time, 0, 0, 1);
    
    if (ret == 0) {
        /* Establish connection */
        ret = tls_api_connection_loop(ctx->tls_ctx, NULL, 0, &simulated_time);
        
        if (ret == 0 && ctx->tls_ctx->cnx_server != NULL) {
            /* Set the server callback on the actual server connection */
            picoquic_set_callback(ctx->tls_ctx->cnx_server, deadline_e2e_callback, ctx);
        }
    }
    
    if (ret == 0) {
        /* Configure fairness - 30% minimum for regular streams */
        picoquic_set_deadline_fairness_params(ctx->tls_ctx->cnx_client, 0.3, 100000);
    }
    
    if (ret == 0) {
        /* Create multiple streams */
        for (i = 0; i < 10 && ret == 0; i++) {
            uint64_t stream_id = (i + 1) * 4;  /* Start from stream 4, not 0 */
            
            if (i < 7) {
                /* 70% deadline streams with 100ms deadline */
                ret = picoquic_set_stream_deadline(ctx->tls_ctx->cnx_client,
                    stream_id, 100, 0); /* soft deadline */
                total_deadline_bytes += sizeof(buffer);
            } else {
                /* 30% regular streams */
                total_regular_bytes += sizeof(buffer);
            }
            
            /* Send data */
            memset(buffer, (uint8_t)i, sizeof(buffer));
            ret |= picoquic_add_to_stream(ctx->tls_ctx->cnx_client,
                stream_id, buffer, sizeof(buffer), 0);
        }
    }
    
    if (ret == 0) {
        /* Run under congestion for extended period */
        uint64_t start_time = simulated_time;
        uint64_t end_time = start_time + 5000000; /* 5 seconds */
        
        while (simulated_time < end_time && ret == 0) {
            ret = tls_api_one_sim_round(ctx->tls_ctx, &simulated_time, 20000, NULL);
        }
    }
    
    if (ret == 0) {
        /* Check fairness - regular streams should get reasonable throughput */
        picoquic_cnx_t* cnx = ctx->tls_ctx->cnx_client;
        if (cnx->deadline_context != NULL) {
            double deadline_ratio = (double)cnx->deadline_context->deadline_bytes_sent /
                (cnx->deadline_context->deadline_bytes_sent + 
                 cnx->deadline_context->non_deadline_bytes_sent);
            
            DBG_PRINTF("Deadline traffic ratio: %.2f\n", deadline_ratio);
            
            if (deadline_ratio > 0.75) {
                DBG_PRINTF("%s", "Regular streams starved - fairness not enforced\n");
                ret = -1;
            }
        }
    }
    
    /* Cleanup */
    deadline_e2e_delete_ctx(ctx);
    
    return ret;
}

/* Test 4: Smart retransmission under loss */
int deadline_e2e_retransmit_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    deadline_e2e_ctx_t* ctx = NULL;
    uint8_t buffer[5000];
    uint64_t deadline_ms = 80;
    
    /* Initialize with loss and multipath */
    ret = deadline_e2e_init_ctx(&ctx, &simulated_time, 1, 1, 0);
    
    if (ret == 0) {
        /* Establish connection */
        ret = tls_api_connection_loop(ctx->tls_ctx, NULL, 0, &simulated_time);
        
        if (ret == 0 && ctx->tls_ctx->cnx_server != NULL) {
            /* Set the server callback on the actual server connection */
            picoquic_set_callback(ctx->tls_ctx->cnx_server, deadline_e2e_callback, ctx);
        }
    }
    
    if (ret == 0) {
        /* Create deadline stream */
        ctx->stream_id_deadline = 4;  /* Use stream 4 instead of 0 */
        ret = picoquic_set_stream_deadline(ctx->tls_ctx->cnx_client,
            ctx->stream_id_deadline, deadline_ms, 1); /* hard deadline */
        
        /* Send data that will experience loss */
        memset(buffer, 0xBB, sizeof(buffer));
        ret = picoquic_add_to_stream(ctx->tls_ctx->cnx_client,
            ctx->stream_id_deadline, buffer, sizeof(buffer), 1);
        ctx->deadline_bytes_sent = sizeof(buffer);
    }
    
    if (ret == 0) {
        /* Exchange data with controlled timing */
        uint64_t deadline_time = simulated_time + (deadline_ms * 1000);
        int packets_lost = 0;
        int packets_retransmitted = 0;
        
        while (simulated_time < deadline_time + 50000 && ret == 0) {
            int was_active = 0;
            ret = tls_api_one_sim_round(ctx->tls_ctx, &simulated_time, 10000, &was_active);
            
            /* Track retransmissions */
            picoquic_cnx_t* cnx = ctx->tls_ctx->cnx_client;
            if (cnx->nb_retransmission_total > packets_retransmitted) {
                packets_retransmitted = (int)cnx->nb_retransmission_total;
                DBG_PRINTF("Retransmission at time %lu\n", (unsigned long)simulated_time);
            }
        }
        
        DBG_PRINTF("Total retransmissions: %d\n", packets_retransmitted);
    }
    
    if (ret == 0) {
        /* Verify data delivery or proper dropping */
        if (ctx->deadline_bytes_received == ctx->deadline_bytes_sent) {
            DBG_PRINTF("%s", "Data delivered successfully with smart retransmission\n");
        } else if (ctx->gaps_received > 0) {
            DBG_PRINTF("%s", "Deadline missed - data properly dropped\n");
        } else {
            DBG_PRINTF("Unexpected state: sent=%zu, received=%zu, gaps=%d\n",
                ctx->deadline_bytes_sent, ctx->deadline_bytes_received, ctx->gaps_received);
            ret = -1;
        }
    }
    
    /* Cleanup */
    deadline_e2e_delete_ctx(ctx);
    
    return ret;
}

/* Test 5: Stress test with many deadline streams */
int deadline_e2e_stress_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    deadline_e2e_ctx_t* ctx = NULL;
    uint8_t buffer[500];
    int i;
    int num_streams = 50;
    int deadlines_met = 0;
    int deadlines_missed = 0;
    
    /* Initialize with all features enabled */
    ret = deadline_e2e_init_ctx(&ctx, &simulated_time, 1, 1, 1);
    
    if (ret == 0) {
        /* Establish connection */
        ret = tls_api_connection_loop(ctx->tls_ctx, NULL, 0, &simulated_time);
        
        if (ret == 0 && ctx->tls_ctx->cnx_server != NULL) {
            /* Set the server callback on the actual server connection */
            picoquic_set_callback(ctx->tls_ctx->cnx_server, deadline_e2e_callback, ctx);
        }
    }
    
    if (ret == 0) {
        /* Create many streams with varying deadlines */
        for (i = 0; i < num_streams && ret == 0; i++) {
            uint64_t stream_id = (i + 1) * 4;  /* Start from stream 4, not 0 */
            uint64_t deadline_ms = 50 + (i % 100); /* 50-150ms deadlines */
            int is_hard = (i % 3) == 0; /* Every third stream has hard deadline */
            
            ret = picoquic_set_stream_deadline(ctx->tls_ctx->cnx_client,
                stream_id, deadline_ms, is_hard);
            
            /* Send varying amounts of data */
            size_t data_size = 100 + (i * 20);
            if (data_size > sizeof(buffer)) {
                data_size = sizeof(buffer);
            }
            
            memset(buffer, (uint8_t)i, data_size);
            ret |= picoquic_add_to_stream(ctx->tls_ctx->cnx_client,
                stream_id, buffer, data_size, 1);
        }
    }
    
    if (ret == 0) {
        /* Run simulation for extended period */
        uint64_t start_time = simulated_time;
        uint64_t run_duration = 10000000; /* 10 seconds */
        
        while (simulated_time < start_time + run_duration && ret == 0) {
            ret = tls_api_one_sim_round(ctx->tls_ctx, &simulated_time, 50000, NULL);
        }
    }
    
    if (ret == 0) {
        /* Analyze results */
        picoquic_cnx_t* cnx = ctx->tls_ctx->cnx_client;
        
        DBG_PRINTF("%s", "\nStress test results:\n");
        DBG_PRINTF("Gaps received: %d\n", ctx->gaps_received);
        
        if (cnx->deadline_context != NULL) {
            DBG_PRINTF("Deadline bytes sent: %lu\n",
                (unsigned long)cnx->deadline_context->deadline_bytes_sent);
            DBG_PRINTF("Non-deadline bytes sent: %lu\n",
                (unsigned long)cnx->deadline_context->non_deadline_bytes_sent);
        }
        
        /* Basic sanity check */
        if (ctx->gaps_received > num_streams / 2) {
            DBG_PRINTF("%s", "Too many gaps - possible issue with deadline handling\n");
            ret = -1;
        }
    }
    
    /* Cleanup */
    deadline_e2e_delete_ctx(ctx);
    
    return ret;
}

/* Master test function that runs all E2E tests */
int deadline_e2e_test()
{
    int ret = 0;
    
    DBG_PRINTF("%s", "\n=== Deadline E2E Test Suite ===\n");
    
    if (ret == 0) {
        DBG_PRINTF("%s", "\n--- Test 1: Basic deadline with dropping ---\n");
        ret = deadline_e2e_basic_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "Basic deadline test FAILED\n");
        }
    }
    
    if (ret == 0) {
        DBG_PRINTF("%s", "\n--- Test 2: Multipath deadline routing ---\n");
        ret = deadline_e2e_multipath_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "Multipath deadline test FAILED\n");
        }
    }
    
    if (ret == 0) {
        DBG_PRINTF("%s", "\n--- Test 3: Fairness under congestion ---\n");
        ret = deadline_e2e_fairness_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "Fairness test FAILED\n");
        }
    }
    
    if (ret == 0) {
        DBG_PRINTF("%s", "\n--- Test 4: Smart retransmission ---\n");
        ret = deadline_e2e_retransmit_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "Retransmission test FAILED\n");
        }
    }
    
    if (ret == 0) {
        DBG_PRINTF("%s", "\n--- Test 5: Stress test ---\n");
        ret = deadline_e2e_stress_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "Stress test FAILED\n");
        }
    }
    
    if (ret == 0) {
        DBG_PRINTF("%s", "\n=== All E2E tests PASSED ===\n");
    }
    
    return ret;
}