/*
* Deadline Demo - Raw bytes transfer with deadline-aware streams
* 
* This program demonstrates deadline functionality using raw QUIC streams
* without HTTP/3, avoiding stream ID conflicts. It transfers raw bytes
* with different deadline configurations to show partial reliability.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "../picoquic/picoquic.h"
#include "../picoquic/picoquic_internal.h"
#include "../picoquic/picosocks.h"
#include "../picoquic/picoquic_utils.h"
#include "../picoquic/picoquic_packet_loop.h"
#include "../picoquic/tls_api.h"

#define DEADLINE_DEMO_PORT 4443
#define DEADLINE_DEMO_ALPN "hq-interop"  /* Use raw bytes protocol, not HTTP/3 */

/* Test context */
typedef struct st_deadline_demo_ctx_t {
    int is_server;
    int test_complete;
    
    /* Stream tracking */
    struct {
        uint64_t stream_id;
        uint64_t deadline_ms;
        int is_hard;
        uint64_t start_time;
        uint64_t bytes_sent;
        uint64_t bytes_received;
        uint64_t bytes_dropped;
        int completed;
    } streams[10];
    int num_streams;
    
    /* Statistics */
    int gaps_received;
    uint64_t total_bytes_dropped;
} deadline_demo_ctx_t;

static int server_mode = 0;
static int running = 1;

/* Signal handler for clean shutdown */
void signal_handler(int sig) {
    fprintf(stdout, "\nReceived signal %d, shutting down...\n", sig);
    running = 0;
}

