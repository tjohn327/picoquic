/*

*/

#include "picoquic_internal.h"
#include "picoquic_utils.h"
#include "picosocks.h"
#include "tls_api.h"
#include "picoquictest_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>


/* Test scenarios - using unidirectional stream IDs (2, 6, 10, etc.) */
st_test_api_deadline_stream_desc_t test_scenario_single_deadline[] = {
    { 2, st_stream_type_deadline, 0, 1000, 100, 150, 10, 0 }  /* 10 chunks of 1KB every 100ms with 150ms deadline */
};

st_test_api_deadline_stream_desc_t test_scenario_mixed_streams[] = {
    { 2, st_stream_type_normal, 10000, 0, 0, 0, 0, 0 },      /* Normal stream: 10KB */
    { 6, st_stream_type_deadline, 0, 512, 50, 100, 20, 0 },  /* Deadline stream: 512B chunks every 50ms, 100ms deadline */
    { 10, st_stream_type_normal, 5000, 0, 0, 0, 0, 0 }       /* Normal stream: 5KB */
};

st_test_api_deadline_stream_desc_t test_scenario_multiple_deadlines[] = {
    { 2, st_stream_type_deadline, 0, 2048, 33, 100, 30, 0 },   /* Video-like: 2KB every 33ms (30fps), 100ms deadline */
    { 6, st_stream_type_deadline, 0, 256, 100, 200, 10, 0 },   /* Sensor: 256B every 100ms, 200ms deadline */
    { 10, st_stream_type_deadline, 0, 4096, 16, 50, 60, 0 }    /* Game: 4KB every 16ms (60fps), 50ms deadline */
};

/* Forward declaration removed - function is now non-static */

/* Initialize deadline test context */
static int deadline_api_init_test_ctx(deadline_api_test_ctx_t** p_test_ctx)
{
    deadline_api_test_ctx_t* test_ctx = (deadline_api_test_ctx_t*)malloc(sizeof(deadline_api_test_ctx_t));
    if (test_ctx == NULL) {
        return -1;
    }
    
    memset(test_ctx, 0, sizeof(deadline_api_test_ctx_t));
    *p_test_ctx = test_ctx;
    return 0;
}

/* Delete deadline test context */
void deadline_api_delete_test_ctx(deadline_api_test_ctx_t* test_ctx)
{
    if (test_ctx != NULL) {
        for (int i = 0; i < test_ctx->nb_streams; i++) {
            if (test_ctx->stream_state[i].send_buffer != NULL) {
                free(test_ctx->stream_state[i].send_buffer);
            }
            if (test_ctx->stream_state[i].recv_buffer != NULL) {
                free(test_ctx->stream_state[i].recv_buffer);
            }
            if (test_ctx->stream_state[i].chunk_send_times != NULL) {
                free(test_ctx->stream_state[i].chunk_send_times);
            }
        }
        /* Free server-side buffers */
        for (int i = 0; i < test_ctx->nb_server_streams; i++) {
            if (test_ctx->server_stream_state[i].recv_buffer != NULL) {
                free(test_ctx->server_stream_state[i].recv_buffer);
            }
            if (test_ctx->server_stream_state[i].chunk_receive_times != NULL) {
                free(test_ctx->server_stream_state[i].chunk_receive_times);
            }
            if (test_ctx->server_stream_state[i].chunk_sizes != NULL) {
                free(test_ctx->server_stream_state[i].chunk_sizes);
            }
        }
        /* Free statistics */
        for (int i = 0; i < 32; i++) {
            if (test_ctx->normal_stats[i] != NULL) {
                free(test_ctx->normal_stats[i]);
            }
            if (test_ctx->deadline_stats[i] != NULL) {
                if (test_ctx->deadline_stats[i]->chunk_stats != NULL) {
                    free(test_ctx->deadline_stats[i]->chunk_stats);
                }
                free(test_ctx->deadline_stats[i]);
            }
        }
        free(test_ctx);
    }
}

/* Initialize connection context with deadline-aware streams enabled */
int deadline_api_init_ctx(picoquic_test_tls_api_ctx_t** p_test_ctx, 
                         uint64_t* simulated_time,
                         deadline_api_test_ctx_t** p_deadline_ctx)
{
    int ret = tls_api_init_ctx(p_test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, 
                              simulated_time, NULL, NULL, 0, 1, 0);
    
    if (ret == 0) {
        /* Enable deadline-aware streams on both client and server */
        (*p_test_ctx)->cnx_client->local_parameters.enable_deadline_aware_streams = 1;
        (*p_test_ctx)->qserver->default_tp.enable_deadline_aware_streams = 1;
        
        /* Initialize deadline test context */
        ret = deadline_api_init_test_ctx(p_deadline_ctx);
        if (ret == 0) {
            (*p_deadline_ctx)->start_time = *simulated_time;
            
            /* Set up callbacks */
            (*p_deadline_ctx)->client_callback.client_mode = 1;
            (*p_deadline_ctx)->server_callback.client_mode = 0;
            
            /* Replace the test callbacks with our deadline callbacks */
            picoquic_set_callback((*p_test_ctx)->cnx_client, deadline_api_callback, &(*p_deadline_ctx)->client_callback);
            picoquic_set_default_callback((*p_test_ctx)->qserver, deadline_api_callback, &(*p_deadline_ctx)->server_callback);
        }
    }
    
    return ret;
}

/* Global deadline context for the test - simplifying approach */
deadline_api_test_ctx_t* g_deadline_ctx = NULL;

