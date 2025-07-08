/*
* Deadline Implementation Evaluation Framework
* 
* This program evaluates the deadline-aware stream implementation against
* various configurations defined in NOTES/eval_configurations.md
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "picoquic.h"
#include "picoquic_internal.h"
#include "picoquictest_internal.h"
#include "tls_api.h"
#include "picoquic_binlog.h"

/* Evaluation result structure */
typedef struct st_eval_result_t {
    char* test_name;
    char* protocol_variant;
    char* scenario_name;
    char* network_condition;
    int num_streams;
    int streams_with_deadlines;
    double overall_deadline_compliance;
    double avg_latency_ms;
    double avg_jitter_ms;
    double total_throughput_mbps;
    double time_to_completion_sec;
    int test_passed;
    /* Per-stream metrics */
    struct {
        uint64_t stream_id;
        int has_deadline;
        uint32_t deadline_ms;
        double deadline_compliance;
        double avg_latency;
        double p99_latency;
        double throughput_mbps;
        size_t bytes_transferred;
        int chunks_on_time;
        int total_chunks;
    } stream_metrics[10];
} eval_result_t;

/* Test scenario definitions based on eval_configurations.md */
typedef struct st_eval_scenario_t {
    const char* name;
    st_test_api_deadline_stream_desc_t streams[10];
    size_t num_streams;
} eval_scenario_t;

/* Video Conferencing Scenario */
static st_test_api_deadline_stream_desc_t video_conf_streams[] = {
    /* Video stream: 2Mbps, 30fps, 100ms deadline */
    { 2, st_stream_type_deadline, 250000, 8333, 33, 100, 30, 0 },
    /* Audio stream: 20ms chunks, 150ms deadline */
    { 6, st_stream_type_deadline, 8000, 320, 20, 150, 50, 0 },
    /* Screen share: 500ms deadline */
    { 10, st_stream_type_deadline, 100000, 10000, 100, 500, 10, 0 },
    /* File transfer: No deadline */
    { 14, st_stream_type_normal, 500000, 0, 0, 0, 0, 0 }
};

/* Live Streaming Scenario */
static st_test_api_deadline_stream_desc_t live_streaming_streams[] = {
    /* 4K video: 60fps, 200ms deadline */
    { 2, st_stream_type_deadline, 1000000, 16666, 16, 200, 60, 0 },
    /* Live audio: 150ms deadline */
    { 6, st_stream_type_deadline, 16000, 320, 20, 150, 50, 0 },
    /* Chat messages: 1000ms deadline */
    { 10, st_stream_type_deadline, 1000, 100, 1000, 1000, 1, 0 },
    /* Analytics: No deadline */
    { 14, st_stream_type_normal, 50000, 0, 0, 0, 0, 0 }
};

/* Gaming Scenario */
static st_test_api_deadline_stream_desc_t gaming_streams[] = {
    /* Game state: 50ms deadline */
    { 2, st_stream_type_deadline, 60000, 1000, 16, 50, 60, 0 },
    /* Voice chat: 100ms deadline */
    { 6, st_stream_type_deadline, 8000, 160, 20, 100, 50, 0 },
    /* Asset downloads: No deadline */
    { 10, st_stream_type_normal, 1000000, 0, 0, 0, 0, 0 },
    /* Telemetry: 500ms deadline */
    { 14, st_stream_type_deadline, 10000, 1000, 500, 500, 2, 0 }
};

/* IoT/Telemetry Scenario */
static st_test_api_deadline_stream_desc_t iot_streams[] = {
    /* Sensor data streams with 200ms deadlines */
    { 2, st_stream_type_deadline, 20000, 500, 50, 200, 40, 0 },
    { 6, st_stream_type_deadline, 20000, 500, 50, 200, 40, 0 },
    { 10, st_stream_type_deadline, 20000, 500, 50, 200, 40, 0 },
    { 14, st_stream_type_deadline, 20000, 500, 50, 200, 40, 0 },
    { 18, st_stream_type_deadline, 20000, 500, 50, 200, 40, 0 },
    /* Control commands with 100ms deadlines */
    { 22, st_stream_type_deadline, 5000, 250, 50, 100, 20, 0 },
    { 26, st_stream_type_deadline, 5000, 250, 50, 100, 20, 0 },
    { 30, st_stream_type_deadline, 5000, 250, 50, 100, 20, 0 },
    /* Bulk logs: No deadline */
    { 34, st_stream_type_normal, 100000, 0, 0, 0, 0, 0 },
    { 38, st_stream_type_normal, 100000, 0, 0, 0, 0, 0 }
};