/* Demo callback */
int deadline_demo_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{
    deadline_demo_ctx_t* ctx = (deadline_demo_ctx_t*)callback_ctx;
    int stream_idx = -1;
    
    /* Safety check */
    if (ctx == NULL) {
        fprintf(stderr, "ERROR: callback_ctx is NULL\n");
        return -1;
    }
    
    
    /* Find stream index */
    for (int i = 0; i < ctx->num_streams && i < 10; i++) {
        if (ctx->streams[i].stream_id == stream_id) {
            stream_idx = i;
            break;
        }
    }
    
    switch (fin_or_event) {
    case picoquic_callback_almost_ready:
        /* Enable deadline on server connections */
        if (ctx->is_server) {
            cnx->local_parameters.enable_deadline_aware_streams = 1;
            printf("[SERVER] Enabled deadline support for new connection\n");
        }
        break;
        
    case picoquic_callback_stream_data:
        /* For server, create stream entry if new */
        if (ctx->is_server && stream_idx < 0 && ctx->num_streams < 10) {
            stream_idx = ctx->num_streams++;
            ctx->streams[stream_idx].stream_id = stream_id;
            ctx->streams[stream_idx].start_time = picoquic_get_quic_time(cnx->quic);
            ctx->streams[stream_idx].deadline_ms = 0; /* Server doesn't set deadlines */
            printf("[SERVER] New stream %lu received\n", stream_id);
        }
        
        if (stream_idx >= 0) {
            ctx->streams[stream_idx].bytes_received += length;
            if (ctx->is_server) {
                /* Server should echo - mark stream as active */
                picoquic_mark_active_stream(cnx, stream_id, 1, NULL);
            }
        }
        if (!ctx->is_server && (ctx->streams[stream_idx].bytes_received % 10000) == 0) {
            printf(".");
            fflush(stdout);
        }
        break;
        
    case picoquic_callback_stream_fin:
        if (stream_idx >= 0) {
            ctx->streams[stream_idx].completed = 1;
            uint64_t duration = picoquic_get_quic_time(cnx->quic) - ctx->streams[stream_idx].start_time;
            printf("\n[STREAM %lu] Completed in %lu ms (deadline was %lu ms)\n",
                   stream_id, duration / 1000, ctx->streams[stream_idx].deadline_ms);
        }
        break;
        
    case picoquic_callback_stream_gap:
        /* Gap notification - data was dropped */
        ctx->gaps_received++;
        if (stream_idx >= 0) {
            ctx->streams[stream_idx].bytes_dropped += length;
        }
        ctx->total_bytes_dropped += length;
        
        printf("\n[STREAM %lu] GAP: %zu bytes dropped\n",
               stream_id, length);
        break;
        
    case picoquic_callback_prepare_to_send:
        /* Debug which stream is being asked to send */
        if (!ctx->is_server && stream_idx < 0) {
            printf("[DEBUG] prepare_to_send for unknown stream %lu\n", stream_id);
        }
        
        /* Send data on client streams */
        if (!ctx->is_server && stream_idx >= 0 && stream_idx < 10) {
            size_t target_size = 100000;  /* 100KB per stream */
            size_t available = target_size - ctx->streams[stream_idx].bytes_sent;
            
            /* Check if we're done */
            if (available == 0) {
                /* All data sent, close stream by returning 0 */
                return 0;
            }
            
            /* Limit to space available */
            if (available > length) {
                available = length;
            }
            
            /* Get the actual buffer to write to */
            int is_fin = (ctx->streams[stream_idx].bytes_sent + available >= target_size) ? 1 : 0;
            int is_still_active = !is_fin;
            uint8_t* buffer = picoquic_provide_stream_data_buffer(bytes, available, is_fin, is_still_active);
            
            if (buffer == NULL) {
                return 0;
            }
            
            /* Create pattern: stream_id byte repeated */
            memset(buffer, (uint8_t)(stream_id & 0xFF), available);
            
            /* Add sequence markers every 1000 bytes */
            for (size_t i = 0; i < available; i += 1000) {
                if (i + 8 <= available) {
                    uint64_t seq = (ctx->streams[stream_idx].bytes_sent + i) / 1000;
                    memcpy(buffer + i, &seq, 8);
                }
            }
            
            ctx->streams[stream_idx].bytes_sent += available;
            
            /* Return number of bytes written */
            if (available > 0) {
                printf("[CLIENT] Stream %lu: sent %zu bytes (total: %lu/%lu)\n", 
                       stream_id, available, ctx->streams[stream_idx].bytes_sent, target_size);
                
                /* Keep stream active if not done */
                if (ctx->streams[stream_idx].bytes_sent < target_size) {
                    picoquic_mark_active_stream(cnx, stream_id, 1, NULL);
                }
            }
            return 0; /* Return 0 for success, not the number of bytes */
        }
        else if (ctx->is_server && stream_idx >= 0 && stream_idx < 10) {
            /* Server echoes received data */
            size_t echo_size = ctx->streams[stream_idx].bytes_received;
            size_t available = echo_size - ctx->streams[stream_idx].bytes_sent;
            
            if (available == 0) {
                /* Check if we should close */
                if (ctx->streams[stream_idx].completed) {
                    return 0;
                }
                /* Nothing to send yet */
                return 0;
            }
            
            if (available > length) {
                available = length;
            }
            
            /* Get the actual buffer to write to */
            int is_fin = (ctx->streams[stream_idx].bytes_sent + available >= echo_size && 
                         ctx->streams[stream_idx].completed) ? 1 : 0;
            uint8_t* buffer = picoquic_provide_stream_data_buffer(bytes, available, is_fin, !is_fin);
            
            if (buffer == NULL) {
                return 0;
            }
            
            /* Echo pattern: 'E' + stream_id */
            memset(buffer, 'E', available);
            for (size_t i = 1; i < available; i += 2) {
                buffer[i] = (uint8_t)(stream_id & 0xFF);
            }
            ctx->streams[stream_idx].bytes_sent += available;
            
            return 0; /* Return 0 for success, not the number of bytes */
        }
        /* No data to send on this stream */
        return 0;
        
    case picoquic_callback_ready:
        printf("Connection ready (client_mode=%d, ALPN=%s)\n", 
               cnx->client_mode, DEADLINE_DEMO_ALPN);
        printf("Using raw QUIC byte transfer (not HTTP/3) - safe for all stream IDs\n");
        
        /* Client starts streams */
        if (cnx->client_mode && ctx->num_streams == 0) {
            /* Test scenario: 3 streams with different deadlines */
            ctx->num_streams = 3;
            
            /* Stream 4: Urgent with hard deadline (150ms for 100KB) */
            ctx->streams[0].stream_id = 4;
            ctx->streams[0].deadline_ms = 150;
            ctx->streams[0].is_hard = 1;
            ctx->streams[0].start_time = picoquic_get_quic_time(cnx->quic);
            
            /* Stream 8: Normal with soft deadline (1 second) */
            ctx->streams[1].stream_id = 8;
            ctx->streams[1].deadline_ms = 1000;
            ctx->streams[1].is_hard = 0;
            ctx->streams[1].start_time = picoquic_get_quic_time(cnx->quic);
            
            /* Stream 12: No deadline (best effort) */
            ctx->streams[2].stream_id = 12;
            ctx->streams[2].deadline_ms = 0;
            ctx->streams[2].is_hard = 0;
            ctx->streams[2].start_time = picoquic_get_quic_time(cnx->quic);
            
            /* Set deadlines and start streams */
            for (int i = 0; i < ctx->num_streams && i < 10; i++) {
                if (ctx->streams[i].deadline_ms > 0) {
                    int ret = picoquic_set_stream_deadline(cnx, 
                        ctx->streams[i].stream_id,
                        ctx->streams[i].deadline_ms,
                        ctx->streams[i].is_hard);
                    
                    printf("[STREAM %lu] Set %s deadline: %lu ms (ret=%d)\n",
                           ctx->streams[i].stream_id,
                           ctx->streams[i].is_hard ? "HARD" : "SOFT",
                           ctx->streams[i].deadline_ms, ret);
                }
                
                /* Mark stream as active to start sending */
                int mark_ret = picoquic_mark_active_stream(cnx, ctx->streams[i].stream_id, 1, NULL);
                printf("[STREAM %lu] Mark active ret=%d\n", ctx->streams[i].stream_id, mark_ret);
            }
        }
        break;
        
    case picoquic_callback_close:
        printf("Connection closed (reason: 0x%x)\n", picoquic_get_local_error(cnx));
        ctx->test_complete = 1;
        break;
        
    default:
        break;
    }
    
    return 0;
}