/* Callback to handle received data */
int deadline_api_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* stream_ctx)
{
    int ret = 0;
    test_api_callback_t* cb_ctx = (test_api_callback_t*)callback_ctx;
    deadline_api_test_ctx_t* deadline_ctx = g_deadline_ctx;
    
    if (deadline_ctx == NULL) {
        cb_ctx->error_detected |= test_api_fail_data_on_unknown_stream;
        return -1;
    }
    
    switch (fin_or_event) {
    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin:
    {
        /* Server side data tracking */
        if (!cb_ctx->client_mode) {
            /* Find or create server stream tracking */
            int server_stream_idx = -1;
            for (int i = 0; i < deadline_ctx->nb_server_streams; i++) {
                if (deadline_ctx->server_stream_state[i].stream_id == stream_id) {
                    server_stream_idx = i;
                    break;
                }
            }
            
            if (server_stream_idx < 0 && deadline_ctx->nb_server_streams < 32) {
                /* Create new server stream tracking */
                server_stream_idx = deadline_ctx->nb_server_streams++;
                deadline_ctx->server_stream_state[server_stream_idx].stream_id = stream_id;
                deadline_ctx->server_stream_state[server_stream_idx].bytes_received = 0;
                deadline_ctx->server_stream_state[server_stream_idx].expected_bytes = 0;
                deadline_ctx->server_stream_state[server_stream_idx].recv_buffer = NULL;
                deadline_ctx->server_stream_state[server_stream_idx].fin_received = 0;
                deadline_ctx->server_stream_state[server_stream_idx].first_byte_time = 0;
                deadline_ctx->server_stream_state[server_stream_idx].last_byte_time = 0;
                deadline_ctx->server_stream_state[server_stream_idx].chunks_received = 0;
                deadline_ctx->server_stream_state[server_stream_idx].chunk_receive_times = NULL;
                deadline_ctx->server_stream_state[server_stream_idx].chunk_sizes = NULL;
                
                /* Calculate expected bytes from scenario and setup tracking */
                size_t expected = 0;
                if (deadline_ctx->scenario != NULL) {
                    for (size_t i = 0; i < deadline_ctx->nb_scenario; i++) {
                        if (deadline_ctx->scenario[i].stream_id == stream_id) {
                            deadline_ctx->server_stream_state[server_stream_idx].stream_type = 
                                deadline_ctx->scenario[i].stream_type;
                            
                            if (deadline_ctx->scenario[i].stream_type == st_stream_type_deadline) {
                                expected = deadline_ctx->scenario[i].chunk_size * 
                                          deadline_ctx->scenario[i].num_chunks;
                                deadline_ctx->server_stream_state[server_stream_idx].num_expected_chunks = 
                                    deadline_ctx->scenario[i].num_chunks;
                                deadline_ctx->server_stream_state[server_stream_idx].deadline_ms = 
                                    deadline_ctx->scenario[i].deadline_ms;
                                    
                                /* Allocate chunk tracking arrays */
                                deadline_ctx->server_stream_state[server_stream_idx].chunk_receive_times = 
                                    (uint64_t*)calloc(deadline_ctx->scenario[i].num_chunks, sizeof(uint64_t));
                                deadline_ctx->server_stream_state[server_stream_idx].chunk_sizes = 
                                    (size_t*)calloc(deadline_ctx->scenario[i].num_chunks, sizeof(size_t));
                            } else {
                                expected = deadline_ctx->scenario[i].len;
                                deadline_ctx->server_stream_state[server_stream_idx].num_expected_chunks = 0;
                            }
                            break;
                        }
                    }
                }
                deadline_ctx->server_stream_state[server_stream_idx].expected_bytes = expected;
                /* Allocate receive buffer */
                if (expected > 0) {
                    deadline_ctx->server_stream_state[server_stream_idx].recv_buffer = 
                        (uint8_t*)malloc(expected);
                    if (deadline_ctx->server_stream_state[server_stream_idx].recv_buffer == NULL) {
                        cb_ctx->error_detected |= test_api_fail_data_on_unknown_stream;
                        return -1;
                    }
                }
            }
            
            if (server_stream_idx < 0) {
                DBG_PRINTF("Server: No tracking slot available for stream %lu\n", 
                    (unsigned long)stream_id);
                cb_ctx->error_detected |= test_api_fail_data_on_unknown_stream;
                ret = -1;
            } else if (length > 0) {
                uint64_t current_time = picoquic_get_quic_time(cnx->quic);
                
                /* Track first byte time */
                if (deadline_ctx->server_stream_state[server_stream_idx].first_byte_time == 0) {
                    deadline_ctx->server_stream_state[server_stream_idx].first_byte_time = current_time;
                }
                deadline_ctx->server_stream_state[server_stream_idx].last_byte_time = current_time;
                
                /* Store received data */
                size_t offset = deadline_ctx->server_stream_state[server_stream_idx].bytes_received;
                if (deadline_ctx->server_stream_state[server_stream_idx].recv_buffer != NULL &&
                    offset + length <= deadline_ctx->server_stream_state[server_stream_idx].expected_bytes) {
                    memcpy(deadline_ctx->server_stream_state[server_stream_idx].recv_buffer + offset,
                           bytes, length);
                }
                
                /* Track chunks for deadline streams */
                if (deadline_ctx->server_stream_state[server_stream_idx].stream_type == st_stream_type_deadline &&
                    deadline_ctx->server_stream_state[server_stream_idx].chunk_receive_times != NULL) {
                    /* Only count complete chunks based on expected chunk size */
                    int scenario_idx = -1;
                    if (deadline_ctx->scenario != NULL) {
                        for (size_t i = 0; i < deadline_ctx->nb_scenario; i++) {
                            if (deadline_ctx->scenario[i].stream_id == stream_id) {
                                scenario_idx = i;
                                break;
                            }
                        }
                    }
                    
                    if (scenario_idx >= 0) {
                        size_t expected_chunk_size = deadline_ctx->scenario[scenario_idx].chunk_size;
                        size_t prev_chunks = deadline_ctx->server_stream_state[server_stream_idx].bytes_received / expected_chunk_size;
                        size_t new_chunks = (deadline_ctx->server_stream_state[server_stream_idx].bytes_received + length) / expected_chunk_size;
                        
                        /* Record time for each newly completed chunk */
                        for (size_t i = prev_chunks; i < new_chunks && i < deadline_ctx->server_stream_state[server_stream_idx].num_expected_chunks; i++) {
                            deadline_ctx->server_stream_state[server_stream_idx].chunk_receive_times[i] = current_time;
                            deadline_ctx->server_stream_state[server_stream_idx].chunk_sizes[i] = expected_chunk_size;
                            deadline_ctx->server_stream_state[server_stream_idx].chunks_received++;
                        }
                    }
                }
                
                deadline_ctx->server_stream_state[server_stream_idx].bytes_received += length;
                
                DBG_PRINTF("Server received %zu bytes on stream %lu at time %lu, total %zu/%zu\n",
                    length, (unsigned long)stream_id, (unsigned long)current_time,
                    deadline_ctx->server_stream_state[server_stream_idx].bytes_received,
                    deadline_ctx->server_stream_state[server_stream_idx].expected_bytes);
            }
            
            if (fin_or_event == picoquic_callback_stream_fin) {
                deadline_ctx->server_stream_state[server_stream_idx].fin_received = 1;
                DBG_PRINTF("Server: Stream %lu finished with %zu bytes\n", 
                    (unsigned long)stream_id,
                    deadline_ctx->server_stream_state[server_stream_idx].bytes_received);
            }
            break;
        }
        
        /* Find the stream in our tracking (client side only) */
        int stream_idx = -1;
        for (int i = 0; i < deadline_ctx->nb_streams; i++) {
            if (deadline_ctx->stream_state[i].stream_id == stream_id) {
                stream_idx = i;
                break;
            }
        }
        
        if (stream_idx < 0) {
            DBG_PRINTF("Data received on unknown stream %lu\n", (unsigned long)stream_id);
            cb_ctx->error_detected |= test_api_fail_data_on_unknown_stream;
            ret = -1;
        } else if (length > 0) {
            /* Allocate receive buffer if needed */
            if (deadline_ctx->stream_state[stream_idx].recv_buffer == NULL) {
                /* Get expected total size from the send buffer */
                size_t total_size = 0;
                if (deadline_ctx->stream_state[stream_idx].send_buffer != NULL) {
                    /* For server side, we need to find the matching stream on client side */
                    for (int i = 0; i < deadline_ctx->nb_streams; i++) {
                        if (deadline_ctx->stream_state[i].stream_id == stream_id) {
                            /* Calculate expected size based on stream type */
                            if (i < 3) { /* Max scenarios */
                                total_size = 100000; /* Allocate max 100KB for safety */
                            }
                            break;
                        }
                    }
                } else {
                    /* This is server side receiving - allocate a reasonable buffer */
                    total_size = 100000; /* 100KB should be enough for our tests */
                }
                
                if (total_size > 0) {
                    deadline_ctx->stream_state[stream_idx].recv_buffer = (uint8_t*)malloc(total_size);
                    if (deadline_ctx->stream_state[stream_idx].recv_buffer == NULL) {
                        cb_ctx->error_detected |= test_api_fail_data_on_unknown_stream;
                        return -1;
                    }
                }
            }
            
            /* Copy received data */
            if (deadline_ctx->stream_state[stream_idx].recv_buffer != NULL) {
                /* Make sure we don't overflow the buffer */
                size_t buffer_size = 100000; /* Same as allocation above */
                if (deadline_ctx->stream_state[stream_idx].bytes_received + length <= buffer_size) {
                    memcpy(deadline_ctx->stream_state[stream_idx].recv_buffer + 
                           deadline_ctx->stream_state[stream_idx].bytes_received,
                           bytes, length);
                } else {
                    DBG_PRINTF("Buffer overflow prevented: received %zu + %zu > %zu\n",
                        deadline_ctx->stream_state[stream_idx].bytes_received, length, buffer_size);
                }
            }
            
            deadline_ctx->stream_state[stream_idx].bytes_received += length;
            
            DBG_PRINTF("Client received %zu bytes on stream %lu, total %zu\n",
                length, (unsigned long)stream_id,
                deadline_ctx->stream_state[stream_idx].bytes_received);
        }
        
        if (fin_or_event == picoquic_callback_stream_fin) {
            DBG_PRINTF("Client: Stream %lu finished, received %zu bytes\n",
                (unsigned long)stream_id,
                deadline_ctx->stream_state[stream_idx].bytes_received);
        }
        break;
    }
    case picoquic_callback_prepare_to_send:
        /* Nothing special to do for sending in this test */
        break;
    case picoquic_callback_almost_ready:
    case picoquic_callback_ready:
        /* Connection ready */
        break;
    case picoquic_callback_close:
    case picoquic_callback_application_close:
        /* Connection closed */
        break;
    default:
        /* Unexpected event */
        break;
    }
    
    return ret;
}