/* Mixed Media Scenario */
static st_test_api_deadline_stream_desc_t mixed_media_streams[] = {
    /* Real-time video: 150ms deadline */
    { 2, st_stream_type_deadline, 100000, 4166, 33, 150, 30, 0 },
    /* Buffered video: 2000ms deadline */
    { 6, st_stream_type_deadline, 200000, 8333, 33, 2000, 30, 0 },
    /* Interactive data: 100ms deadline */
    { 10, st_stream_type_deadline, 10000, 500, 50, 100, 20, 0 },
    /* Background sync: No deadline */
    { 14, st_stream_type_normal, 250000, 0, 0, 0, 0, 0 },
    /* Metrics: 1000ms deadline */
    { 18, st_stream_type_deadline, 5000, 1000, 1000, 1000, 1, 0 }
};

static eval_scenario_t evaluation_scenarios[] = {
    { "VideoConferencing", video_conf_streams, 4 },
    { "LiveStreaming", live_streaming_streams, 4 },
    { "Gaming", gaming_streams, 4 },
    { "IoT_Telemetry", iot_streams, 10 },
    { "MixedMedia", mixed_media_streams, 5 }
};

/* Network condition structure */
typedef struct st_network_condition_t {
    const char* name;
    uint64_t bandwidth_bps;
    uint64_t rtt_us;
    double loss_rate;
    uint64_t jitter_us;
    int is_multipath;
    /* For multipath */
    uint64_t path2_bandwidth_bps;
    uint64_t path2_rtt_us;
    double path2_loss_rate;
    uint64_t path2_jitter_us;
} network_condition_t;

static network_condition_t network_conditions[] = {
    /* Single path conditions */
    { "SP1_Ideal", 100000000, 10000, 0.0, 0, 0, 0, 0, 0, 0 },
    { "SP2_Residential", 50000000, 30000, 0.001, 5000, 0, 0, 0, 0, 0 },
    { "SP3_CongestedWiFi", 20000000, 50000, 0.02, 20000, 0, 0, 0, 0, 0 },
    { "SP4_Cellular", 10000000, 100000, 0.01, 30000, 0, 0, 0, 0, 0 },
    { "SP5_Satellite", 25000000, 600000, 0.005, 10000, 0, 0, 0, 0, 0 },
    { "SP6_Lossy", 50000000, 50000, 0.05, 10000, 0, 0, 0, 0, 0 },
    { "SP7_Congested", 10000000, 300000, 0.03, 50000, 0, 0, 0, 0, 0 },
    /* Multipath conditions */
    { "MP1_WiFi_Cellular", 50000000, 30000, 0.01, 5000, 1, 10000000, 80000, 0.005, 10000 },
    { "MP2_DualWAN", 100000000, 20000, 0.001, 2000, 1, 50000000, 40000, 0.002, 5000 },
    { "MP3_Satellite_DSL", 25000000, 600000, 0.005, 10000, 1, 10000000, 50000, 0.001, 5000 },
    { "MP4_Asymmetric", 100000000, 200000, 0.001, 10000, 1, 10000000, 20000, 0.001, 2000 }
};

/* Output file for results */
static FILE* eval_output_file = NULL;

/* Write evaluation result to CSV file */
static void write_eval_result(eval_result_t* result)
{
    if (eval_output_file == NULL) {
        eval_output_file = fopen("deadline_evaluation_results.csv", "a");
        if (eval_output_file == NULL) {
            DBG_PRINTF("%s", "Failed to open evaluation results file\n");
            return;
        }
        
        /* Write header if file is empty */
        fseek(eval_output_file, 0, SEEK_END);
        if (ftell(eval_output_file) == 0) {
            fprintf(eval_output_file, 
                "timestamp,test_name,protocol,scenario,network,num_streams,deadline_streams,"
                "overall_compliance,avg_latency_ms,avg_jitter_ms,throughput_mbps,"
                "completion_time_sec,test_passed,"
                "stream_id,has_deadline,deadline_ms,stream_compliance,stream_avg_latency,"
                "stream_p99_latency,stream_throughput,bytes_transferred,chunks_on_time,total_chunks\n");
        }
    }
    
    time_t now = time(NULL);
    
    /* Write main test results */
    for (int i = 0; i < result->num_streams; i++) {
        fprintf(eval_output_file, 
            "%ld,%s,%s,%s,%s,%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%d,"
            "%lu,%d,%u,%.2f,%.2f,%.2f,%.2f,%zu,%d,%d\n",
            now,
            result->test_name,
            result->protocol_variant,
            result->scenario_name,
            result->network_condition,
            result->num_streams,
            result->streams_with_deadlines,
            result->overall_deadline_compliance,
            result->avg_latency_ms,
            result->avg_jitter_ms,
            result->total_throughput_mbps,
            result->time_to_completion_sec,
            result->test_passed,
            result->stream_metrics[i].stream_id,
            result->stream_metrics[i].has_deadline,
            result->stream_metrics[i].deadline_ms,
            result->stream_metrics[i].stream_compliance,
            result->stream_metrics[i].avg_latency,
            result->stream_metrics[i].p99_latency,
            result->stream_metrics[i].throughput_mbps,
            result->stream_metrics[i].bytes_transferred,
            result->stream_metrics[i].chunks_on_time,
            result->stream_metrics[i].total_chunks);
    }
    
    fflush(eval_output_file);
}

