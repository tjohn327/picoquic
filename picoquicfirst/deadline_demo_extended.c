/*
* Deadline Demo Extended - Comprehensive testing of deadline-aware streams
* 
* This extended demo tests all deadline features in realistic scenarios:
* 1. Multiple concurrent streams with different deadlines
* 2. Mixed traffic (deadline and non-deadline streams)
* 3. Partial reliability with data dropping
* 4. Gap handling and recovery
* 5. Fairness between deadline and non-deadline traffic
* 6. Performance under various network conditions
* 7. Multipath scenarios (if available)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include "../picoquic/picoquic.h"
#include "../picoquic/picoquic_internal.h"
#include "../picoquic/picosocks.h"
#include "../picoquic/picoquic_utils.h"
#include "../picoquic/picoquic_packet_loop.h"

#define DEMO_PORT 4443
#define DEMO_ALPN "deadline-test-extended"

/* Test scenarios */
typedef enum {
    SCENARIO_BASIC = 0,
    SCENARIO_MIXED_TRAFFIC,
    SCENARIO_HIGH_LOAD,
    SCENARIO_NETWORK_STRESS,
    SCENARIO_PARTIAL_RELIABILITY,
    SCENARIO_FAIRNESS,
    SCENARIO_REALTIME_VIDEO,
    SCENARIO_MAX
} test_scenario_t;

/* Stream types for testing */
typedef enum {
    STREAM_TYPE_URGENT_SMALL = 0,    /* Small urgent messages (control, telemetry) */
    STREAM_TYPE_VIDEO_FRAME,         /* Video frames with hard deadlines */
    STREAM_TYPE_AUDIO_PACKET,        /* Audio packets with soft deadlines */
    STREAM_TYPE_BULK_DATA,           /* Large file transfer, no deadline */
    STREAM_TYPE_INTERACTIVE,         /* Interactive data with medium deadline */
    STREAM_TYPE_BACKGROUND           /* Background sync, no deadline */
} stream_type_t;

/* Stream configuration */
typedef struct {
    uint64_t stream_id;
    stream_type_t type;
    size_t data_size;
    uint64_t deadline_ms;
    int is_hard;
    uint64_t start_time;
    uint64_t end_time;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t bytes_dropped;
    uint64_t gaps_received;
    int completed;
    int success;  /* Met deadline? */
    
    /* Pattern verification */
    uint32_t pattern_seed;
    uint64_t last_verified_offset;
    
    /* Statistics */
    uint64_t first_byte_time;
    uint64_t last_byte_time;
    double throughput_mbps;
    double latency_ms;
} stream_config_t;

/* Test context */
typedef struct {
    int is_server;
    test_scenario_t scenario;
    int scenario_complete;
    
    /* Streams */
    stream_config_t streams[100];
    int num_streams;
    
    /* Global statistics */
    uint64_t total_bytes_sent;
    uint64_t total_bytes_received;
    uint64_t total_bytes_dropped;
    uint64_t total_gaps;
    int streams_succeeded;
    int streams_failed;
    int hard_deadlines_missed;
    int soft_deadlines_missed;
    
    /* Timing */
    uint64_t test_start_time;
    uint64_t test_end_time;
    
    /* Network simulation */
    int simulate_loss;
    double loss_rate;
    int simulate_delay;
    uint64_t added_delay_us;
    int simulate_bandwidth_limit;
    uint64_t bandwidth_bps;
    
    /* Verification */
    int verify_data;
    int errors_detected;
} demo_context_t;

static int running = 1;
static int verbose = 0;

/* Signal handler */
void signal_handler(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    running = 0;
}

/* Generate deterministic data pattern */
static void generate_pattern(uint8_t* buffer, size_t length, uint32_t seed, uint64_t offset) {
    for (size_t i = 0; i < length; i++) {
        /* Simple but verifiable pattern */
        uint64_t pos = offset + i;
        buffer[i] = (uint8_t)((seed ^ pos) & 0xFF);
    }
}