/* Initialize and queue initial data for deadline streams */
int deadline_api_init_streams(picoquic_test_tls_api_ctx_t* test_ctx,
                                    deadline_api_test_ctx_t* deadline_ctx,
                                    st_test_api_deadline_stream_desc_t* scenario,
                                    size_t nb_scenario,
                                    uint64_t* simulated_time)
{
    int ret = 0;
    
    /* Initialize streams based on scenario */
    for (size_t i = 0; i < nb_scenario && ret == 0; i++) {
        uint64_t stream_id = scenario[i].stream_id;
        
        if (scenario[i].stream_type == st_stream_type_deadline) {
            /* For deadline streams, create the stream and set deadline */
            picoquic_stream_head_t* stream = picoquic_create_stream(test_ctx->cnx_client, stream_id);
            if (stream == NULL) {
                DBG_PRINTF("Failed to create stream %lu\n", (unsigned long)stream_id);
                ret = -1;
                break;
            }
            
            /* Set deadline parameters on the stream */
            stream->deadline_ms = scenario[i].deadline_ms;
            
            DBG_PRINTF("Set deadline on stream %zu with ID %lu, deadline_ms=%lu\n", 
                i, (unsigned long)stream_id, (unsigned long)scenario[i].deadline_ms);
        } else {
            /* For normal streams, just create it */
            picoquic_stream_head_t* stream = picoquic_create_stream(test_ctx->cnx_client, stream_id);
            if (stream == NULL) {
                DBG_PRINTF("Failed to create stream %lu\n", (unsigned long)stream_id);
                ret = -1;
                break;
            }
            DBG_PRINTF("Created normal stream %zu with ID %lu\n", i, (unsigned long)stream_id);
        }
        
        /* Track stream state */
        int idx = deadline_ctx->nb_streams++;
        deadline_ctx->stream_state[idx].stream_id = stream_id;
        deadline_ctx->stream_state[idx].chunks_sent = 0;
        deadline_ctx->stream_state[idx].next_send_time = *simulated_time;
        deadline_ctx->stream_state[idx].bytes_sent = 0;
        deadline_ctx->stream_state[idx].bytes_received = 0;
        deadline_ctx->stream_state[idx].stream_created_time = *simulated_time;
        deadline_ctx->stream_state[idx].first_send_time = 0;
        deadline_ctx->stream_state[idx].last_send_time = 0;
        
        /* Allocate buffers */
        size_t total_size = (scenario[i].stream_type == st_stream_type_deadline) ?
            scenario[i].chunk_size * scenario[i].num_chunks : scenario[i].len;
        
        deadline_ctx->stream_state[idx].send_buffer = (uint8_t*)malloc(total_size);
        deadline_ctx->stream_state[idx].recv_buffer = NULL;
        
        /* Allocate chunk send times for deadline streams */
        if (scenario[i].stream_type == st_stream_type_deadline) {
            deadline_ctx->stream_state[idx].chunk_send_times = 
                (uint64_t*)calloc(scenario[i].num_chunks, sizeof(uint64_t));
        } else {
            deadline_ctx->stream_state[idx].chunk_send_times = NULL;
        }
        
        if (deadline_ctx->stream_state[idx].send_buffer == NULL) {
            ret = -1;
            break;
        }
        
        /* Initialize send buffer with test pattern */
        for (size_t j = 0; j < total_size; j++) {
            deadline_ctx->stream_state[idx].send_buffer[j] = (uint8_t)(j & 0xFF);
        }
    }
    
    return ret;
}

