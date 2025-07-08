/*
* Simple runner for deadline evaluation tests
* 
* This program runs a subset of the evaluation tests with simpler scenarios
* to verify the evaluation framework works correctly.
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "picoquic.h"
#include "picoquic_internal.h"
#include "picoquictest_internal.h"
#include "tls_api.h"

/* Simple evaluation scenarios for testing */

/* Simple Video Scenario - 10 seconds of video */
static st_test_api_deadline_stream_desc_t simple_video_streams[] = {
    /* Video stream: 30fps, 100ms deadline, 10 seconds */
    { 2, st_stream_type_deadline, 300000, 1000, 33, 100, 300, 0 },
    /* Audio stream: 150ms deadline, 10 seconds */
    { 6, st_stream_type_deadline, 10000, 200, 20, 150, 500, 0 },
    /* File transfer: 100KB */
    { 10, st_stream_type_normal, 100000, 0, 0, 0, 0, 0 }
};

/* Simple Gaming Scenario - 5 seconds */
static st_test_api_deadline_stream_desc_t simple_gaming_streams[] = {
    /* Game state: 50ms deadline, 5 seconds at 60fps */
    { 2, st_stream_type_deadline, 300000, 1000, 16, 50, 300, 0 },
    /* Voice chat: 100ms deadline, 5 seconds */
    { 6, st_stream_type_deadline, 8000, 160, 20, 100, 250, 0 }
};