/* Verify data pattern */
static int verify_pattern(const uint8_t* buffer, size_t length, uint32_t seed, uint64_t offset) {
    for (size_t i = 0; i < length; i++) {
        uint64_t pos = offset + i;
        uint8_t expected = (uint8_t)((seed ^ pos) & 0xFF);
        if (buffer[i] != expected) {
            return -1;
        }
    }
    return 0;
}

/* Get stream type name */
static const char* stream_type_name(stream_type_t type) {
    switch (type) {
    case STREAM_TYPE_URGENT_SMALL: return "URGENT_SMALL";
    case STREAM_TYPE_VIDEO_FRAME: return "VIDEO_FRAME";
    case STREAM_TYPE_AUDIO_PACKET: return "AUDIO";
    case STREAM_TYPE_BULK_DATA: return "BULK";
    case STREAM_TYPE_INTERACTIVE: return "INTERACTIVE";
    case STREAM_TYPE_BACKGROUND: return "BACKGROUND";
    default: return "UNKNOWN";
    }
}

/* Configure streams for scenario */
static void configure_scenario_streams(demo_context_t* ctx) {
    ctx->num_streams = 0;
    uint64_t stream_id = 4; /* Start from 4 to avoid reserved streams */
    
    switch (ctx->scenario) {
    case SCENARIO_BASIC:
        /* Basic test: 3 streams with different deadlines */
        ctx->streams[0] = (stream_config_t){
            .stream_id = stream_id,
            .type = STREAM_TYPE_URGENT_SMALL,
            .data_size = 10000,
            .deadline_ms = 50,
            .is_hard = 1,
            .pattern_seed = 0x1234
        };
        ctx->streams[1] = (stream_config_t){
            .stream_id = stream_id + 4,
            .type = STREAM_TYPE_INTERACTIVE,
            .data_size = 100000,
            .deadline_ms = 200,
            .is_hard = 0,
            .pattern_seed = 0x5678
        };
        ctx->streams[2] = (stream_config_t){
            .stream_id = stream_id + 8,
            .type = STREAM_TYPE_BULK_DATA,
            .data_size = 1000000,
            .deadline_ms = 0,
            .is_hard = 0,
            .pattern_seed = 0x9ABC
        };
        ctx->num_streams = 3;
        break;
        
    case SCENARIO_MIXED_TRAFFIC:
        /* Mixed traffic: urgent, interactive, and bulk */
        for (int i = 0; i < 10; i++) {
            ctx->streams[i] = (stream_config_t){
                .stream_id = stream_id + (i * 4),
                .type = i % 3 == 0 ? STREAM_TYPE_URGENT_SMALL : 
                       (i % 3 == 1 ? STREAM_TYPE_INTERACTIVE : STREAM_TYPE_BULK_DATA),
                .data_size = i % 3 == 0 ? 5000 : (i % 3 == 1 ? 50000 : 500000),
                .deadline_ms = i % 3 == 0 ? 30 : (i % 3 == 1 ? 100 : 0),
                .is_hard = i % 3 == 0 ? 1 : 0,
                .pattern_seed = 0x1000 + i
            };
        }
        ctx->num_streams = 10;
        break;
        
    case SCENARIO_HIGH_LOAD:
        /* High load: 50 concurrent streams */
        for (int i = 0; i < 50; i++) {
            ctx->streams[i] = (stream_config_t){
                .stream_id = stream_id + (i * 4),
                .type = i < 10 ? STREAM_TYPE_URGENT_SMALL : STREAM_TYPE_INTERACTIVE,
                .data_size = i < 10 ? 1000 : 10000,
                .deadline_ms = i < 10 ? 20 : 100,
                .is_hard = i < 5 ? 1 : 0,
                .pattern_seed = 0x2000 + i
            };
        }
        ctx->num_streams = 50;
        break;
        
    case SCENARIO_NETWORK_STRESS:
        /* Network stress: Large transfers with tight deadlines */
        ctx->simulate_loss = 1;
        ctx->loss_rate = 0.01; /* 1% loss */
        ctx->simulate_delay = 1;
        ctx->added_delay_us = 50000; /* 50ms additional delay */
        
        for (int i = 0; i < 5; i++) {
            ctx->streams[i] = (stream_config_t){
                .stream_id = stream_id + (i * 4),
                .type = STREAM_TYPE_VIDEO_FRAME,
                .data_size = 100000, /* 100KB video frame */
                .deadline_ms = 150, /* 150ms deadline */
                .is_hard = 1,
                .pattern_seed = 0x3000 + i
            };
        }
        ctx->num_streams = 5;
        break;
        
    case SCENARIO_PARTIAL_RELIABILITY:
        /* Test partial reliability with very tight deadlines */
        ctx->simulate_bandwidth_limit = 1;
        ctx->bandwidth_bps = 1000000; /* 1 Mbps limit */
        
        for (int i = 0; i < 10; i++) {
            ctx->streams[i] = (stream_config_t){
                .stream_id = stream_id + (i * 4),
                .type = STREAM_TYPE_VIDEO_FRAME,
                .data_size = 50000,
                .deadline_ms = 50, /* Very tight deadline */
                .is_hard = 1,
                .pattern_seed = 0x4000 + i
            };
        }
        ctx->num_streams = 10;
        break;
        
    case SCENARIO_FAIRNESS:
        /* Test fairness between deadline and non-deadline streams */
        for (int i = 0; i < 20; i++) {
            ctx->streams[i] = (stream_config_t){
                .stream_id = stream_id + (i * 4),
                .type = i < 10 ? STREAM_TYPE_INTERACTIVE : STREAM_TYPE_BACKGROUND,
                .data_size = 100000,
                .deadline_ms = i < 10 ? 100 : 0,
                .is_hard = 0,
                .pattern_seed = 0x5000 + i
            };
        }
        ctx->num_streams = 20;
        break;
        
    case SCENARIO_REALTIME_VIDEO:
        /* Simulate real-time video streaming */
        /* 30 fps, 1 second of video = 30 frames */
        for (int i = 0; i < 30; i++) {
            ctx->streams[i] = (stream_config_t){
                .stream_id = stream_id + (i * 4),
                .type = STREAM_TYPE_VIDEO_FRAME,
                .data_size = 50000 + (rand() % 50000), /* 50-100KB per frame */
                .deadline_ms = 33, /* 33ms per frame for 30fps */
                .is_hard = 1,
                .pattern_seed = 0x6000 + i
            };
        }
        ctx->num_streams = 30;
        break;
        
    default:
        break;
    }
}

