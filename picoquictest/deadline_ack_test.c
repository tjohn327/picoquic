/*
* Test to isolate ACK frame issue with deadline streams
*/

#include "../picoquic/picoquic_internal.h"
#include "../picoquic/tls_api.h"
#include "picoquictest_internal.h"
#include <stdlib.h>
#include <string.h>

int deadline_ack_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    uint64_t current_time = 0;
    picoquic_quic_t* quic_client = NULL;
    picoquic_quic_t* quic_server = NULL;
    picoquic_cnx_t* cnx_client = NULL;
    picoquic_cnx_t* cnx_server = NULL;
    struct sockaddr_in addr_client, addr_server;
    
    DBG_PRINTF("%s", "\n=== Deadline ACK Test ===\n");
    
    /* Create QUIC contexts */
    quic_client = picoquic_create(8, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        current_time, &simulated_time, NULL, NULL, 0);
    
    quic_server = picoquic_create(8, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        current_time, &simulated_time, NULL, NULL, 0);
    
    if (quic_client == NULL || quic_server == NULL) {
        DBG_PRINTF("%s", "Failed to create QUIC contexts\n");
        ret = -1;
    }
    
    /* Set server transport parameters */
    if (ret == 0) {
        picoquic_tp_t server_tp;
        memset(&server_tp, 0, sizeof(server_tp));
        picoquic_init_transport_parameters(&server_tp, 1);
        server_tp.enable_deadline_aware_streams = 1;
        picoquic_set_default_tp(quic_server, &server_tp);
        DBG_PRINTF("%s", "Server deadline streams enabled\n");
    }
    
    /* Create client connection */
    if (ret == 0) {
        memset(&addr_client, 0, sizeof(addr_client));
        memset(&addr_server, 0, sizeof(addr_server));
        addr_client.sin_family = AF_INET;
        addr_server.sin_family = AF_INET;
        addr_server.sin_port = 1234;
        
        cnx_client = picoquic_create_cnx(quic_client,
            picoquic_null_connection_id, picoquic_null_connection_id,
            (struct sockaddr*)&addr_server, current_time, 0,
            "test-sni", "test-alpn", 1);
        
        if (cnx_client == NULL) {
            DBG_PRINTF("%s", "Failed to create client connection\n");
            ret = -1;
        } else {
            cnx_client->local_parameters.enable_deadline_aware_streams = 1;
            DBG_PRINTF("%s", "Client connection created\n");
        }
    }
    
    /* Simulate initial exchange */
    if (ret == 0) {
        size_t send_length = 0;
        uint8_t send_buffer[PICOQUIC_MAX_PACKET_SIZE];
        struct sockaddr_storage addr_to, addr_from;
        int if_index = 0;
        
        /* Convert sockaddr_in to sockaddr_storage */
        memset(&addr_to, 0, sizeof(addr_to));
        memset(&addr_from, 0, sizeof(addr_from));
        memcpy(&addr_to, &addr_server, sizeof(addr_server));
        memcpy(&addr_from, &addr_client, sizeof(addr_client));
        
        /* Start client connection */
        ret = picoquic_start_client_cnx(cnx_client);
        DBG_PRINTF("Client connection started: %d\n", ret);
        
        /* Prepare initial packet */
        if (ret == 0) {
            ret = picoquic_prepare_packet(cnx_client, current_time,
                send_buffer, sizeof(send_buffer), &send_length,
                &addr_to, &addr_from, &if_index);
            DBG_PRINTF("Initial packet prepared: ret=%d, length=%zu\n", ret, send_length);
        }
        
        /* Simulate receiving at server */
        if (ret == 0 && send_length > 0) {
            ret = picoquic_incoming_packet(quic_server, send_buffer, send_length,
                (struct sockaddr*)&addr_client, (struct sockaddr*)&addr_server,
                0, 0, current_time);
            DBG_PRINTF("Server processed initial: ret=%d\n", ret);
            
            /* Get server connection */
            cnx_server = quic_server->cnx_list;
            if (cnx_server != NULL) {
                DBG_PRINTF("%s", "Server connection created\n");
            }
        }
    }
    
    /* Check if deadline contexts are created */
    if (ret == 0 && cnx_server != NULL) {
        if (cnx_client->deadline_context != NULL) {
            DBG_PRINTF("%s", "Client deadline context exists\n");
        }
        if (cnx_server->deadline_context != NULL) {
            DBG_PRINTF("%s", "Server deadline context exists\n");
        }
    }
    
    /* Try to set deadline and send data */
    if (ret == 0 && cnx_client != NULL) {
        /* First create a stream by sending data */
        uint8_t data[100];
        memset(data, 0x42, sizeof(data));
        
        ret = picoquic_add_to_stream(cnx_client, 0, data, sizeof(data), 0);
        DBG_PRINTF("Added data to stream: ret=%d\n", ret);
        
        /* Now set deadline */
        if (ret == 0) {
            ret = picoquic_set_stream_deadline(cnx_client, 0, 100, 1);
            DBG_PRINTF("Set deadline on stream: ret=%d\n", ret);
        }
        
        /* Prepare packet with stream data */
        if (ret == 0) {
            size_t length = 0;
            uint8_t buffer[PICOQUIC_MAX_PACKET_SIZE];
            struct sockaddr_storage addr_to, addr_from;
            int if_index = 0;
            
            /* Convert sockaddr_in to sockaddr_storage */
            memset(&addr_to, 0, sizeof(addr_to));
            memset(&addr_from, 0, sizeof(addr_from));
            memcpy(&addr_to, &addr_server, sizeof(addr_server));
            memcpy(&addr_from, &addr_client, sizeof(addr_client));
            
            ret = picoquic_prepare_packet(cnx_client, current_time,
                buffer, sizeof(buffer), &length,
                &addr_to, &addr_from, &if_index);
            DBG_PRINTF("Prepared packet with stream: ret=%d, length=%zu\n", ret, length);
            
            /* Check packet content */
            if (ret == 0 && length > 0) {
                DBG_PRINTF("%s", "First 20 bytes: ");
                for (int i = 0; i < 20 && i < length; i++) {
                    DBG_PRINTF("%02x ", buffer[i]);
                }
                DBG_PRINTF("%s", "\n");
            }
        }
    }
    
    /* Cleanup */
    if (cnx_client != NULL) {
        picoquic_delete_cnx(cnx_client);
    }
    if (cnx_server != NULL) {
        picoquic_delete_cnx(cnx_server);
    }
    if (quic_client != NULL) {
        picoquic_free(quic_client);
    }
    if (quic_server != NULL) {
        picoquic_free(quic_server);
    }
    
    DBG_PRINTF("\n=== ACK Test Result: %s ===\n", (ret == 0) ? "PASS" : "FAIL");
    
    return ret;
}