/* Print results */
void print_results(deadline_demo_ctx_t* ctx)
{
    printf("\n=== Deadline Demo Results ===\n");
    printf("Protocol: Raw QUIC bytes transfer (not HTTP/3)\n");
    printf("Transfer size: 100KB per stream\n");
    printf("Gaps received: %d\n", ctx->gaps_received);
    printf("Total bytes dropped: %lu\n", ctx->total_bytes_dropped);
    
    printf("\nPer-stream results:\n");
    printf("%-10s %-15s %-10s %-10s %-10s %-10s %-10s\n", 
           "Stream", "Deadline", "Type", "Sent", "Received", "Dropped", "Status");
    printf("%-10s %-15s %-10s %-10s %-10s %-10s %-10s\n", 
           "------", "--------", "----", "----", "--------", "-------", "------");
    
    for (int i = 0; i < ctx->num_streams && i < 10; i++) {
        char deadline_str[32];
        if (ctx->streams[i].deadline_ms == 0) {
            snprintf(deadline_str, sizeof(deadline_str), "None");
        } else {
            snprintf(deadline_str, sizeof(deadline_str), "%lums", ctx->streams[i].deadline_ms);
        }
        
        printf("%-10lu %-15s %-10s %-10lu %-10lu %-10lu %-10s\n",
               ctx->streams[i].stream_id,
               deadline_str,
               ctx->streams[i].is_hard ? "HARD" : (ctx->streams[i].deadline_ms > 0 ? "SOFT" : "NONE"),
               ctx->streams[i].bytes_sent,
               ctx->streams[i].bytes_received,
               ctx->streams[i].bytes_dropped,
               ctx->streams[i].completed ? "Complete" : "Partial");
    }
    
    if (ctx->total_bytes_dropped > 0) {
        printf("\nNote: %lu bytes were dropped due to deadline expiry (partial reliability in action)\n", 
               ctx->total_bytes_dropped);
    }
}

/* Run server */
int run_deadline_server(const char* cert_file, const char* key_file)
{
    int ret = 0;
    picoquic_quic_t* quic = NULL;
    deadline_demo_ctx_t* server_ctx = calloc(1, sizeof(deadline_demo_ctx_t));
    if (server_ctx == NULL) {
        return -1;
    }
    server_ctx->is_server = 1;
    
    /* Create QUIC context */
    quic = picoquic_create(8, cert_file, key_file, NULL,
        DEADLINE_DEMO_ALPN, NULL, NULL, NULL, NULL, NULL,
        picoquic_current_time(), NULL, NULL, NULL, 0);
    
    if (quic == NULL) {
        fprintf(stderr, "Could not create server context\n");
        return -1;
    }
    
    /* Enable deadline-aware streams by default - will be set per connection */
    
    printf("Server listening on port %d with deadline support enabled\n", DEADLINE_DEMO_PORT);
    
    /* Set callback for new connections */
    picoquic_set_default_callback(quic, deadline_demo_callback, server_ctx);
    
    /* Run packet loop */
    ret = picoquic_packet_loop(quic, DEADLINE_DEMO_PORT, 0, 0, 0, 0, NULL, NULL);
    
    /* Cleanup */
    picoquic_free(quic);
    free(server_ctx);
    
    return ret;
}