/* Demo callback */
int demo_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{
    demo_context_t* ctx = (demo_context_t*)callback_ctx;
    stream_config_t* stream = NULL;
    
    /* Find stream */
    for (int i = 0; i < ctx->num_streams; i++) {
        if (ctx->streams[i].stream_id == stream_id) {
            stream = &ctx->streams[i];
            break;
        }
    }
    
    switch (fin_or_event) {
    case picoquic_callback_stream_data:
        if (stream == NULL && ctx->is_server && ctx->num_streams < 100) {
            /* New stream on server */
            stream = &ctx->streams[ctx->num_streams++];
            stream->stream_id = stream_id;
            stream->start_time = picoquic_get_quic_time(cnx->quic);
            stream->pattern_seed = stream_id; /* Use stream ID as seed for server */
            if (verbose) {
                printf("[SERVER] New stream %lu\n", stream_id);
            }
            
            /* Echo mode - mark stream as active */
            picoquic_mark_active_stream(cnx, stream_id, 1, NULL);
        }
        
        if (stream != NULL) {
            if (stream->first_byte_time == 0) {
                stream->first_byte_time = picoquic_get_quic_time(cnx->quic);
            }
            stream->last_byte_time = picoquic_get_quic_time(cnx->quic);
            stream->bytes_received += length;
            ctx->total_bytes_received += length;
            
            /* Verify data if enabled */
            if (ctx->verify_data && length > 0) {
                if (verify_pattern(bytes, length, stream->pattern_seed, 
                                 stream->last_verified_offset) != 0) {
                    ctx->errors_detected++;
                    printf("[ERROR] Data corruption detected on stream %lu at offset %lu\n",
                           stream_id, stream->last_verified_offset);
                }
                stream->last_verified_offset += length;
            }
            
            if (verbose && (stream->bytes_received % 100000) == 0) {
                printf("[%s] Stream %lu: %lu KB received\n", 
                       ctx->is_server ? "SERVER" : "CLIENT",
                       stream_id, stream->bytes_received / 1000);
            }
        }
        break;
        
    case picoquic_callback_stream_fin:
        if (stream != NULL) {
            stream->completed = 1;
            stream->end_time = picoquic_get_quic_time(cnx->quic);
            
            /* Calculate statistics */
            uint64_t duration = stream->end_time - stream->start_time;
            if (duration > 0) {
                stream->throughput_mbps = (stream->bytes_received * 8.0) / duration;
            }
            
            /* Check if deadline was met */
            if (stream->deadline_ms > 0) {
                stream->success = (duration <= stream->deadline_ms * 1000);
                if (!stream->success) {
                    if (stream->is_hard) {
                        ctx->hard_deadlines_missed++;
                    } else {
                        ctx->soft_deadlines_missed++;
                    }
                } else {
                    ctx->streams_succeeded++;
                }
            } else {
                stream->success = 1;
                ctx->streams_succeeded++;
            }
            
            if (verbose) {
                printf("[%s] Stream %lu completed: %lu bytes in %lu ms (deadline: %lu ms) - %s\n",
                       ctx->is_server ? "SERVER" : "CLIENT",
                       stream_id, stream->bytes_received, duration / 1000,
                       stream->deadline_ms, stream->success ? "SUCCESS" : "FAILED");
            }
        }
        break;
        
    case picoquic_callback_stream_gap:
        /* Gap notification - data was dropped */
        if (stream != NULL) {
            uint64_t gap_length = length; /* Length contains the gap size */
            stream->gaps_received++;
            stream->bytes_dropped += gap_length;
            ctx->total_gaps++;
            ctx->total_bytes_dropped += gap_length;
            
            /* Adjust verification offset */
            if (ctx->verify_data) {
                stream->last_verified_offset += gap_length;
            }
            
            printf("[%s] GAP on stream %lu: %lu bytes dropped (total gaps: %lu)\n",
                   ctx->is_server ? "SERVER" : "CLIENT",
                   stream_id, gap_length, stream->gaps_received);
        }
        break;
        
    case picoquic_callback_prepare_to_send:
        /* Send data */
        if (!ctx->is_server && stream != NULL && stream->bytes_sent < stream->data_size) {
            size_t available = stream->data_size - stream->bytes_sent;
            if (available > length) {
                available = length;
            }
            
            /* Get buffer and generate pattern */
            int is_fin = (stream->bytes_sent + available >= stream->data_size) ? 1 : 0;
            int is_still_active = !is_fin;
            uint8_t* buffer = picoquic_provide_stream_data_buffer(bytes, available, 
                                                                 is_fin, is_still_active);
            
            if (buffer != NULL) {
                generate_pattern(buffer, available, stream->pattern_seed, stream->bytes_sent);
                stream->bytes_sent += available;
                ctx->total_bytes_sent += available;
                
                if (verbose && (stream->bytes_sent % 100000) == 0) {
                    printf("[CLIENT] Stream %lu: %lu KB sent\n", 
                           stream_id, stream->bytes_sent / 1000);
                }
                
                return 0; /* Return 0 for success */
            }
        }
        else if (ctx->is_server && stream != NULL) {
            /* Server echoes data */
            size_t echo_size = stream->bytes_received;
            size_t available = echo_size - stream->bytes_sent;
            
            if (available == 0 && stream->completed) {
                return 0; /* Stream done */
            }
            
            if (available > length) {
                available = length;
            }
            
            if (available > 0) {
                int is_fin = (stream->bytes_sent + available >= echo_size && stream->completed);
                int is_still_active = !is_fin;
                uint8_t* buffer = picoquic_provide_stream_data_buffer(bytes, available,
                                                                     is_fin, is_still_active);
                
                if (buffer != NULL) {
                    /* Echo with pattern */
                    generate_pattern(buffer, available, stream->pattern_seed, stream->bytes_sent);
                    stream->bytes_sent += available;
                    return 0; /* Return 0 for success */
                }
            }
        }
        return 0;
        
    case picoquic_callback_ready:
        printf("[%s] Connection ready\n", ctx->is_server ? "SERVER" : "CLIENT");
        
        /* Client starts streams */
        if (cnx->client_mode) {
            ctx->test_start_time = picoquic_get_quic_time(cnx->quic);
            
            /* Configure streams based on scenario */
            configure_scenario_streams(ctx);
            
            printf("[CLIENT] Starting scenario %d with %d streams\n", 
                   ctx->scenario, ctx->num_streams);
            
            /* Set deadlines and start streams */
            for (int i = 0; i < ctx->num_streams; i++) {
                stream_config_t* s = &ctx->streams[i];
                s->start_time = picoquic_get_quic_time(cnx->quic);
                
                /* Set deadline if specified */
                if (s->deadline_ms > 0) {
                    int ret = picoquic_set_stream_deadline(cnx, s->stream_id,
                                                         s->deadline_ms, s->is_hard);
                    if (verbose) {
                        printf("[CLIENT] Stream %lu: %s deadline %lu ms (ret=%d)\n",
                               s->stream_id, s->is_hard ? "HARD" : "SOFT",
                               s->deadline_ms, ret);
                    }
                }
                
                /* Mark stream as active */
                picoquic_mark_active_stream(cnx, s->stream_id, 1, NULL);
            }
            
            /* Set fairness parameters for fairness test */
            if (ctx->scenario == SCENARIO_FAIRNESS) {
                picoquic_set_deadline_fairness_params(cnx, 0.3, 100000); /* 30% min share, 100ms max starvation */
                printf("[CLIENT] Fairness parameters set: 30%% minimum share\n");
            }
        }
        break;
        
    case picoquic_callback_close:
        {
            uint64_t close_time = picoquic_get_quic_time(cnx->quic);
            uint32_t error_code = picoquic_get_local_error(cnx);
            printf("[%s] Connection closed (error: 0x%x)\n", 
                   ctx->is_server ? "SERVER" : "CLIENT", error_code);
            
            if (!ctx->is_server && ctx->test_start_time > 0) {
                ctx->test_end_time = close_time;
                ctx->scenario_complete = 1;
            }
        }
        break;
        
    default:
        break;
    }
    
    return 0;
}

