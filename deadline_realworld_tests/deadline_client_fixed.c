/*
 * Fixed QUIC client with deadline-aware stream testing
 * Based on picoquic sample client pattern
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <picoquic.h>
#include <picoquic_utils.h>
#include <picosocks.h>
#include <autoqlog.h>
#include <picoquic_packet_loop.h>

#ifdef _WINDOWS
#include <WinSock2.h>
#include <Windows.h>
#endif

typedef struct st_test_stream_t {
    uint64_t stream_id;
    size_t chunk_size;
    int chunk_interval_ms;
    int nb_chunks;
    int chunks_sent;
    uint64_t deadline_ms;
    int64_t start_time;
    int64_t next_send_time;
    uint8_t* send_buffer;
    int is_active;
} test_stream_t;

typedef struct st_client_ctx_t {
    picoquic_cnx_t* cnx;
    int nb_streams;
    test_stream_t* streams;
    int all_streams_done;
    size_t total_bytes_sent;
    int64_t test_start_time;
    int is_disconnected;
} client_ctx_t;

static int send_test_chunk(picoquic_cnx_t* cnx, test_stream_t* stream)
{
    int ret = 0;
    
    /* Fill buffer with test pattern */
    for (size_t i = 0; i < stream->chunk_size; i++) {
        stream->send_buffer[i] = (uint8_t)((stream->chunks_sent + i) & 0xFF);
    }

    /* Send chunk */
    ret = picoquic_add_to_stream(cnx, stream->stream_id, 
        stream->send_buffer, stream->chunk_size, 
        (stream->chunks_sent + 1 >= stream->nb_chunks) ? 1 : 0);

    if (ret == 0) {
        stream->chunks_sent++;
        printf("Client: Stream %lu sent chunk %d/%d (%zu bytes)\n",
            stream->stream_id, stream->chunks_sent, stream->nb_chunks, stream->chunk_size);
        
        if (stream->chunks_sent < stream->nb_chunks) {
            stream->next_send_time = picoquic_current_time() + 
                (int64_t)stream->chunk_interval_ms * 1000;
        } else {
            stream->is_active = 0;
        }
    }

    return ret;
}

/* Callback for packet loop to check if we need to send more data */
static int client_loop_cb(picoquic_quic_t* quic, picoquic_packet_loop_cb_enum cb_mode, 
    void* callback_ctx, void * callback_arg)
{
    int ret = 0;
    client_ctx_t* ctx = (client_ctx_t*)callback_ctx;
    
    if (ctx == NULL) {
        ret = -1;
    } else if (cb_mode == picoquic_packet_loop_after_receive || cb_mode == picoquic_packet_loop_after_send) {
        int64_t current_time = picoquic_current_time();
        
        /* Check if we need to send more chunks */
        for (int i = 0; i < ctx->nb_streams; i++) {
            test_stream_t* stream = &ctx->streams[i];
            if (stream->is_active && stream->chunks_sent < stream->nb_chunks && 
                current_time >= stream->next_send_time) {
                send_test_chunk(ctx->cnx, stream);
            }
        }
        
        /* Check if all streams are done */
        int all_done = 1;
        for (int i = 0; i < ctx->nb_streams; i++) {
            if (ctx->streams[i].is_active) {
                all_done = 0;
                break;
            }
        }
        
        if (all_done && !ctx->all_streams_done) {
            ctx->all_streams_done = 1;
            int64_t test_duration = current_time - ctx->test_start_time;
            printf("\nClient: All streams completed in %.3f ms\n", 
                test_duration / 1000.0);
            
            /* Signal that we want to close */
            picoquic_close(ctx->cnx, 0);
        }
    }
    
    /* Continue running while not disconnected */
    return ctx->is_disconnected ? -1 : 0;
}

static int client_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{
    client_ctx_t* ctx = (client_ctx_t*)callback_ctx;
    int ret = 0;

    switch (fin_or_event) {
    case picoquic_callback_ready:
        printf("Client: Connection ready\n");
        printf("Client: Starting to send on deadline streams\n");
        
        /* Start sending on all test streams */
        for (int i = 0; i < ctx->nb_streams; i++) {
            test_stream_t* stream = &ctx->streams[i];
            
            /* Create stream - IDs must be 4 or higher */
            stream->stream_id = (uint64_t)(4 + i * 4);
            
            /* Set deadline if specified */
            if (stream->deadline_ms > 0) {
                ret = picoquic_set_stream_deadline(cnx, stream->stream_id, 
                    stream->deadline_ms, 1);
                if (ret == 0) {
                    printf("Client: Set deadline %lu ms on stream %lu\n",
                        stream->deadline_ms, stream->stream_id);
                } else {
                    printf("Client: Failed to set deadline on stream %lu\n",
                        stream->stream_id);
                }
            }
            
            /* Send first chunk */
            stream->start_time = picoquic_current_time();
            stream->is_active = 1;
            ret = send_test_chunk(cnx, stream);
            if (ret != 0) {
                printf("Client: Failed to send first chunk on stream %lu\n",
                    stream->stream_id);
            }
        }
        ctx->test_start_time = picoquic_current_time();
        break;

    case picoquic_callback_close:
    case picoquic_callback_application_close:
        printf("Client: Connection closed\n");
        ctx->is_disconnected = 1;
        break;

    default:
        break;
    }

    return ret;
}