/* Deadline stream data sending loop using proper test pattern */
int deadline_api_data_sending_loop(picoquic_test_tls_api_ctx_t* test_ctx,
                                         deadline_api_test_ctx_t* deadline_ctx,
                                         st_test_api_deadline_stream_desc_t* scenario,
                                         size_t nb_scenario,
                                         uint64_t* simulated_time)
{
    /* Store scenario info for server callback */
    deadline_ctx->scenario = scenario;
    deadline_ctx->nb_scenario = nb_scenario;
    int ret = 0;
    int nb_trials = 0;
    int nb_inactive = 0;
    int max_trials = 100000;
    uint64_t next_wake_time = UINT64_MAX;
    
    /* First, initialize all streams */
    ret = deadline_api_init_streams(test_ctx, deadline_ctx, scenario, nb_scenario, simulated_time);
    
    if (ret != 0) {
        return ret;
    }
    
    /* Mark that we have work to do */
    test_ctx->test_finished = 0;
    
    /* Main loop following tls_api_data_sending_loop pattern */
    while (ret == 0 && nb_trials < max_trials && nb_inactive < 256 && 
           TEST_CLIENT_READY && TEST_SERVER_READY) {
        int was_active = 0;
        nb_trials++;
        
        /* Find the next wake time */
        next_wake_time = UINT64_MAX;
        for (int i = 0; i < deadline_ctx->nb_streams; i++) {
            size_t scenario_idx = i;
            if (scenario_idx >= nb_scenario) continue;
            
            if (scenario[scenario_idx].stream_type == st_stream_type_deadline &&
                deadline_ctx->stream_state[i].chunks_sent < scenario[scenario_idx].num_chunks &&
                deadline_ctx->stream_state[i].next_send_time < next_wake_time) {
                next_wake_time = deadline_ctx->stream_state[i].next_send_time;
            }
        }
        
        /* Check which streams need to send data */
        for (int i = 0; i < deadline_ctx->nb_streams && ret == 0; i++) {
            size_t scenario_idx = i;
            if (scenario_idx >= nb_scenario) continue;
            
            if (*simulated_time >= deadline_ctx->stream_state[i].next_send_time) {
                if (scenario[scenario_idx].stream_type == st_stream_type_deadline) {
                    /* Send next chunk for deadline stream */
                    if (deadline_ctx->stream_state[i].chunks_sent < scenario[scenario_idx].num_chunks) {
                        size_t offset = deadline_ctx->stream_state[i].chunks_sent * scenario[scenario_idx].chunk_size;
                        int is_fin = (deadline_ctx->stream_state[i].chunks_sent == scenario[scenario_idx].num_chunks - 1);
                        
                        ret = picoquic_add_to_stream(test_ctx->cnx_client,
                                                    deadline_ctx->stream_state[i].stream_id,
                                                    deadline_ctx->stream_state[i].send_buffer + offset,
                                                    scenario[scenario_idx].chunk_size,
                                                    is_fin);
                        
                        if (ret == 0) {
                            /* Track send time */
                            if (deadline_ctx->stream_state[i].first_send_time == 0) {
                                deadline_ctx->stream_state[i].first_send_time = *simulated_time;
                            }
                            deadline_ctx->stream_state[i].last_send_time = *simulated_time;
                            
                            /* Track chunk send time */
                            if (deadline_ctx->stream_state[i].chunk_send_times != NULL) {
                                int chunk_idx = deadline_ctx->stream_state[i].chunks_sent;
                                deadline_ctx->stream_state[i].chunk_send_times[chunk_idx] = *simulated_time;
                            }
                            
                            deadline_ctx->stream_state[i].chunks_sent++;
                            deadline_ctx->stream_state[i].bytes_sent += scenario[scenario_idx].chunk_size;
                            deadline_ctx->stream_state[i].next_send_time = *simulated_time + 
                                (scenario[scenario_idx].interval_ms * 1000);
                            was_active = 1;
                            
                            DBG_PRINTF("Stream %lu: Sent chunk %d/%d at time %lu, next send at %lu\n",
                                (unsigned long)deadline_ctx->stream_state[i].stream_id,
                                deadline_ctx->stream_state[i].chunks_sent,
                                scenario[scenario_idx].num_chunks,
                                (unsigned long)*simulated_time,
                                (unsigned long)deadline_ctx->stream_state[i].next_send_time);
                        } else {
                            DBG_PRINTF("Failed to add to stream %lu, ret=%d\n", 
                                (unsigned long)deadline_ctx->stream_state[i].stream_id, ret);
                        }
                    }
                } else {
                    /* Send all data at once for normal stream */
                    if (deadline_ctx->stream_state[i].bytes_sent == 0) {
                        ret = picoquic_add_to_stream(test_ctx->cnx_client,
                                                    deadline_ctx->stream_state[i].stream_id,
                                                    deadline_ctx->stream_state[i].send_buffer,
                                                    scenario[scenario_idx].len,
                                                    1); /* Set FIN */
                        
                        if (ret == 0) {
                            /* Track send time */
                            if (deadline_ctx->stream_state[i].first_send_time == 0) {
                                deadline_ctx->stream_state[i].first_send_time = *simulated_time;
                            }
                            deadline_ctx->stream_state[i].last_send_time = *simulated_time;
                            deadline_ctx->stream_state[i].bytes_sent = scenario[scenario_idx].len;
                            was_active = 1;
                        }
                    }
                }
            }
        }
        
        /* Run one simulation round with next wake time */
        ret = tls_api_one_sim_round(test_ctx, simulated_time, next_wake_time, &was_active);
        
        if (ret < 0) {
            break;
        }
        
        if (was_active) {
            nb_inactive = 0;
        } else {
            nb_inactive++;
        }
        
        /* Check if all chunks have been sent */
        int all_sent = 1;
        for (int i = 0; i < deadline_ctx->nb_streams; i++) {
            size_t scenario_idx = i;
            if (scenario_idx >= nb_scenario) continue;
            
            if (scenario[scenario_idx].stream_type == st_stream_type_deadline) {
                if (deadline_ctx->stream_state[i].chunks_sent < scenario[scenario_idx].num_chunks) {
                    all_sent = 0;
                    break;
                }
            } else {
                if (deadline_ctx->stream_state[i].bytes_sent == 0) {
                    all_sent = 0;
                    break;
                }
            }
        }
        
        if (all_sent) {
            test_ctx->test_finished = 1;
            deadline_ctx->test_completed = 1;
        }
        
        /* Check if we should exit */
        if (test_ctx->test_finished) {
            if (picoquic_is_cnx_backlog_empty(test_ctx->cnx_client) && 
                picoquic_is_cnx_backlog_empty(test_ctx->cnx_server)) {
                break;
            }
        }
    }
    
    if (nb_trials >= max_trials) {
        DBG_PRINTF("Test exit after %d trials\n", nb_trials);
        ret = -1;
    } else if (nb_inactive >= 256) {
        DBG_PRINTF("Test exit after %d inactive rounds, completed=%d\n", nb_inactive, deadline_ctx->test_completed);
        /* Don't fail if we completed the test */
        if (!deadline_ctx->test_completed) {
            ret = -1;
        }
    }
    
    return ret;
}