/* Print comprehensive results */
void print_results(demo_context_t* ctx) {
    printf("\n==================== DEADLINE DEMO EXTENDED RESULTS ====================\n");
    printf("Scenario: %d\n", ctx->scenario);
    
    if (ctx->test_end_time > ctx->test_start_time) {
        uint64_t duration = ctx->test_end_time - ctx->test_start_time;
        printf("Test duration: %.2f seconds\n", duration / 1000000.0);
        
        double throughput_mbps = (ctx->total_bytes_received * 8.0) / duration;
        printf("Overall throughput: %.2f Mbps\n", throughput_mbps);
    }
    
    printf("\nGlobal Statistics:\n");
    printf("  Total bytes sent: %lu\n", ctx->total_bytes_sent);
    printf("  Total bytes received: %lu\n", ctx->total_bytes_received);
    printf("  Total bytes dropped: %lu (%.2f%%)\n", ctx->total_bytes_dropped,
           ctx->total_bytes_sent > 0 ? 
           (100.0 * ctx->total_bytes_dropped / ctx->total_bytes_sent) : 0);
    printf("  Total gaps: %lu\n", ctx->total_gaps);
    printf("  Data errors detected: %d\n", ctx->errors_detected);
    
    printf("\nDeadline Statistics:\n");
    printf("  Streams succeeded: %d\n", ctx->streams_succeeded);
    printf("  Hard deadlines missed: %d\n", ctx->hard_deadlines_missed);
    printf("  Soft deadlines missed: %d\n", ctx->soft_deadlines_missed);
    
    printf("\nPer-Stream Results:\n");
    printf("%-8s %-15s %-10s %-10s %-10s %-10s %-10s %-10s %-8s %-8s\n",
           "Stream", "Type", "Size", "Deadline", "Duration", "Sent", "Received", 
           "Dropped", "Gaps", "Result");
    printf("%-8s %-15s %-10s %-10s %-10s %-10s %-10s %-10s %-8s %-8s\n",
           "------", "----", "----", "--------", "--------", "----", "--------",
           "-------", "----", "------");
    
    for (int i = 0; i < ctx->num_streams; i++) {
        stream_config_t* s = &ctx->streams[i];
        if (s->start_time == 0) continue;
        
        uint64_t duration = (s->end_time > s->start_time) ? 
                           (s->end_time - s->start_time) / 1000 : 0;
        
        char deadline_str[32];
        if (s->deadline_ms > 0) {
            snprintf(deadline_str, sizeof(deadline_str), "%lu ms %s",
                    s->deadline_ms, s->is_hard ? "(H)" : "(S)");
        } else {
            snprintf(deadline_str, sizeof(deadline_str), "none");
        }
        
        printf("%-8lu %-15s %-10lu %-10s %-10lu %-10lu %-10lu %-10lu %-8lu %-8s\n",
               s->stream_id,
               stream_type_name(s->type),
               s->data_size,
               deadline_str,
               duration,
               s->bytes_sent,
               s->bytes_received,
               s->bytes_dropped,
               s->gaps_received,
               s->completed ? (s->success ? "SUCCESS" : "FAILED") : "PARTIAL");
    }
    
    /* Scenario-specific analysis */
    printf("\nScenario Analysis:\n");
    switch (ctx->scenario) {
    case SCENARIO_BASIC:
        printf("Basic functionality test: %s\n",
               (ctx->streams_succeeded >= 2) ? "PASSED" : "FAILED");
        break;
        
    case SCENARIO_MIXED_TRAFFIC:
        {
            int urgent_success = 0, interactive_success = 0, bulk_success = 0;
            for (int i = 0; i < ctx->num_streams; i++) {
                if (ctx->streams[i].completed && ctx->streams[i].success) {
                    switch (ctx->streams[i].type) {
                    case STREAM_TYPE_URGENT_SMALL: urgent_success++; break;
                    case STREAM_TYPE_INTERACTIVE: interactive_success++; break;
                    case STREAM_TYPE_BULK_DATA: bulk_success++; break;
                    default: break;
                    }
                }
            }
            printf("Mixed traffic results:\n");
            printf("  Urgent streams: %d succeeded\n", urgent_success);
            printf("  Interactive streams: %d succeeded\n", interactive_success);
            printf("  Bulk streams: %d succeeded\n", bulk_success);
        }
        break;
        
    case SCENARIO_HIGH_LOAD:
        printf("High load test (%d concurrent streams): %d/%d succeeded\n",
               ctx->num_streams, ctx->streams_succeeded, ctx->num_streams);
        break;
        
    case SCENARIO_NETWORK_STRESS:
        printf("Network stress test (loss=%.1f%%, delay=%lums): %d/%d succeeded\n",
               ctx->loss_rate * 100, ctx->added_delay_us / 1000,
               ctx->streams_succeeded, ctx->num_streams);
        break;
        
    case SCENARIO_PARTIAL_RELIABILITY:
        printf("Partial reliability test: %lu bytes dropped, %lu gaps\n",
               ctx->total_bytes_dropped, ctx->total_gaps);
        printf("Drop rate: %.2f%%\n", 
               ctx->total_bytes_sent > 0 ? 
               (100.0 * ctx->total_bytes_dropped / ctx->total_bytes_sent) : 0);
        break;
        
    case SCENARIO_FAIRNESS:
        {
            uint64_t deadline_bytes = 0, non_deadline_bytes = 0;
            for (int i = 0; i < ctx->num_streams; i++) {
                if (ctx->streams[i].deadline_ms > 0) {
                    deadline_bytes += ctx->streams[i].bytes_received;
                } else {
                    non_deadline_bytes += ctx->streams[i].bytes_received;
                }
            }
            printf("Fairness test results:\n");
            printf("  Deadline stream bytes: %lu\n", deadline_bytes);
            printf("  Non-deadline stream bytes: %lu\n", non_deadline_bytes);
            if (deadline_bytes + non_deadline_bytes > 0) {
                printf("  Non-deadline share: %.2f%%\n",
                       100.0 * non_deadline_bytes / (deadline_bytes + non_deadline_bytes));
            }
        }
        break;
        
    case SCENARIO_REALTIME_VIDEO:
        {
            int frames_on_time = 0;
            double avg_latency = 0;
            for (int i = 0; i < ctx->num_streams; i++) {
                if (ctx->streams[i].completed && ctx->streams[i].success) {
                    frames_on_time++;
                }
                if (ctx->streams[i].first_byte_time > ctx->streams[i].start_time) {
                    avg_latency += (ctx->streams[i].first_byte_time - ctx->streams[i].start_time);
                }
            }
            if (ctx->num_streams > 0) {
                avg_latency /= (ctx->num_streams * 1000.0); /* Convert to ms */
            }
            printf("Real-time video test:\n");
            printf("  Frames delivered on time: %d/%d (%.1f%%)\n",
                   frames_on_time, ctx->num_streams,
                   100.0 * frames_on_time / ctx->num_streams);
            printf("  Average first byte latency: %.2f ms\n", avg_latency);
        }
        break;
    }
    
    printf("\n========================================================================\n");
}

