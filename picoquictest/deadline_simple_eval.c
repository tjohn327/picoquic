/*
* Simple deadline evaluation test
* 
* This runs basic evaluation tests to compare deadline-aware vs regular QUIC
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "picoquic.h"
#include "picoquic_internal.h"
#include "picoquictest_internal.h"
#include "tls_api.h"

/* Simple test scenarios */
static st_test_api_deadline_stream_desc_t eval_video_streams[] = {
    /* Video: 30fps, 100ms deadline, 5 seconds = 150 frames */
    { 2, st_stream_type_deadline, 150000, 1000, 33, 100, 150, 0 },
    /* Audio: 150ms deadline, 5 seconds = 250 chunks */
    { 6, st_stream_type_deadline, 25000, 100, 20, 150, 250, 0 },
    /* File: 50KB */
    { 10, st_stream_type_normal, 50000, 0, 0, 0, 0, 0 }
};

static st_test_api_deadline_stream_desc_t eval_gaming_streams[] = {
    /* Game state: 60fps, 50ms deadline, 3 seconds = 180 frames */
    { 2, st_stream_type_deadline, 180000, 1000, 16, 50, 180, 0 },
    /* Voice: 100ms deadline, 3 seconds = 150 chunks */  
    { 6, st_stream_type_deadline, 15000, 100, 20, 100, 150, 0 }
};

