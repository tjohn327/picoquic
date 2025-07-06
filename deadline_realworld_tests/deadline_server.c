/*
 * Simple QUIC server with deadline-aware stream support
 * Tests deadline negotiation and stream handling
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <picoquic.h>
#include <picoquic_utils.h>
#include <picosocks.h>
#include <autoqlog.h>
#include <picoquic_packet_loop.h>

#ifdef _WINDOWS
#include <WinSock2.h>
#include <Windows.h>
#endif

static int server_running = 1;

void signal_handler(int sig) {
    server_running = 0;
}

typedef struct st_server_stream_ctx_t {
    uint64_t stream_id;
    size_t bytes_received;
    int64_t first_byte_time;
    int64_t last_byte_time;
    int has_deadline;
    uint64_t deadline_ms;
} server_stream_ctx_t;

typedef struct st_server_ctx_t {
    int nb_streams;
    int nb_deadline_streams;
    size_t total_bytes_received;
} server_ctx_t;

static int server_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{
    server_ctx_t* ctx = (server_ctx_t*)callback_ctx;
    server_stream_ctx_t* stream_ctx = (server_stream_ctx_t*)v_stream_ctx;
    int ret = 0;

    switch (fin_or_event) {
    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin:
        if (stream_ctx == NULL) {
            stream_ctx = (server_stream_ctx_t*)malloc(sizeof(server_stream_ctx_t));
            if (stream_ctx == NULL) {
                ret = -1;
                break;
            }
            memset(stream_ctx, 0, sizeof(server_stream_ctx_t));
            stream_ctx->stream_id = stream_id;
            stream_ctx->first_byte_time = picoquic_current_time();
            picoquic_set_app_stream_ctx(cnx, stream_id, stream_ctx);
            ctx->nb_streams++;
            
            printf("Server: New stream %lu started\n", stream_id);
        }

        stream_ctx->bytes_received += length;
        stream_ctx->last_byte_time = picoquic_current_time();
        ctx->total_bytes_received += length;

        if (length > 0) {
            printf("Server: Stream %lu received %zu bytes (total: %zu)\n", 
                stream_id, length, stream_ctx->bytes_received);
        }

        if (fin_or_event == picoquic_callback_stream_fin) {
            int64_t duration_us = stream_ctx->last_byte_time - stream_ctx->first_byte_time;
            printf("Server: Stream %lu finished - received %zu bytes in %.3f ms\n",
                stream_id, stream_ctx->bytes_received, duration_us / 1000.0);
        }
        break;

    case picoquic_callback_stream_reset:
        printf("Server: Stream %lu reset by peer\n", stream_id);
        break;

    case picoquic_callback_stop_sending:
        printf("Server: Stop sending requested for stream %lu\n", stream_id);
        picoquic_reset_stream(cnx, stream_id, 0);
        break;

    case picoquic_callback_stateless_reset:
    case picoquic_callback_close:
    case picoquic_callback_application_close:
        printf("Server: Connection closed\n");
        if (ctx->total_bytes_received > 0) {
            printf("Server: Total bytes received: %zu across %d streams\n", 
                ctx->total_bytes_received, ctx->nb_streams);
        }
        break;

    case picoquic_callback_version_negotiation:
        break;

    case picoquic_callback_stream_gap:
        printf("Server: Stream %lu gap detected\n", stream_id);
        break;

    case picoquic_callback_prepare_to_send:
        // Server just receives data, doesn't send
        break;

    case picoquic_callback_datagram:
    case picoquic_callback_datagram_acked:
    case picoquic_callback_datagram_lost:
    case picoquic_callback_datagram_spurious:
        break;

    case picoquic_callback_pacing_changed:
        break;

    case picoquic_callback_ready:
        printf("Server: Connection ready\n");
        printf("Server: Transport parameters negotiated successfully\n");
        break;

    default:
        break;
    }

    return ret;
}

int main(int argc, char** argv)
{
    int ret = 0;
    const char* server_cert = "../certs/cert.pem";
    const char* server_key = "../certs/key.pem";
    int server_port = 4443;
    picoquic_quic_t* quic = NULL;
    server_ctx_t server_ctx = { 0 };

#ifdef _WINDOWS
    WSADATA wsaData = { 0 };
    (void)WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    if (argc > 1) {
        server_port = atoi(argv[1]);
    }

    printf("Starting deadline-aware QUIC server on port %d\n", server_port);

    /* Setup signal handler */
    signal(SIGINT, signal_handler);

    /* Create QUIC context */
    quic = picoquic_create(8,
        server_cert,
        server_key,
        NULL,
        "deadline-test",
        server_callback,
        &server_ctx,
        NULL,
        NULL,
        NULL,
        picoquic_current_time(),
        NULL,
        NULL,
        NULL,
        0);

    if (quic == NULL) {
        printf("Could not create QUIC context\n");
        ret = -1;
        goto finish;
    }

    /* Enable deadline support by getting and modifying default transport parameters */
    const picoquic_tp_t* old_tp = picoquic_get_default_tp(quic);
    picoquic_tp_t new_tp = *old_tp;
    new_tp.enable_deadline_aware_streams = 1;
    picoquic_set_default_tp(quic, &new_tp);
    printf("Server: Deadline-aware streams enabled\n");

    /* Set default congestion algorithm */
    picoquic_set_default_congestion_algorithm(quic, picoquic_get_congestion_algorithm("bbr"));

    /* Run the server */
    ret = picoquic_packet_loop(quic, server_port, 0, 0, 0, 0, NULL, NULL);

    printf("\nServer shutting down...\n");

finish:
    if (quic != NULL) {
        picoquic_free(quic);
    }

#ifdef _WINDOWS
    WSACleanup();
#endif

    return ret;
}