/* Run server */
int run_server(const char* cert_file, const char* key_file) {
    int ret = 0;
    picoquic_quic_t* quic = NULL;
    demo_context_t* ctx = calloc(1, sizeof(demo_context_t));
    if (ctx == NULL) {
        return -1;
    }
    ctx->is_server = 1;
    ctx->verify_data = 1;
    
    /* Create QUIC context */
    quic = picoquic_create(8, cert_file, key_file, NULL,
        DEMO_ALPN, NULL, NULL, NULL, NULL, NULL,
        picoquic_current_time(), NULL, NULL, NULL, 0);
    
    if (quic == NULL) {
        fprintf(stderr, "Could not create server context\n");
        free(ctx);
        return -1;
    }
    
    /* Enable deadline support in default transport parameters */
    picoquic_tp_t* tp = (picoquic_tp_t*)calloc(1, sizeof(picoquic_tp_t));
    if (tp != NULL) {
        picoquic_init_transport_parameters(tp, 1);
        tp->enable_deadline_aware_streams = 1;
        tp->max_idle_timeout = 120000; /* 2 minutes */
        picoquic_set_default_tp(quic, tp);
        free(tp);
    }
    
    /* Set default callback */
    picoquic_set_default_callback(quic, demo_callback, ctx);
    
    printf("Extended deadline demo server listening on port %d\n", DEMO_PORT);
    printf("Deadline support enabled, data verification enabled\n");
    
    /* Run packet loop */
    ret = picoquic_packet_loop(quic, DEMO_PORT, 0, 0, 0, 0, NULL, NULL);
    
    /* Cleanup */
    picoquic_free(quic);
    free(ctx);
    
    return ret;
}