/* Calculate statistics for normal streams */
void deadline_api_calculate_normal_stats(deadline_api_test_ctx_t* deadline_ctx, int stream_idx)
{
    int server_idx = -1;
    uint64_t stream_id = deadline_ctx->stream_state[stream_idx].stream_id;
    
    /* Find corresponding server stream */
    for (int i = 0; i < deadline_ctx->nb_server_streams; i++) {
        if (deadline_ctx->server_stream_state[i].stream_id == stream_id) {
            server_idx = i;
            break;
        }
    }
    
    if (server_idx < 0) return;
    
    /* Allocate stats structure */
    deadline_ctx->normal_stats[stream_idx] = (normal_stream_stats_t*)calloc(1, sizeof(normal_stream_stats_t));
    if (deadline_ctx->normal_stats[stream_idx] == NULL) return;
    
    normal_stream_stats_t* stats = deadline_ctx->normal_stats[stream_idx];
    
    /* Calculate time to first byte */
    stats->time_to_first_byte = deadline_ctx->server_stream_state[server_idx].first_byte_time - 
                                deadline_ctx->stream_state[stream_idx].first_send_time;
    
    /* Calculate time to completion */
    stats->time_to_completion = deadline_ctx->server_stream_state[server_idx].last_byte_time - 
                               deadline_ctx->stream_state[stream_idx].first_send_time;
    
    /* Calculate throughput */
    stats->bytes_transferred = deadline_ctx->server_stream_state[server_idx].bytes_received;
    if (stats->time_to_completion > 0) {
        /* Convert to Mbps: (bytes * 8) / (microseconds) */
        stats->throughput_mbps = (stats->bytes_transferred * 8.0) / stats->time_to_completion;
    }
}

/* Calculate statistics for deadline streams */
void deadline_api_calculate_deadline_stats(deadline_api_test_ctx_t* deadline_ctx, int stream_idx,
                                                 st_test_api_deadline_stream_desc_t* scenario, size_t nb_scenario)
{
    int server_idx = -1;
    uint64_t stream_id = deadline_ctx->stream_state[stream_idx].stream_id;
    
    /* Find corresponding server stream */
    for (int i = 0; i < deadline_ctx->nb_server_streams; i++) {
        if (deadline_ctx->server_stream_state[i].stream_id == stream_id) {
            server_idx = i;
            break;
        }
    }
    
    if (server_idx < 0) return;
    
    /* Find scenario info */
    int scenario_idx = -1;
    for (size_t i = 0; i < nb_scenario; i++) {
        if (scenario[i].stream_id == stream_id) {
            scenario_idx = i;
            break;
        }
    }
    
    if (scenario_idx < 0) return;
    
    /* Allocate stats structure */
    deadline_ctx->deadline_stats[stream_idx] = (deadline_stream_stats_t*)calloc(1, sizeof(deadline_stream_stats_t));
    if (deadline_ctx->deadline_stats[stream_idx] == NULL) return;
    
    deadline_stream_stats_t* stats = deadline_ctx->deadline_stats[stream_idx];
    stats->num_chunks = scenario[scenario_idx].num_chunks;
    
    /* Allocate chunk stats */
    stats->chunk_stats = calloc(stats->num_chunks, 
                               sizeof(struct { uint64_t send_time; uint64_t receive_time; 
                                              uint64_t deadline_time; int deadline_met; uint64_t latency; }));
    if (stats->chunk_stats == NULL) return;
    
    /* Calculate per-chunk statistics */
    int deadlines_met = 0;
    double total_latency = 0;
    double latency_squared_sum = 0;
    
    int chunks_to_analyze = stats->num_chunks;
    if (chunks_to_analyze > deadline_ctx->server_stream_state[server_idx].chunks_received) {
        chunks_to_analyze = deadline_ctx->server_stream_state[server_idx].chunks_received;
    }
    
    for (int i = 0; i < chunks_to_analyze; i++) {
        if (deadline_ctx->stream_state[stream_idx].chunk_send_times != NULL &&
            deadline_ctx->server_stream_state[server_idx].chunk_receive_times != NULL &&
            deadline_ctx->stream_state[stream_idx].chunk_send_times[i] > 0 &&
            deadline_ctx->server_stream_state[server_idx].chunk_receive_times[i] > 0) {
            
            stats->chunk_stats[i].send_time = deadline_ctx->stream_state[stream_idx].chunk_send_times[i];
            stats->chunk_stats[i].receive_time = deadline_ctx->server_stream_state[server_idx].chunk_receive_times[i];
            stats->chunk_stats[i].deadline_time = stats->chunk_stats[i].send_time + 
                                                 (scenario[scenario_idx].deadline_ms * 1000);
            stats->chunk_stats[i].latency = stats->chunk_stats[i].receive_time - stats->chunk_stats[i].send_time;
            stats->chunk_stats[i].deadline_met = (stats->chunk_stats[i].receive_time <= stats->chunk_stats[i].deadline_time) ? 1 : 0;
        } else {
            /* Invalid data, skip this chunk */
            chunks_to_analyze = i;
            break;
        }
        
        if (stats->chunk_stats[i].deadline_met) {
            deadlines_met++;
        }
        
        total_latency += stats->chunk_stats[i].latency;
        latency_squared_sum += (stats->chunk_stats[i].latency * stats->chunk_stats[i].latency);
    }
    
    /* Calculate aggregate statistics */
    if (chunks_to_analyze > 0) {
        stats->deadline_compliance_percent = (deadlines_met * 100.0) / chunks_to_analyze;
        stats->avg_latency_ms = (total_latency / chunks_to_analyze) / 1000.0;
        
        /* Calculate jitter (standard deviation) */
        if (chunks_to_analyze > 1) {
            double avg_latency_us = total_latency / chunks_to_analyze;
            double variance = (latency_squared_sum / chunks_to_analyze) - (avg_latency_us * avg_latency_us);
            if (variance > 0) {
                stats->latency_jitter_ms = sqrt(variance) / 1000.0;
            } else {
                stats->latency_jitter_ms = 0;
            }
        }
    }
    
    /* Update num_chunks to reflect actual analyzed chunks */
    stats->num_chunks = chunks_to_analyze;
    
    /* Time to first byte and completion */
    stats->time_to_first_byte = deadline_ctx->server_stream_state[server_idx].first_byte_time - 
                               deadline_ctx->stream_state[stream_idx].first_send_time;
    stats->time_to_completion = deadline_ctx->server_stream_state[server_idx].last_byte_time - 
                               deadline_ctx->stream_state[stream_idx].first_send_time;
    
    /* Throughput */
    if (stats->time_to_completion > 0) {
        stats->avg_throughput_mbps = (deadline_ctx->server_stream_state[server_idx].bytes_received * 8.0) / 
                                    stats->time_to_completion;
    }
}

