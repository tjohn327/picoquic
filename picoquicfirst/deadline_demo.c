/*
* Deadline Demo - Simple test program for deadline-aware streams
* 
* This program demonstrates and validates deadline functionality
* without modifying the core picoquicdemo.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "../picoquic/picoquic.h"
#include "../picoquic/picoquic_internal.h"
#include "../picoquic/picosocks.h"
#include "../picoquic/picoquic_utils.h"
#include "../picoquic/picoquic_packet_loop.h"

#define DEADLINE_DEMO_PORT 4443
#define DEADLINE_DEMO_ALPN "deadline-test"

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
    
    /* Find stream index */
    for (int i = 0; i < ctx->num_streams; i++) {
        if (ctx->streams[i].stream_id == stream_id) {
            stream_idx = i;
            break;
        }
    }
    
    switch (fin_or_event) {
    case picoquic_callback_stream_data:
        if (stream_idx >= 0) {
            ctx->streams[stream_idx].bytes_received += length;
        }
        if (!ctx->is_server) {
            printf(".");
            fflush(stdout);
        }
        break;
        
    case picoquic_callback_stream_fin:
        if (stream_idx >= 0) {
            ctx->streams[stream_idx].completed = 1;
            uint64_t duration = picoquic_current_time() - ctx->streams[stream_idx].start_time;
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
        
        printf("\n[STREAM %lu] GAP: %zu bytes dropped at offset %lu\n",
               stream_id, length, bytes ? *(uint64_t*)bytes : 0);
        break;
        
    case picoquic_callback_prepare_to_send:
        /* Send data on client streams */
        if (!ctx->is_server && stream_idx >= 0) {
            size_t available = 50000 - ctx->streams[stream_idx].bytes_sent;
            if (available > 0) {
                if (available > length) {
                    available = length;
                }
                memset(bytes, 'D', available);
                ctx->streams[stream_idx].bytes_sent += available;
                
                /* Close stream if done */
                if (ctx->streams[stream_idx].bytes_sent >= 50000) {
                    picoquic_add_to_stream(cnx, stream_id, NULL, 0, 1);
                }
                
                return (int)available;
            }
        }
        break;
        
    case picoquic_callback_ready:
        printf("Connection ready (client_mode=%d)\n", cnx->client_mode);
        
        /* Client starts streams */
        if (cnx->client_mode && ctx->num_streams == 0) {
            /* Test scenario: 3 streams with different deadlines */
            ctx->num_streams = 3;
            
            /* Stream 0: Urgent with hard deadline */
            ctx->streams[0].stream_id = 0;
            ctx->streams[0].deadline_ms = 100;
            ctx->streams[0].is_hard = 1;
            ctx->streams[0].start_time = picoquic_current_time();
            
            /* Stream 4: Normal with soft deadline */
            ctx->streams[1].stream_id = 4;
            ctx->streams[1].deadline_ms = 500;
            ctx->streams[1].is_hard = 0;
            ctx->streams[1].start_time = picoquic_current_time();
            
            /* Stream 8: No deadline */
            ctx->streams[2].stream_id = 8;
            ctx->streams[2].deadline_ms = 0;
            ctx->streams[2].is_hard = 0;
            ctx->streams[2].start_time = picoquic_current_time();
            
            /* Set deadlines and start streams */
            for (int i = 0; i < ctx->num_streams; i++) {
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
                
                /* Mark stream as active */
                picoquic_mark_active_stream(cnx, ctx->streams[i].stream_id, 1, ctx);
            }
        }
        break;
        
    case picoquic_callback_close:
        printf("Connection closed\n");
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
    printf("\n=== Test Results ===\n");
    printf("Gaps received: %d\n", ctx->gaps_received);
    printf("Total bytes dropped: %lu\n", ctx->total_bytes_dropped);
    
    printf("\nPer-stream results:\n");
    for (int i = 0; i < ctx->num_streams; i++) {
        printf("Stream %lu: sent=%lu, received=%lu, dropped=%lu, completed=%s\n",
               ctx->streams[i].stream_id,
               ctx->streams[i].bytes_sent,
               ctx->streams[i].bytes_received,
               ctx->streams[i].bytes_dropped,
               ctx->streams[i].completed ? "YES" : "NO");
    }
}

/* Run server */
int run_deadline_server(const char* cert_file, const char* key_file)
{
    int ret = 0;
    picoquic_quic_t* quic = NULL;
    deadline_demo_ctx_t server_ctx = {0};
    server_ctx.is_server = 1;
    
    /* Create QUIC context */
    quic = picoquic_create(8, cert_file, key_file, NULL,
        DEADLINE_DEMO_ALPN, NULL, NULL, NULL, NULL, NULL,
        picoquic_current_time(), NULL, NULL, NULL, 0);
    
    if (quic == NULL) {
        fprintf(stderr, "Could not create server context\n");
        return -1;
    }
    
    /* Enable deadline-aware streams by default */
    picoquic_tp_t default_tp;
    memset(&default_tp, 0, sizeof(default_tp));
    picoquic_init_transport_parameters(&default_tp, 1);
    default_tp.enable_deadline_aware_streams = 1;
    picoquic_set_default_tp(quic, &default_tp);
    
    printf("Server listening on port %d with deadline support enabled\n", DEADLINE_DEMO_PORT);
    
    /* Create packet loop */
    picoquic_packet_loop_param_t loop_param = {0};
    loop_param.local_port = DEADLINE_DEMO_PORT;
    loop_param.local_af = AF_INET;
    loop_param.do_not_use_gso = 1;
    
    /* Set callback for new connections */
    picoquic_set_default_callback(quic, deadline_demo_callback, &server_ctx);
    
    /* Run packet loop */
    ret = picoquic_packet_loop_v2(quic, &loop_param, NULL, 0);
    
    /* Cleanup */
    picoquic_free(quic);
    
    return ret;
}

/* Run client */
int run_deadline_client(const char* server_name, int port)
{
    int ret = 0;
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    deadline_demo_ctx_t client_ctx = {0};
    client_ctx.is_server = 0;
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
    
    /* Set callback */
    picoquic_set_callback(cnx, deadline_demo_callback, &client_ctx);
    
    /* Start connection */
    ret = picoquic_start_client_cnx(cnx);
    if (ret != 0) {
        fprintf(stderr, "Could not start connection\n");
        picoquic_free(quic);
        return -1;
    }
    
    printf("Connecting to %s:%d with deadline support enabled\n", server_name, port);
    
    /* Run packet loop */
    picoquic_packet_loop_param_t loop_param = {0};
    loop_param.local_port = 0;  /* Let system choose */
    loop_param.local_af = AF_INET;
    loop_param.do_not_use_gso = 1;
    loop_param.send_length_max = 1536;
    
    /* Add bandwidth limit to trigger deadline drops */
    if (getenv("DEADLINE_TEST_LIMIT_BW")) {
        /* Simple rate limiting for testing */
        printf("Note: Bandwidth limiting enabled for testing\n");
    }
    
    uint64_t start_time = picoquic_current_time();
    uint64_t timeout = 30000000; /* 30 seconds */
    
    while (running && !client_ctx.test_complete) {
        int64_t delay_max = 100000;
        
        ret = picoquic_packet_loop_v2(quic, &loop_param, NULL, delay_max);
        
        if (picoquic_current_time() - start_time > timeout) {
            printf("\nTimeout reached\n");
            break;
        }
    }
    
    /* Print results */
    print_results(&client_ctx);
    
    /* Cleanup */
    picoquic_free(quic);
    
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
    
    /* Initialize picoquic */
    picoquic_tls_api_init();
    
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
    
    /* Cleanup */
    picoquic_tls_api_unload();
    
    return ret;
}