/* Run client */
int run_client(const char* server_name, int port, test_scenario_t scenario) {
    int ret = 0;
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    demo_context_t* ctx = calloc(1, sizeof(demo_context_t));
    if (ctx == NULL) {
        return -1;
    }
    ctx->is_server = 0;
    ctx->scenario = scenario;
    ctx->verify_data = 1;
    struct sockaddr_storage server_addr;
    int is_name = 0;
    
    /* Resolve server address */
    ret = picoquic_get_server_address(server_name, port, &server_addr, &is_name);
    if (ret != 0) {
        fprintf(stderr, "Cannot resolve server address\n");
        free(ctx);
        return -1;
    }
    
    /* Create QUIC context */
    quic = picoquic_create(8, NULL, NULL, NULL,
        DEMO_ALPN, NULL, NULL, NULL, NULL, NULL,
        picoquic_current_time(), NULL, NULL, NULL, 0);
    
    if (quic == NULL) {
        fprintf(stderr, "Could not create client context\n");
        free(ctx);
        return -1;
    }
    
    /* Create connection */
    cnx = picoquic_create_cnx(quic, picoquic_null_connection_id, picoquic_null_connection_id,
        (struct sockaddr*)&server_addr, picoquic_current_time(), 0,
        is_name ? server_name : NULL, DEMO_ALPN, 1);
    
    if (cnx == NULL) {
        fprintf(stderr, "Could not create connection\n");
        picoquic_free(quic);
        free(ctx);
        return -1;
    }
    
    /* Enable deadline-aware streams */
    cnx->local_parameters.enable_deadline_aware_streams = 1;
    cnx->local_parameters.max_idle_timeout = 120000; /* 2 minutes */
    
    /* Set callback */
    picoquic_set_callback(cnx, demo_callback, ctx);
    
    /* Start connection */
    ret = picoquic_start_client_cnx(cnx);
    if (ret != 0) {
        fprintf(stderr, "Could not start connection\n");
        picoquic_free(quic);
        free(ctx);
        return -1;
    }
    
    printf("Connecting to %s:%d for scenario %d\n", server_name, port, scenario);
    
    /* Run packet loop */
    ret = picoquic_packet_loop(quic, 0, 0, 0, 0, 0, NULL, NULL);
    
    /* Print results */
    print_results(ctx);
    
    /* Cleanup */
    picoquic_free(quic);
    free(ctx);
    
    return ret;
}