/* Print statistics report */
void deadline_api_print_stats(deadline_api_test_ctx_t* deadline_ctx,
                                    st_test_api_deadline_stream_desc_t* scenario,
                                    size_t nb_scenario)
{
    DBG_PRINTF("%s:%u:%s: \n======== Stream Statistics Report ========\n", __FILE__, __LINE__, __func__);
    
    for (int i = 0; i < deadline_ctx->nb_streams; i++) {
        uint64_t stream_id = deadline_ctx->stream_state[i].stream_id;
        
        /* Find scenario info */
        int scenario_idx = -1;
        for (size_t j = 0; j < nb_scenario; j++) {
            if (scenario[j].stream_id == stream_id) {
                scenario_idx = j;
                break;
            }
        }
        
        if (scenario_idx < 0) continue;
        
        DBG_PRINTF("\nStream %lu (%s):\n", (unsigned long)stream_id,
            scenario[scenario_idx].stream_type == st_stream_type_normal ? "Normal" : "Deadline");
        
        if (scenario[scenario_idx].stream_type == st_stream_type_normal && deadline_ctx->normal_stats[i]) {
            normal_stream_stats_t* stats = deadline_ctx->normal_stats[i];
            DBG_PRINTF("  Time to first byte: %.2f ms\n", stats->time_to_first_byte / 1000.0);
            DBG_PRINTF("  Time to completion: %.2f ms\n", stats->time_to_completion / 1000.0);
            DBG_PRINTF("  Bytes transferred: %zu\n", stats->bytes_transferred);
            DBG_PRINTF("  Average throughput: %.2f Mbps\n", stats->throughput_mbps);
        } else if (scenario[scenario_idx].stream_type == st_stream_type_deadline && deadline_ctx->deadline_stats[i]) {
            deadline_stream_stats_t* stats = deadline_ctx->deadline_stats[i];
            DBG_PRINTF("  Deadline: %lu ms\n", (unsigned long)scenario[scenario_idx].deadline_ms);
            DBG_PRINTF("  Deadline compliance: %.1f%% (%d/%d chunks)\n", 
                stats->deadline_compliance_percent,
                (int)(stats->deadline_compliance_percent * stats->num_chunks / 100),
                stats->num_chunks);
            DBG_PRINTF("  Average latency: %.2f ms\n", stats->avg_latency_ms);
            DBG_PRINTF("  Latency jitter: %.2f ms\n", stats->latency_jitter_ms);
            DBG_PRINTF("  Time to first byte: %.2f ms\n", stats->time_to_first_byte / 1000.0);
            DBG_PRINTF("  Time to completion: %.2f ms\n", stats->time_to_completion / 1000.0);
            DBG_PRINTF("  Average throughput: %.2f Mbps\n", stats->avg_throughput_mbps);
            
            /* Print per-chunk details for deadline violations */
            int violations = 0;
            for (int j = 0; j < stats->num_chunks; j++) {
                if (!stats->chunk_stats[j].deadline_met) {
                    if (violations == 0) {
                        DBG_PRINTF("%s:%u:%s:   Deadline violations:\n", __FILE__, __LINE__, __func__);
                    }
                    DBG_PRINTF("    Chunk %d: latency %.2f ms (deadline was %.2f ms)\n",
                        j + 1, stats->chunk_stats[j].latency / 1000.0,
                        scenario[scenario_idx].deadline_ms);
                    violations++;
                }
            }
        }
    }
    
    DBG_PRINTF("%s:%u:%s: \n========================================\n", __FILE__, __LINE__, __func__);
}