/* Run a simple evaluation test */
static int run_simple_evaluation(const char* test_name,
                                st_test_api_deadline_stream_desc_t* streams,
                                size_t num_streams,
                                int enable_deadline,
                                int enable_multipath,
                                FILE* output_file)
{
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    deadline_api_test_ctx_t* deadline_ctx = NULL;
    picoquic_connection_id_t initial_cid = { {0xde, 0xad, 0xbe, 0xef, 5, 6, 7, 8}, 8 };
    uint64_t loss_mask = 0;
    int ret = 0;
    
    /* Create test context */
    if (enable_multipath) {
        /* Use multipath initialization */
        ret = tls_api_init_ctx_ex2(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
            PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 1, 0, 
            &initial_cid, 8, 0, 0, 0);
    } else {
        ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN,
                              &simulated_time, NULL, NULL, 0, 1, 0);
    }
    
    if (ret == 0 && test_ctx == NULL) {
        ret = -1;
    }
    
    /* Configure protocol features */
    if (ret == 0) {
        if (enable_deadline) {
            test_ctx->cnx_client->local_parameters.enable_deadline_aware_streams = 1;
            test_ctx->qserver->default_tp.enable_deadline_aware_streams = 1;
        }
        
        if (enable_multipath) {
            test_ctx->cnx_client->local_parameters.is_multipath_enabled = 1;
            test_ctx->cnx_client->local_parameters.initial_max_path_id = 2;
            test_ctx->qserver->default_tp.is_multipath_enabled = 1;
            test_ctx->qserver->default_tp.initial_max_path_id = 2;
        }
    }
    
    /* Initialize deadline test context if needed */
    if (ret == 0 && enable_deadline) {
        ret = deadline_api_init_test_ctx(&deadline_ctx);
        if (ret == 0) {
            deadline_ctx->start_time = simulated_time;
            deadline_ctx->scenario = streams;
            deadline_ctx->nb_scenario = num_streams;
            g_deadline_ctx = deadline_ctx;
            
            deadline_ctx->client_callback.client_mode = 1;
            deadline_ctx->server_callback.client_mode = 0;
            
            picoquic_set_callback(test_ctx->cnx_client, deadline_api_callback, &deadline_ctx->client_callback);
            picoquic_set_default_callback(test_ctx->qserver, deadline_api_callback, &deadline_ctx->server_callback);
        }
    }
    
    /* Start connection */
    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }
    
    /* Wait for ready */
    if (ret == 0) {
        ret = wait_client_connection_ready(test_ctx, &simulated_time);
    }
    
    /* Add second path for multipath */
    if (ret == 0 && enable_multipath) {
        /* Initialize second address */
        test_ctx->client_addr_2 = test_ctx->client_addr;
        test_ctx->client_addr_2.sin_port += 17;
        
        /* Create second links - different characteristics */
        test_ctx->c_to_s_link_2 = picoquictest_sim_link_create(0.01, 10000, NULL, 40000, 0);
        test_ctx->s_to_c_link_2 = picoquictest_sim_link_create(0.01, 10000, NULL, 40000, 0);
        
        if (test_ctx->c_to_s_link_2 && test_ctx->s_to_c_link_2) {
            ret = picoquic_probe_new_path(test_ctx->cnx_client,
                (struct sockaddr*)&test_ctx->server_addr,
                (struct sockaddr*)&test_ctx->client_addr_2,
                simulated_time);
                
            /* Wait for path to be ready */
            if (ret == 0) {
                uint64_t timeout = simulated_time + 4000000;
                int nb_inactive = 0;
                
                while (simulated_time < timeout && ret == 0 && nb_inactive < 64 &&
                       test_ctx->cnx_client->nb_paths < 2) {
                    int was_active = 0;
                    ret = tls_api_one_sim_round(test_ctx, &simulated_time, timeout, &was_active);
                    nb_inactive = was_active ? 0 : nb_inactive + 1;
                }
            }
        }
    }
    
    /* Run the test */
    uint64_t test_start_time = simulated_time;
    
    if (ret == 0) {
        if (enable_deadline) {
            ret = deadline_api_data_sending_loop(test_ctx, deadline_ctx,
                                               streams, num_streams,
                                               &simulated_time);
        } else {
            /* Convert to regular stream test */
            test_api_stream_desc_t regular_streams[10];
            
            for (size_t i = 0; i < num_streams; i++) {
                regular_streams[i].stream_id = streams[i].stream_id;
                regular_streams[i].q_len = 0;
                regular_streams[i].r_len = 0;
                regular_streams[i].len = streams[i].len > 0 ? 
                    streams[i].len : 
                    streams[i].chunk_size * streams[i].num_chunks;
            }
            
            ret = test_api_init_send_recv_scenario(test_ctx, regular_streams, 
                num_streams * sizeof(test_api_stream_desc_t));
            if (ret == 0) {
                ret = tls_api_data_sending_loop(test_ctx, &loss_mask, &simulated_time, 0);
            }
        }
    }
    
    /* Calculate and report results */
    if (ret == 0) {
        double test_duration = (simulated_time - test_start_time) / 1000000.0;
        double total_bytes = 0;
        double compliance = 100.0;
        double avg_latency = 0;
        int deadline_streams = 0;
        
        /* Calculate metrics */
        if (enable_deadline && deadline_ctx != NULL) {
            for (int i = 0; i < deadline_ctx->nb_streams; i++) {
                int scenario_idx = -1;
                for (size_t j = 0; j < num_streams; j++) {
                    if (streams[j].stream_id == deadline_ctx->stream_state[i].stream_id) {
                        scenario_idx = j;
                        break;
                    }
                }
                
                if (scenario_idx >= 0) {
                    total_bytes += deadline_ctx->stream_state[i].bytes_sent;
                    
                    if (streams[scenario_idx].stream_type == st_stream_type_deadline) {
                        deadline_api_calculate_deadline_stats(deadline_ctx, i, streams, num_streams);
                        
                        if (deadline_ctx->deadline_stats[i] != NULL) {
                            compliance = deadline_ctx->deadline_stats[i]->deadline_compliance_percent;
                            avg_latency = deadline_ctx->deadline_stats[i]->avg_latency_ms;
                            deadline_streams++;
                            
                            /* Print per-stream results */
                            printf("  Stream %lu: compliance=%.1f%%, latency=%.1fms\n",
                                (unsigned long)deadline_ctx->stream_state[i].stream_id,
                                compliance, avg_latency);
                        }
                    }
                }
            }
        } else {
            /* Calculate bytes for non-deadline test */
            for (size_t i = 0; i < num_streams; i++) {
                total_bytes += streams[i].len > 0 ? streams[i].len :
                    streams[i].chunk_size * streams[i].num_chunks;
            }
        }
        
        double throughput = (total_bytes * 8.0) / (test_duration * 1000000.0);
        
        /* Print summary */
        printf("%s: duration=%.2fs, throughput=%.2f Mbps", test_name, test_duration, throughput);
        if (enable_deadline && deadline_streams > 0) {
            printf(", compliance=%.1f%%, latency=%.1fms", compliance, avg_latency);
        }
        printf("\n");
        
        /* Write to CSV file */
        if (output_file != NULL) {
            fprintf(output_file, "%ld,%s,%d,%d,%zu,%.2f,%.2f,%.1f,%.1f\n",
                time(NULL), test_name, enable_deadline, enable_multipath, 
                num_streams, test_duration, throughput, compliance, avg_latency);
        }
    } else {
        printf("%s: FAILED (ret=%d)\n", test_name, ret);
    }
    
    /* Clean up */
    if (test_ctx != NULL) {
        if (test_ctx->cnx_client != NULL) {
            picoquic_set_callback(test_ctx->cnx_client, NULL, NULL);
        }
        if (test_ctx->qserver != NULL) {
            picoquic_set_default_callback(test_ctx->qserver, NULL, NULL);
        }
    }
    
    g_deadline_ctx = NULL;
    if (deadline_ctx != NULL) {
        deadline_api_delete_test_ctx(deadline_ctx);
    }
    
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    return ret;
}