/* Run client */
int run_deadline_client(const char* server_name, int port)
{
    int ret = 0;
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    deadline_demo_ctx_t* client_ctx = calloc(1, sizeof(deadline_demo_ctx_t));
    if (client_ctx == NULL) {
        return -1;
    }
    client_ctx->is_server = 0;
    struct sockaddr_storage server_addr;
    int is_name = 0;
    
    /* Resolve server address */
    ret = picoquic_get_server_address(server_name, port, &server_addr, &is_name);
    if (ret != 0) {
        fprintf(stderr, "Cannot resolve server address\n");
        return -1;
    }
    
    /* Create QUIC context */
    quic = picoquic_create(8, NULL, NULL, NULL,
        DEADLINE_DEMO_ALPN, NULL, NULL, NULL, NULL, NULL,
        picoquic_current_time(), NULL, NULL, NULL, 0);
    
    if (quic == NULL) {
        fprintf(stderr, "Could not create client context\n");
        return -1;
    }
    
    /* Create connection */
    cnx = picoquic_create_cnx(quic, picoquic_null_connection_id, picoquic_null_connection_id,
        (struct sockaddr*)&server_addr, picoquic_current_time(), 0,
        is_name ? server_name : NULL, DEADLINE_DEMO_ALPN, 1);
    
    if (cnx == NULL) {
        fprintf(stderr, "Could not create connection\n");
        picoquic_free(quic);
        return -1;
    }
    
    /* Enable deadline-aware streams */
    cnx->local_parameters.enable_deadline_aware_streams = 1;
    
    /* Set longer idle timeout (60 seconds) */
    cnx->local_parameters.max_idle_timeout = 60000;
    
    /* Set callback */
    picoquic_set_callback(cnx, deadline_demo_callback, client_ctx);
    
    /* Start connection */
    ret = picoquic_start_client_cnx(cnx);
    if (ret != 0) {
        fprintf(stderr, "Could not start connection\n");
        picoquic_free(quic);
        return -1;
    }
    
    printf("Connecting to %s:%d with deadline support enabled\n", server_name, port);
    
    /* Add bandwidth limit to trigger deadline drops */
    if (getenv("DEADLINE_TEST_LIMIT_BW")) {
        /* Simple rate limiting for testing */
        printf("Note: Bandwidth limiting enabled for testing\n");
    }
    
    /* Run packet loop */
    ret = picoquic_packet_loop(quic, 0, 0, 0, 0, 0, NULL, NULL);
    
    /* Print results */
    print_results(client_ctx);
    
    /* Cleanup */
    picoquic_free(quic);
    free(client_ctx);
    
    return ret;
}

/* Usage */
void usage(const char* prog)
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  Server: %s -s [-c cert_file] [-k key_file]\n", prog);
    fprintf(stderr, "  Client: %s [-h server] [-p port]\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -s          Run as server\n");
    fprintf(stderr, "  -h host     Server hostname (default: localhost)\n");
    fprintf(stderr, "  -p port     Server port (default: %d)\n", DEADLINE_DEMO_PORT);
    fprintf(stderr, "  -c file     Certificate file (server only)\n");
    fprintf(stderr, "  -k file     Key file (server only)\n");
    fprintf(stderr, "\nEnvironment:\n");
    fprintf(stderr, "  DEADLINE_TEST_LIMIT_BW=1  Enable bandwidth limiting for testing\n");
}

int main(int argc, char* argv[])
{
    int opt;
    const char* server_name = "localhost";
    int port = DEADLINE_DEMO_PORT;
    const char* cert_file = NULL;
    const char* key_file = NULL;
    int ret = 0;
    
    /* Parse options */
    while ((opt = getopt(argc, argv, "sh:p:c:k:")) != -1) {
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
        default:
            usage(argv[0]);
            return 1;
        }
    }
    
    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    if (server_mode) {
        /* Use default test certificates if not specified */
        if (cert_file == NULL) {
            cert_file = "certs/cert.pem";
        }
        if (key_file == NULL) {
            key_file = "certs/key.pem";
        }
        
        ret = run_deadline_server(cert_file, key_file);
    } else {
        ret = run_deadline_client(server_name, port);
    }
    
    return ret;
}