/* Verify that all data was received correctly */
int deadline_api_verify(picoquic_test_tls_api_ctx_t* test_ctx,
                               deadline_api_test_ctx_t* deadline_ctx,
                               st_test_api_deadline_stream_desc_t* scenario,
                               size_t nb_scenario)
{
    int ret = 0;
    
    /* Check for errors in callbacks */
    if (test_ctx->server_callback.error_detected) {
        DBG_PRINTF("Server callback error detected: 0x%x\n", test_ctx->server_callback.error_detected);
        ret = -1;
    }
    else if (test_ctx->client_callback.error_detected) {
        DBG_PRINTF("Client callback error detected: 0x%x\n", test_ctx->client_callback.error_detected);
        ret = -1;
    }
    
    /* Verify all data was sent by client */
    for (size_t i = 0; ret == 0 && i < deadline_ctx->nb_streams; i++) {
        size_t scenario_idx = i;
        if (scenario_idx >= nb_scenario) continue;
        
        size_t expected_bytes;
        if (scenario[scenario_idx].stream_type == st_stream_type_deadline) {
            expected_bytes = scenario[scenario_idx].chunk_size * scenario[scenario_idx].num_chunks;
        } else {
            expected_bytes = scenario[scenario_idx].len;
        }
        
        if (deadline_ctx->stream_state[i].bytes_sent != expected_bytes) {
            DBG_PRINTF("Stream %lu sent %zu bytes, expected %zu\n",
                (unsigned long)deadline_ctx->stream_state[i].stream_id,
                deadline_ctx->stream_state[i].bytes_sent, expected_bytes);
            ret = -1;
        }
    }
    
    /* Verify all data was received by server */
    for (size_t i = 0; ret == 0 && i < deadline_ctx->nb_server_streams; i++) {
        if (!deadline_ctx->server_stream_state[i].fin_received) {
            DBG_PRINTF("Server: Stream %lu did not receive FIN\n",
                (unsigned long)deadline_ctx->server_stream_state[i].stream_id);
            ret = -1;
        }
        
        if (deadline_ctx->server_stream_state[i].bytes_received != 
            deadline_ctx->server_stream_state[i].expected_bytes) {
            DBG_PRINTF("Server: Stream %lu received %zu bytes, expected %zu\n",
                (unsigned long)deadline_ctx->server_stream_state[i].stream_id,
                deadline_ctx->server_stream_state[i].bytes_received,
                deadline_ctx->server_stream_state[i].expected_bytes);
            ret = -1;
        }
        
        /* Verify data content matches */
        if (ret == 0 && deadline_ctx->server_stream_state[i].recv_buffer != NULL) {
            /* Find corresponding client stream to get source data */
            int client_stream_idx = -1;
            for (int j = 0; j < deadline_ctx->nb_streams; j++) {
                if (deadline_ctx->stream_state[j].stream_id == 
                    deadline_ctx->server_stream_state[i].stream_id) {
                    client_stream_idx = j;
                    break;
                }
            }
            
            if (client_stream_idx >= 0 && deadline_ctx->stream_state[client_stream_idx].send_buffer != NULL) {
                if (memcmp(deadline_ctx->server_stream_state[i].recv_buffer,
                          deadline_ctx->stream_state[client_stream_idx].send_buffer,
                          deadline_ctx->server_stream_state[i].bytes_received) != 0) {
                    DBG_PRINTF("Server: Data mismatch on stream %lu\n",
                        (unsigned long)deadline_ctx->server_stream_state[i].stream_id);
                    ret = -1;
                }
            }
        }
    }
    
    /* Verify we received data for all expected streams */
    if (ret == 0) {
        for (size_t i = 0; i < deadline_ctx->nb_streams; i++) {
            uint64_t stream_id = deadline_ctx->stream_state[i].stream_id;
            int found = 0;
            for (size_t j = 0; j < deadline_ctx->nb_server_streams; j++) {
                if (deadline_ctx->server_stream_state[j].stream_id == stream_id) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                DBG_PRINTF("Server: No data received for stream %lu\n", 
                    (unsigned long)stream_id);
                ret = -1;
            }
        }
    }
    
    /* Check memory leaks */
    if (ret == 0 && test_ctx->qclient->nb_data_nodes_allocated > test_ctx->qclient->nb_data_nodes_in_pool) {
        DBG_PRINTF("Client memory leak: %d allocated, %d in pool\n",
            test_ctx->qclient->nb_data_nodes_allocated,
            test_ctx->qclient->nb_data_nodes_in_pool);
        ret = -1;
    }
    
    if (ret == 0 && test_ctx->qserver->nb_data_nodes_allocated > test_ctx->qserver->nb_data_nodes_in_pool) {
        DBG_PRINTF("Server memory leak: %d allocated, %d in pool\n",
            test_ctx->qserver->nb_data_nodes_allocated,
            test_ctx->qserver->nb_data_nodes_in_pool);
        ret = -1;
    }
    
    return ret;
}

/* Test single deadline stream */
int deadline_api_test_single_stream()
{
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    deadline_api_test_ctx_t* deadline_ctx = NULL;
    int ret = 0;
    
    /* Initialize context with deadline-aware streams enabled */
    ret = deadline_api_init_ctx(&test_ctx, &simulated_time, &deadline_ctx);
    
    if (ret == 0) {
        /* Store deadline context globally for simple access */
        g_deadline_ctx = deadline_ctx;
        
        /* Start the connection */
        ret = picoquic_start_client_cnx(test_ctx->cnx_client);
    }
    
    if (ret == 0) {
        /* Run connection establishment */
        ret = tls_api_connection_loop(test_ctx, NULL, 0, &simulated_time);
    }
    
    /* Server will use the same test callbacks */
    
    if (ret == 0) {
        /* Verify deadline-aware streams were negotiated */
        if (!test_ctx->cnx_client->remote_parameters.enable_deadline_aware_streams ||
            !test_ctx->cnx_server->local_parameters.enable_deadline_aware_streams) {
            DBG_PRINTF("%s", "Deadline-aware streams not negotiated\n");
            ret = -1;
        }
    }
    
    if (ret == 0) {
        /* Run the deadline stream test */
        ret = deadline_api_data_sending_loop(test_ctx, deadline_ctx,
                                    test_scenario_single_deadline,
                                    sizeof(test_scenario_single_deadline) / sizeof(st_test_api_deadline_stream_desc_t),
                                    &simulated_time);
    }
    
    if (ret == 0) {
        /* Verify test completed successfully */
        if (!deadline_ctx->test_completed) {
            DBG_PRINTF("Test did not complete. Streams created: %d, chunks sent on stream 0: %d\n",
                deadline_ctx->nb_streams,
                deadline_ctx->nb_streams > 0 ? deadline_ctx->stream_state[0].chunks_sent : -1);
            ret = -1;
        }
    }
    
    /* Calculate and print statistics */
    if (ret == 0) {
        size_t nb_scenario = sizeof(test_scenario_single_deadline) / sizeof(st_test_api_deadline_stream_desc_t);
        
        /* Calculate stats for each stream */
        for (int i = 0; i < deadline_ctx->nb_streams; i++) {
            int scenario_idx = -1;
            for (size_t j = 0; j < nb_scenario; j++) {
                if (test_scenario_single_deadline[j].stream_id == deadline_ctx->stream_state[i].stream_id) {
                    scenario_idx = j;
                    break;
                }
            }
            
            if (scenario_idx >= 0) {
                if (test_scenario_single_deadline[scenario_idx].stream_type == st_stream_type_normal) {
                    deadline_api_calculate_normal_stats(deadline_ctx, i);
                } else {
                    deadline_api_calculate_deadline_stats(deadline_ctx, i, test_scenario_single_deadline, nb_scenario);
                }
            }
        }
        
        /* Print statistics report */
        deadline_api_print_stats(deadline_ctx, test_scenario_single_deadline, nb_scenario);
    }
    
    /* Verify all data was received correctly */
    if (ret == 0) {
        ret = deadline_api_verify(test_ctx, deadline_ctx, test_scenario_single_deadline,
                                 sizeof(test_scenario_single_deadline) / sizeof(st_test_api_deadline_stream_desc_t));
    }
    
    /* Clean up */
    if (deadline_ctx != NULL) {
        deadline_api_delete_test_ctx(deadline_ctx);
    }
    
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    return ret;
}