/* Main function for simple evaluation */
int run_deadline_evaluation_simple()
{
    FILE* output_file = NULL;
    int ret = 0;
    
    printf("\n========== SIMPLE DEADLINE EVALUATION ==========\n");
    printf("Running simplified evaluation tests\n\n");
    
    /* Open output file */
    output_file = fopen("deadline_simple_evaluation.csv", "w");
    if (output_file != NULL) {
        fprintf(output_file, "timestamp,test_name,deadline_enabled,multipath_enabled,"
                           "num_streams,duration_sec,throughput_mbps,compliance_pct,avg_latency_ms\n");
    }
    
    /* Test 1: Vanilla QUIC with video */
    printf("\n--- Test 1: Vanilla QUIC - Video ---\n");
    ret = run_simple_evaluation("VanillaQuic_Video", simple_video_streams, 3, 0, 0, output_file);
    
    /* Test 2: Deadline QUIC with video */
    printf("\n--- Test 2: Deadline QUIC - Video ---\n");
    ret = run_simple_evaluation("DeadlineQuic_Video", simple_video_streams, 3, 1, 0, output_file);
    
    /* Test 3: Vanilla QUIC with gaming */
    printf("\n--- Test 3: Vanilla QUIC - Gaming ---\n");
    ret = run_simple_evaluation("VanillaQuic_Gaming", simple_gaming_streams, 2, 0, 0, output_file);
    
    /* Test 4: Deadline QUIC with gaming */
    printf("\n--- Test 4: Deadline QUIC - Gaming ---\n");
    ret = run_simple_evaluation("DeadlineQuic_Gaming", simple_gaming_streams, 2, 1, 0, output_file);
    
    /* Test 5: Multipath Deadline QUIC with video */
    printf("\n--- Test 5: Multipath Deadline QUIC - Video ---\n");
    ret = run_simple_evaluation("MultipathDeadline_Video", simple_video_streams, 3, 1, 1, output_file);
    
    if (output_file != NULL) {
        fclose(output_file);
        printf("\nResults saved to: deadline_simple_evaluation.csv\n");
    }
    
    return ret;
}

/* Test entry point */
int deadline_simple_evaluation_test()
{
    return run_deadline_evaluation_simple();
}