static void run_test_simple(client_ctx_t* ctx)
{
    /* Single stream test: 100ms deadline, 1000-byte chunks every 10ms */
    ctx->nb_streams = 1;
    ctx->streams = (test_stream_t*)calloc(1, sizeof(test_stream_t));
    
    ctx->streams[0].chunk_size = 1000;
    ctx->streams[0].chunk_interval_ms = 10;
    ctx->streams[0].nb_chunks = 20;  /* 20 chunks over 200ms */
    ctx->streams[0].deadline_ms = 100;
    ctx->streams[0].send_buffer = (uint8_t*)malloc(ctx->streams[0].chunk_size);
    
    printf("\n=== Simple Test Configuration ===\n");
    printf("1 stream with 100ms deadline\n");
    printf("Sending 20 chunks of 1000 bytes every 10ms\n");
    printf("Total duration: ~200ms\n");
    printf("================================\n\n");
}

int main(int argc, char** argv)
{
    int ret = 0;
    const char* server_name = "localhost";
    int server_port = 4443;
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    client_ctx_t client_ctx = { 0 };
    struct sockaddr_storage server_address;
    int64_t current_time;

#ifdef _WINDOWS
    WSADATA wsaData = { 0 };
    (void)WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    if (argc > 1) {
        server_name = argv[1];
    }
    if (argc > 2) {
        server_port = atoi(argv[2]);
    }

    printf("Connecting to deadline-aware QUIC server at %s:%d\n", 
        server_name, server_port);

    /* Get server address */
    int is_name = 0;
    ret = picoquic_get_server_address(server_name, server_port, 
        &server_address, &is_name);
    if (ret != 0) {
        printf("Cannot resolve server address\n");
        goto finish;
    }

    /* Create QUIC context */
    current_time = picoquic_current_time();
    quic = picoquic_create(1,
        NULL,
        NULL,
        NULL,
        "deadline-test",
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        current_time,
        NULL,
        NULL,
        NULL,
        0);

    if (quic == NULL) {
        printf("Could not create QUIC context\n");
        ret = -1;
        goto finish;
    }

    /* Enable deadline support via default transport parameters */
    const picoquic_tp_t* old_tp = picoquic_get_default_tp(quic);
    picoquic_tp_t new_tp = *old_tp;
    new_tp.enable_deadline_aware_streams = 1;
    picoquic_set_default_tp(quic, &new_tp);
    printf("Client: Deadline-aware streams enabled\n");

    /* Set congestion algorithm */
    picoquic_set_default_congestion_algorithm(quic, picoquic_get_congestion_algorithm("bbr"));

    /* Create connection */
    cnx = picoquic_create_cnx(quic,
        picoquic_null_connection_id,
        picoquic_null_connection_id,
        (struct sockaddr*)&server_address,
        current_time,
        0,
        server_name,
        "deadline-test",
        1);

    if (cnx == NULL) {
        printf("Could not create connection\n");
        ret = -1;
        goto finish;
    }

    /* Set connection parameters */
    picoquic_set_callback(cnx, client_callback, &client_ctx);
    client_ctx.cnx = cnx;

    /* Setup test configuration */
    run_test_simple(&client_ctx);

    /* Start connection */
    ret = picoquic_start_client_cnx(cnx);
    if (ret != 0) {
        printf("Could not start connection\n");
        goto finish;
    }

    /* Run event loop with callback */
    ret = picoquic_packet_loop(quic, 0, server_address.ss_family, 0, 0, 0, 
        client_loop_cb, &client_ctx);

    printf("\nClient test completed\n");

finish:
    /* Cleanup */
    if (client_ctx.streams != NULL) {
        for (int i = 0; i < client_ctx.nb_streams; i++) {
            if (client_ctx.streams[i].send_buffer != NULL) {
                free(client_ctx.streams[i].send_buffer);
            }
        }
        free(client_ctx.streams);
    }

    if (quic != NULL) {
        picoquic_free(quic);
    }

#ifdef _WINDOWS
    WSACleanup();
#endif

    return ret;
}