/* Run a single evaluation test */
static int run_evaluation_test(const char* protocol_variant,
                              eval_scenario_t* scenario,
                              network_condition_t* network,
                              eval_result_t* result)
{
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    deadline_api_test_ctx_t* deadline_ctx = NULL;
    uint64_t loss_mask = 0;
    int ret = 0;
    int enable_multipath = 0;
    int enable_deadline = 0;
    
    /* Set protocol features based on variant */
    if (strcmp(protocol_variant, "QUIC-Multipath") == 0) {
        enable_multipath = 1;
    } else if (strcmp(protocol_variant, "Deadline-QUIC") == 0) {
        enable_deadline = 1;
    } else if (strcmp(protocol_variant, "Deadline-Multipath") == 0) {
        enable_multipath = 1;
        enable_deadline = 1;
    }
    
    /* Skip multipath tests for single-path network conditions */
    if (enable_multipath && !network->is_multipath) {
        return -1;
    }
    
    /* Initialize result structure */
    memset(result, 0, sizeof(eval_result_t));
    result->test_name = "deadline_evaluation";
    result->protocol_variant = (char*)protocol_variant;
    result->scenario_name = scenario->name;
    result->network_condition = network->name;
    result->num_streams = scenario->num_streams;
    
    /* Create test context */
    ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN,
                          &simulated_time, NULL, NULL, 0, 1, 0);
    
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
    
    /* Set up network conditions */
    if (ret == 0) {
        /* Configure primary path */
        test_ctx->c_to_s_link->microsec_latency = network->rtt_us / 2;
        test_ctx->s_to_c_link->microsec_latency = network->rtt_us / 2;
        test_ctx->c_to_s_link->picosec_per_byte = (1000000000000ull * 8) / network->bandwidth_bps;
        test_ctx->s_to_c_link->picosec_per_byte = (1000000000000ull * 8) / network->bandwidth_bps;
        
        /* Simple loss model */
        if (network->loss_rate > 0) {
            loss_mask = (uint64_t)(1.0 / network->loss_rate);
        }
    }
    
    /* Initialize deadline test context */
    if (ret == 0 && enable_deadline) {
        ret = deadline_api_init_test_ctx(&deadline_ctx);
        if (ret == 0) {
            deadline_ctx->start_time = simulated_time;
            deadline_ctx->scenario = scenario->streams;
            deadline_ctx->nb_scenario = scenario->num_streams;
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
    
    /* Add second path for multipath tests */
    if (ret == 0 && enable_multipath && network->is_multipath) {
        /* Initialize second address */
        test_ctx->client_addr_2 = test_ctx->client_addr;
        test_ctx->client_addr_2.sin_port += 17;
        
        /* Create second links with different characteristics */
        test_ctx->c_to_s_link_2 = picoquictest_sim_link_create(
            network->path2_loss_rate, 
            network->path2_bandwidth_bps / 8000,
            NULL,
            network->path2_rtt_us / 2,
            0);
        test_ctx->s_to_c_link_2 = picoquictest_sim_link_create(
            network->path2_loss_rate,
            network->path2_bandwidth_bps / 8000,
            NULL,
            network->path2_rtt_us / 2,
            0);
        
        if (test_ctx->c_to_s_link_2 && test_ctx->s_to_c_link_2) {
            ret = picoquic_probe_new_path(test_ctx->cnx_client,
                (struct sockaddr*)&test_ctx->server_addr,
                (struct sockaddr*)&test_ctx->client_addr_2,
                simulated_time);
                
            /* Wait for path to be ready */
            if (ret == 0) {
                ret = wait_client_connection_ready(test_ctx, &simulated_time);
            }
        }
    }
    
    /* Run the test scenario */
    uint64_t test_start_time = simulated_time;
    
    if (ret == 0) {
        if (enable_deadline) {
            ret = deadline_api_data_sending_loop(test_ctx, deadline_ctx,
                                               scenario->streams, scenario->num_streams,
                                               &simulated_time);
        } else {
            /* Convert to regular stream test */
            test_api_stream_desc_t regular_streams[10];
            size_t nb_regular = 0;
            
            for (size_t i = 0; i < scenario->num_streams; i++) {
                if (scenario->streams[i].stream_type == st_stream_type_normal ||
                    !enable_deadline) {
                    regular_streams[nb_regular].stream_id = scenario->streams[i].stream_id;
                    regular_streams[nb_regular].q_len = 0;
                    regular_streams[nb_regular].r_len = 0;
                    regular_streams[nb_regular].len = scenario->streams[i].len > 0 ? 
                        scenario->streams[i].len : 
                        scenario->streams[i].chunk_size * scenario->streams[i].num_chunks;
                    nb_regular++;
                }
            }
            
            if (nb_regular > 0) {
                ret = test_api_init_send_recv_scenario(test_ctx, regular_streams, 
                    nb_regular * sizeof(test_api_stream_desc_t));
                if (ret == 0) {
                    ret = tls_api_data_sending_loop(test_ctx, &loss_mask, &simulated_time, 0);
                }
            }
        }
    }
    
    /* Calculate results */
    if (ret == 0) {
        result->time_to_completion_sec = (simulated_time - test_start_time) / 1000000.0;
        
        if (enable_deadline && deadline_ctx != NULL) {
            /* Calculate deadline-aware metrics */
            int total_deadline_streams = 0;
            double total_compliance = 0;
            double total_latency = 0;
            double total_jitter = 0;
            
            for (int i = 0; i < deadline_ctx->nb_streams; i++) {
                int stream_idx = i;
                int scenario_idx = -1;
                
                /* Find matching scenario */
                for (size_t j = 0; j < scenario->num_streams; j++) {
                    if (scenario->streams[j].stream_id == deadline_ctx->stream_state[i].stream_id) {
                        scenario_idx = j;
                        break;
                    }
                }
                
                if (scenario_idx >= 0) {
                    result->stream_metrics[i].stream_id = deadline_ctx->stream_state[i].stream_id;
                    result->stream_metrics[i].bytes_transferred = deadline_ctx->stream_state[i].bytes_sent;
                    
                    if (scenario->streams[scenario_idx].stream_type == st_stream_type_deadline) {
                        result->stream_metrics[i].has_deadline = 1;
                        result->stream_metrics[i].deadline_ms = scenario->streams[scenario_idx].deadline_ms;
                        result->streams_with_deadlines++;
                        
                        /* Calculate deadline compliance */
                        deadline_api_calculate_deadline_stats(deadline_ctx, i, 
                            scenario->streams, scenario->num_streams);
                        
                        if (deadline_ctx->deadline_stats[i] != NULL) {
                            result->stream_metrics[i].stream_compliance = 
                                deadline_ctx->deadline_stats[i]->deadline_compliance_percent;
                            result->stream_metrics[i].avg_latency = 
                                deadline_ctx->deadline_stats[i]->avg_latency_ms;
                            result->stream_metrics[i].p99_latency = 
                                deadline_ctx->deadline_stats[i]->max_latency_ms;
                            result->stream_metrics[i].chunks_on_time = 
                                deadline_ctx->deadline_stats[i]->chunks_on_time;
                            result->stream_metrics[i].total_chunks = 
                                deadline_ctx->deadline_stats[i]->total_chunks;
                            
                            total_compliance += result->stream_metrics[i].stream_compliance;
                            total_latency += result->stream_metrics[i].avg_latency;
                            total_jitter += deadline_ctx->deadline_stats[i]->latency_jitter_ms;
                            total_deadline_streams++;
                        }
                    } else {
                        /* Normal stream metrics */
                        deadline_api_calculate_normal_stats(deadline_ctx, i);
                        if (deadline_ctx->normal_stats[i] != NULL) {
                            result->stream_metrics[i].avg_latency = 
                                (deadline_ctx->normal_stats[i]->time_to_completion_ms - 
                                 deadline_ctx->normal_stats[i]->time_to_first_byte_ms) / 2.0;
                        }
                    }
                    
                    /* Calculate throughput */
                    if (result->time_to_completion_sec > 0) {
                        result->stream_metrics[i].throughput_mbps = 
                            (result->stream_metrics[i].bytes_transferred * 8.0) / 
                            (result->time_to_completion_sec * 1000000.0);
                        result->total_throughput_mbps += result->stream_metrics[i].throughput_mbps;
                    }
                }
            }
            
            /* Calculate averages */
            if (total_deadline_streams > 0) {
                result->overall_deadline_compliance = total_compliance / total_deadline_streams;
                result->avg_latency_ms = total_latency / total_deadline_streams;
                result->avg_jitter_ms = total_jitter / total_deadline_streams;
            }
        } else {
            /* Non-deadline metrics */
            for (size_t i = 0; i < scenario->num_streams; i++) {
                result->stream_metrics[i].stream_id = scenario->streams[i].stream_id;
                result->stream_metrics[i].has_deadline = 0;
                
                /* Estimate throughput */
                size_t expected_bytes = scenario->streams[i].len > 0 ? 
                    scenario->streams[i].len : 
                    scenario->streams[i].chunk_size * scenario->streams[i].num_chunks;
                    
                result->stream_metrics[i].bytes_transferred = expected_bytes;
                result->stream_metrics[i].throughput_mbps = 
                    (expected_bytes * 8.0) / (result->time_to_completion_sec * 1000000.0);
                result->total_throughput_mbps += result->stream_metrics[i].throughput_mbps;
            }
        }
        
        /* Determine test pass/fail */
        result->test_passed = 1;
        if (enable_deadline && result->overall_deadline_compliance < 95.0) {
            result->test_passed = 0;
        }
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

/* Main evaluation function */
int deadline_evaluation_main()
{
    int ret = 0;
    eval_result_t result;
    int total_tests = 0;
    int passed_tests = 0;
    
    DBG_PRINTF("%s", "\n========== DEADLINE IMPLEMENTATION EVALUATION ==========\n");
    DBG_PRINTF("%s", "Running comprehensive evaluation of deadline-aware streams\n\n");
    
    /* Test each protocol variant */
    const char* protocol_variants[] = {
        "Vanilla-QUIC",
        "QUIC-Multipath",
        "Deadline-QUIC",
        "Deadline-Multipath"
    };
    
    for (int p = 0; p < 4; p++) {
        DBG_PRINTF("\n=== Testing %s ===\n", protocol_variants[p]);
        
        /* Test each scenario */
        for (size_t s = 0; s < sizeof(evaluation_scenarios) / sizeof(eval_scenario_t); s++) {
            DBG_PRINTF("\nScenario: %s\n", evaluation_scenarios[s].name);
            
            /* Test each network condition */
            for (size_t n = 0; n < sizeof(network_conditions) / sizeof(network_condition_t); n++) {
                /* Skip multipath network conditions for single-path protocols */
                if ((strcmp(protocol_variants[p], "Vanilla-QUIC") == 0 ||
                     strcmp(protocol_variants[p], "Deadline-QUIC") == 0) &&
                    network_conditions[n].is_multipath) {
                    continue;
                }
                
                /* Skip single-path conditions for multipath protocols */
                if ((strcmp(protocol_variants[p], "QUIC-Multipath") == 0 ||
                     strcmp(protocol_variants[p], "Deadline-Multipath") == 0) &&
                    !network_conditions[n].is_multipath) {
                    continue;
                }
                
                DBG_PRINTF("  Network: %s... ", network_conditions[n].name);
                fflush(stdout);
                
                ret = run_evaluation_test(protocol_variants[p],
                                        &evaluation_scenarios[s],
                                        &network_conditions[n],
                                        &result);
                
                if (ret == 0) {
                    total_tests++;
                    if (result.test_passed) {
                        passed_tests++;
                        DBG_PRINTF("PASSED (compliance: %.1f%%, latency: %.1fms)\n",
                            result.overall_deadline_compliance,
                            result.avg_latency_ms);
                    } else {
                        DBG_PRINTF("FAILED (compliance: %.1f%%)\n",
                            result.overall_deadline_compliance);
                    }
                    
                    /* Write results to file */
                    write_eval_result(&result);
                } else {
                    DBG_PRINTF("SKIPPED\n");
                }
            }
        }
    }
    
    /* Summary */
    DBG_PRINTF("\n========== EVALUATION SUMMARY ==========\n");
    DBG_PRINTF("Total tests run: %d\n", total_tests);
    DBG_PRINTF("Tests passed: %d (%.1f%%)\n", passed_tests, 
        total_tests > 0 ? (100.0 * passed_tests / total_tests) : 0);
    DBG_PRINTF("Results saved to: deadline_evaluation_results.csv\n");
    
    /* Close output file */
    if (eval_output_file != NULL) {
        fclose(eval_output_file);
    }
    
    return (passed_tests == total_tests) ? 0 : -1;
}

/* Test entry point */
int deadline_evaluation_test()
{
    return deadline_evaluation_main();
}