/* Run one evaluation test */
static int run_simple_eval_test(const char* test_name,
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
    picoquic_tp_t server_parameters;
    uint64_t loss_mask = 0;
    st_test_api_deadline_stream_desc_t* test_streams = NULL;
    int ret = 0;
    
    DBG_PRINTF("Running %s...\n", test_name);
    
    /* Create context */
    if (enable_multipath) {
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
    
    /* Configure server */
    if (ret == 0) {
        memset(&server_parameters, 0, sizeof(picoquic_tp_t));
        picoquic_init_transport_parameters(&server_parameters, 1);
        
        if (enable_deadline) {
            server_parameters.enable_deadline_aware_streams = 1;
        }
        
        if (enable_multipath) {
            server_parameters.is_multipath_enabled = 1;
            server_parameters.initial_max_path_id = 2;
        }
        
        picoquic_set_default_tp(test_ctx->qserver, &server_parameters);
        
        /* Configure client */
        if (enable_deadline) {
            test_ctx->cnx_client->local_parameters.enable_deadline_aware_streams = 1;
        }
        
        if (enable_multipath) {
            test_ctx->cnx_client->local_parameters.is_multipath_enabled = 1;
            test_ctx->cnx_client->local_parameters.initial_max_path_id = 2;
        }
    }
    
    /* Initialize deadline context - always needed now for fair comparison */
    if (ret == 0) {
        deadline_ctx = (deadline_api_test_ctx_t*)calloc(1, sizeof(deadline_api_test_ctx_t));
        if (deadline_ctx == NULL) {
            ret = -1;
        } else {
            deadline_ctx->start_time = simulated_time;
            /* We'll set the scenario later, depending on enable_deadline */
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
        picoquic_start_client_cnx(test_ctx->cnx_client);
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }
    
    /* Wait for ready */
    if (ret == 0) {
        ret = wait_client_connection_ready(test_ctx, &simulated_time);
    }
    
    /* Add second path for multipath */
    if (ret == 0 && enable_multipath) {
        test_ctx->client_addr_2 = test_ctx->client_addr;
        test_ctx->client_addr_2.sin_port += 17;
        
        test_ctx->c_to_s_link_2 = picoquictest_sim_link_create(0.01, 10000, NULL, 40000, 0);
        test_ctx->s_to_c_link_2 = picoquictest_sim_link_create(0.01, 10000, NULL, 40000, 0);
        
        if (test_ctx->c_to_s_link_2 && test_ctx->s_to_c_link_2) {
            ret = picoquic_probe_new_path(test_ctx->cnx_client,
                (struct sockaddr*)&test_ctx->server_addr,
                (struct sockaddr*)&test_ctx->client_addr_2,
                simulated_time);
                
            if (ret == 0) {
                uint64_t timeout = simulated_time + 4000000;
                int nb_inactive = 0;
                
                while (simulated_time < timeout && ret == 0 && nb_inactive < 64 &&
                       (test_ctx->cnx_client->nb_paths < 2 ||
                        !test_ctx->cnx_client->path[1]->first_tuple->challenge_verified)) {
                    int was_active = 0;
                    ret = tls_api_one_sim_round(test_ctx, &simulated_time, timeout, &was_active);
                    nb_inactive = was_active ? 0 : nb_inactive + 1;
                }
            }
        }
    }
    
    /* Run test */
    uint64_t test_start = simulated_time;
    
    if (ret == 0) {
        /* For vanilla test, create a modified copy of streams with deadlines set to 0 */
        if (!enable_deadline) {
            /* Allocate memory for modified streams */
            test_streams = (st_test_api_deadline_stream_desc_t*)malloc(
                num_streams * sizeof(st_test_api_deadline_stream_desc_t));
            if (test_streams == NULL) {
                ret = -1;
            } else {
                /* Copy streams and modify deadlines for vanilla test */
                memcpy(test_streams, streams, num_streams * sizeof(st_test_api_deadline_stream_desc_t));
                for (size_t i = 0; i < num_streams; i++) {
                    /* Keep deadline type for paced sending, but set deadline to 0 */
                    if (test_streams[i].stream_type == st_stream_type_deadline) {
                        test_streams[i].deadline_ms = 0;  /* 0 = no deadline constraint */
                    }
                    /* Keep all other parameters the same for fair comparison */
                }
            }
        } else {
            /* For deadline test, use original streams */
            test_streams = streams;
        }
        
        /* Set the scenario pointer */
        if (ret == 0) {
            /* Keep the original scenario pointer, just pass different streams to the sending loop */
            deadline_ctx->scenario = streams;
            
            /* Use deadline API for both tests - fair comparison */
            ret = deadline_api_data_sending_loop(test_ctx, deadline_ctx,
                                               test_streams, num_streams,
                                               &simulated_time);
        }
    }
    
    /* Calculate results */
    if (ret == 0) {
        double duration = (simulated_time - test_start) / 1000000.0;
        double total_bytes = 0;
        double compliance = 100.0;
        double avg_latency = 0;
        int deadline_streams = 0;
        
        if (deadline_ctx != NULL) {
            /* Calculate metrics for both deadline and vanilla tests */
            for (int i = 0; i < deadline_ctx->nb_streams; i++) {
                total_bytes += deadline_ctx->stream_state[i].bytes_sent;
                
                if (enable_deadline) {
                    int scenario_idx = -1;
                    /* Use original streams array for checking stream types */
                    st_test_api_deadline_stream_desc_t* orig_streams = streams;
                    for (size_t j = 0; j < num_streams; j++) {
                        if (orig_streams[j].stream_id == deadline_ctx->stream_state[i].stream_id) {
                            scenario_idx = j;
                            break;
                        }
                    }
                    
                    if (scenario_idx >= 0 && orig_streams[scenario_idx].stream_type == st_stream_type_deadline) {
                        deadline_api_calculate_deadline_stats(deadline_ctx, i, orig_streams, num_streams);
                        
                        if (deadline_ctx->deadline_stats[i] != NULL) {
                            compliance = deadline_ctx->deadline_stats[i]->deadline_compliance_percent;
                            avg_latency = deadline_ctx->deadline_stats[i]->avg_latency_ms;
                            deadline_streams++;
                            
                            DBG_PRINTF("  Stream %lu: compliance=%.1f%%, latency=%.1fms\n",
                                (unsigned long)deadline_ctx->stream_state[i].stream_id,
                                compliance, avg_latency);
                        }
                    }
                }
            }
        }
        
        double throughput = (total_bytes * 8.0) / (duration * 1000000.0);
        
        DBG_PRINTF("  Duration: %.2fs, Throughput: %.2f Mbps", duration, throughput);
        if (enable_deadline && deadline_streams > 0) {
            DBG_PRINTF(", Compliance: %.1f%%, Latency: %.1fms", compliance, avg_latency);
        }
        DBG_PRINTF("%s", "\n");
        
        /* Write to CSV */
        if (output_file != NULL) {
            fprintf(output_file, "%ld,%s,%d,%d,%zu,%.2f,%.2f,%.1f,%.1f\n",
                time(NULL), test_name, enable_deadline, enable_multipath,
                num_streams, duration, throughput, compliance, avg_latency);
        }
    } else {
        DBG_PRINTF("  FAILED (ret=%d)\n", ret);
    }
    
    /* Clean up */
    /* Clear callbacks first to prevent use after free */
    if (test_ctx != NULL) {
        if (test_ctx->cnx_client != NULL) {
            picoquic_set_callback(test_ctx->cnx_client, NULL, NULL);
        }
        if (test_ctx->qserver != NULL) {
            picoquic_set_default_callback(test_ctx->qserver, NULL, NULL);
        }
    }
    
    /* Free allocated test_streams if we created them */
    if (!enable_deadline && test_streams != NULL) {
        free(test_streams);
    }
    
    /* Clean up test context (this will delete connections) */
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    /* Clear global pointer and clean up deadline context AFTER connections are gone */
    g_deadline_ctx = NULL;
    if (deadline_ctx != NULL) {
        deadline_api_delete_test_ctx(deadline_ctx);
    }
    
    return ret;
}

/* Main evaluation test */
int deadline_evaluation_test()
{
    FILE* output_file = NULL;
    int ret = 0;
    
    DBG_PRINTF("%s", "\n========== DEADLINE EVALUATION TEST ==========\n");
    DBG_PRINTF("%s", "Comparing deadline-aware vs regular QUIC\n\n");
    
    /* Open output file */
    output_file = fopen("deadline_evaluation_results.csv", "w");
    if (output_file != NULL) {
        fprintf(output_file, "timestamp,test_name,deadline_enabled,multipath_enabled,"
                           "num_streams,duration_sec,throughput_mbps,compliance_pct,avg_latency_ms\n");
    }
    
    /* Test 1: Vanilla QUIC - Video */
    ret = run_simple_eval_test("Vanilla_Video", eval_video_streams, 3, 0, 0, output_file);
    
    /* Test 2: Deadline QUIC - Video */
    if (ret == 0) {
        ret = run_simple_eval_test("Deadline_Video", eval_video_streams, 3, 1, 0, output_file);
    }
    
    /* Test 3: Vanilla QUIC - Gaming */
    if (ret == 0) {
        ret = run_simple_eval_test("Vanilla_Gaming", eval_gaming_streams, 2, 0, 0, output_file);
    }
    
    /* Test 4: Deadline QUIC - Gaming */
    if (ret == 0) {
        ret = run_simple_eval_test("Deadline_Gaming", eval_gaming_streams, 2, 1, 0, output_file);
    }
    
    /* Test 5: Multipath Deadline - Video */
    if (ret == 0) {
        ret = run_simple_eval_test("Multipath_Deadline_Video", eval_video_streams, 3, 1, 1, output_file);
    }
    
    /* Test 6: Multipath Deadline - Gaming */
    if (ret == 0) {
        ret = run_simple_eval_test("Multipath_Deadline_Gaming", eval_gaming_streams, 2, 1, 1, output_file);
    }
    
    if (output_file != NULL) {
        fclose(output_file);
        DBG_PRINTF("%s", "\nResults saved to: deadline_evaluation_results.csv\n");
    }
    
    DBG_PRINTF("\nEvaluation %s\n", ret == 0 ? "PASSED" : "FAILED");
    
    return ret;
}