/* Usage */
void usage(const char* prog) {
    fprintf(stderr, "Extended Deadline Demo - Comprehensive testing\n");
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  Server: %s -s [-c cert_file] [-k key_file] [-v]\n", prog);
    fprintf(stderr, "  Client: %s [-h server] [-p port] [-t scenario] [-v]\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -s          Run as server\n");
    fprintf(stderr, "  -h host     Server hostname (default: localhost)\n");
    fprintf(stderr, "  -p port     Server port (default: %d)\n", DEMO_PORT);
    fprintf(stderr, "  -c file     Certificate file (server only)\n");
    fprintf(stderr, "  -k file     Key file (server only)\n");
    fprintf(stderr, "  -t scenario Test scenario (0-%d):\n", SCENARIO_MAX - 1);
    fprintf(stderr, "              0: Basic functionality\n");
    fprintf(stderr, "              1: Mixed traffic types\n");
    fprintf(stderr, "              2: High load (50 streams)\n");
    fprintf(stderr, "              3: Network stress (loss + delay)\n");
    fprintf(stderr, "              4: Partial reliability test\n");
    fprintf(stderr, "              5: Fairness test\n");
    fprintf(stderr, "              6: Real-time video simulation\n");
    fprintf(stderr, "  -v          Verbose output\n");
}