/* Test mixed deadline and normal streams */
int deadline_api_test_mixed_streams()
{
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    deadline_api_test_ctx_t* deadline_ctx = NULL;
    int ret = 0;
    
    /* Initialize context */
    ret = deadline_api_init_ctx(&test_ctx, &simulated_time, &deadline_ctx);
    
    if (ret == 0) {
        /* Store deadline context globally */
        g_deadline_ctx = deadline_ctx;
        
        /* Start connection */
        ret = picoquic_start_client_cnx(test_ctx->cnx_client);
    }
    
    if (ret == 0) {
        /* Establish connection */
        ret = tls_api_connection_loop(test_ctx, NULL, 0, &simulated_time);
    }
    
    /* Server will use the same test callbacks */
    
    if (ret == 0) {
        /* Run mixed stream test */
        ret = deadline_api_data_sending_loop(test_ctx, deadline_ctx,
                                    test_scenario_mixed_streams,
                                    sizeof(test_scenario_mixed_streams) / sizeof(st_test_api_deadline_stream_desc_t),
                                    &simulated_time);
    }
    
    /* Calculate and print statistics */
    if (ret == 0) {
        size_t nb_scenario = sizeof(test_scenario_mixed_streams) / sizeof(st_test_api_deadline_stream_desc_t);
        
        /* Calculate stats for each stream */
        for (int i = 0; i < deadline_ctx->nb_streams; i++) {
            int scenario_idx = -1;
            for (size_t j = 0; j < nb_scenario; j++) {
                if (test_scenario_mixed_streams[j].stream_id == deadline_ctx->stream_state[i].stream_id) {
                    scenario_idx = j;
                    break;
                }
            }
            
            if (scenario_idx >= 0) {
                if (test_scenario_mixed_streams[scenario_idx].stream_type == st_stream_type_normal) {
                    deadline_api_calculate_normal_stats(deadline_ctx, i);
                } else {
                    deadline_api_calculate_deadline_stats(deadline_ctx, i, test_scenario_mixed_streams, nb_scenario);
                }
            }
        }
        
        /* Print statistics report */
        deadline_api_print_stats(deadline_ctx, test_scenario_mixed_streams, nb_scenario);
    }
    
    /* Verify all data was received correctly */
    if (ret == 0) {
        ret = deadline_api_verify(test_ctx, deadline_ctx, test_scenario_mixed_streams,
                                 sizeof(test_scenario_mixed_streams) / sizeof(st_test_api_deadline_stream_desc_t));
    }
    
    /* Clean up */
    if (deadline_ctx != NULL) {
        deadline_api_delete_test_ctx(deadline_ctx);
    }
    
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    return ret;
}

/* Test multiple deadline streams with different parameters */
int deadline_api_test_multiple_deadlines()
{
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    deadline_api_test_ctx_t* deadline_ctx = NULL;
    int ret = 0;
    
    /* Initialize context */
    ret = deadline_api_init_ctx(&test_ctx, &simulated_time, &deadline_ctx);
    
    if (ret == 0) {
        /* Store deadline context globally */
        g_deadline_ctx = deadline_ctx;
        
        /* Start connection */
        ret = picoquic_start_client_cnx(test_ctx->cnx_client);
    }
    
    if (ret == 0) {
        /* Establish connection */
        ret = tls_api_connection_loop(test_ctx, NULL, 0, &simulated_time);
    }
    
    /* Server will use the same test callbacks */
    
    if (ret == 0) {
        /* Run multiple deadline streams test */
        ret = deadline_api_data_sending_loop(test_ctx, deadline_ctx,
                                    test_scenario_multiple_deadlines,
                                    sizeof(test_scenario_multiple_deadlines) / sizeof(st_test_api_deadline_stream_desc_t),
                                    &simulated_time);
    }
    
    /* Calculate and print statistics */
    if (ret == 0) {
        size_t nb_scenario = sizeof(test_scenario_multiple_deadlines) / sizeof(st_test_api_deadline_stream_desc_t);
        
        /* Calculate stats for each stream */
        for (int i = 0; i < deadline_ctx->nb_streams; i++) {
            int scenario_idx = -1;
            for (size_t j = 0; j < nb_scenario; j++) {
                if (test_scenario_multiple_deadlines[j].stream_id == deadline_ctx->stream_state[i].stream_id) {
                    scenario_idx = j;
                    break;
                }
            }
            
            if (scenario_idx >= 0) {
                if (test_scenario_multiple_deadlines[scenario_idx].stream_type == st_stream_type_normal) {
                    deadline_api_calculate_normal_stats(deadline_ctx, i);
                } else {
                    deadline_api_calculate_deadline_stats(deadline_ctx, i, test_scenario_multiple_deadlines, nb_scenario);
                }
            }
        }
        
        /* Print statistics report */
        deadline_api_print_stats(deadline_ctx, test_scenario_multiple_deadlines, nb_scenario);
    }
    
    /* Verify all data was received correctly */
    if (ret == 0) {
        ret = deadline_api_verify(test_ctx, deadline_ctx, test_scenario_multiple_deadlines,
                                 sizeof(test_scenario_multiple_deadlines) / sizeof(st_test_api_deadline_stream_desc_t));
    }
    
    /* Clean up */
    if (deadline_ctx != NULL) {
        deadline_api_delete_test_ctx(deadline_ctx);
    }
    
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }
    
    return ret;
}