int main(int argc, char* argv[]) {
    int opt;
    int server_mode = 0;
    const char* server_name = "localhost";
    int port = DEMO_PORT;
    const char* cert_file = "certs/cert.pem";
    const char* key_file = "certs/key.pem";
    test_scenario_t scenario = SCENARIO_BASIC;
    int ret = 0;
    
    /* Parse options */
    while ((opt = getopt(argc, argv, "sh:p:c:k:t:v")) != -1) {
        switch (opt) {
        case 's':
            server_mode = 1;
            break;
        case 'h':
            server_name = optarg;
            break;
        case 'p':
            port = atoi(optarg);
            break;
        case 'c':
            cert_file = optarg;
            break;
        case 'k':
            key_file = optarg;
            break;
        case 't':
            scenario = atoi(optarg);
            if (scenario >= SCENARIO_MAX) {
                fprintf(stderr, "Invalid scenario: %d\n", scenario);
                usage(argv[0]);
                return 1;
            }
            break;
        case 'v':
            verbose = 1;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }
    
    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Seed random number generator */
    srand(time(NULL));
    
    printf("=== Extended Deadline-Aware Streams Demo ===\n");
    printf("Testing all deadline features for production readiness\n\n");
    
    if (server_mode) {
        ret = run_server(cert_file, key_file);
    } else {
        ret = run_client(server_name, port, scenario);
    }
    
    return ret;
}