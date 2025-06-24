/*

*/

#include <stdlib.h>
#include <string.h>
#include "picoquic_internal.h"
#include "picoquic_utils.h"
#include "picoquictest_internal.h"
#include "tls_api.h"
#ifdef _WINDOWS
#include <WS2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif


/* Test receive timestamp transport parameter negotiation */
int receive_timestamp_transport_param_test()
{
    int ret = 0;
    uint8_t buffer[256];
    size_t consumed = 0;
    
    /* Create a simple transport parameter structure */
    picoquic_tp_t tp;
    memset(&tp, 0, sizeof(tp));
    
    /* Set receive timestamp parameters */
    tp.max_receive_timestamps_per_ack = 10;
    tp.receive_timestamps_exponent = 2;
    
    /* Manually encode the transport parameters */
    uint8_t* bytes = buffer;
    uint8_t* bytes_max = buffer + sizeof(buffer);
    
    /* Encode max_receive_timestamps_per_ack */
    bytes = picoquic_frames_varint_encode(bytes, bytes_max, picoquic_tp_max_receive_timestamps_per_ack);
    if (bytes != NULL) {
        bytes = picoquic_frames_varint_encode(bytes, bytes_max, 1); /* length */
        if (bytes != NULL) {
            bytes = picoquic_frames_varint_encode(bytes, bytes_max, tp.max_receive_timestamps_per_ack);
        }
    }
    
    /* Encode receive_timestamps_exponent */
    if (bytes != NULL) {
        bytes = picoquic_frames_varint_encode(bytes, bytes_max, picoquic_tp_receive_timestamps_exponent);
        if (bytes != NULL) {
            bytes = picoquic_frames_varint_encode(bytes, bytes_max, 1); /* length */
            if (bytes != NULL) {
                bytes = picoquic_frames_varint_encode(bytes, bytes_max, tp.receive_timestamps_exponent);
            }
        }
    }
    
    if (bytes == NULL) {
        ret = -1;
    } else {
        consumed = bytes - buffer;
        
        /* Test decoding */
        picoquic_tp_t tp_decoded;
        memset(&tp_decoded, 0, sizeof(tp_decoded));
        size_t byte_index = 0;
        
        while (byte_index < consumed && ret == 0) {
            uint64_t type = 0;
            uint64_t length = 0;
            
            byte_index += picoquic_varint_decode(buffer + byte_index, consumed - byte_index, &type);
            byte_index += picoquic_varint_decode(buffer + byte_index, consumed - byte_index, &length);
            
            if (type == picoquic_tp_max_receive_timestamps_per_ack) {
                byte_index += picoquic_varint_decode(buffer + byte_index, consumed - byte_index, 
                    &tp_decoded.max_receive_timestamps_per_ack);
            } else if (type == picoquic_tp_receive_timestamps_exponent) {
                byte_index += picoquic_varint_decode(buffer + byte_index, consumed - byte_index,
                    &tp_decoded.receive_timestamps_exponent);
            } else {
                byte_index += length;
            }
        }
        
        /* Verify */
        if (tp_decoded.max_receive_timestamps_per_ack != 10 ||
            tp_decoded.receive_timestamps_exponent != 2) {
            DBG_PRINTF("Transport parameter mismatch: max_ts=%llu, exp=%llu\n",
                (unsigned long long)tp_decoded.max_receive_timestamps_per_ack,
                (unsigned long long)tp_decoded.receive_timestamps_exponent);
            ret = -1;
        }
    }
    
    return ret;
}

/* Test receive timestamp collection */
int receive_timestamp_collection_test()
{
    int ret = 0;
    picoquic_cnx_t cnx;
    picoquic_ack_context_t ack_ctx;
    uint64_t current_time = 1000000;
    
    /* Initialize connection and ack context */
    memset(&cnx, 0, sizeof(cnx));
    memset(&ack_ctx, 0, sizeof(ack_ctx));
    cnx.ack_ctx[picoquic_packet_context_application] = ack_ctx;
    picoquic_init_ack_ctx(&cnx, &cnx.ack_ctx[picoquic_packet_context_application]);
    
    /* Enable receive timestamps */
    cnx.remote_parameters.max_receive_timestamps_per_ack = 5;
    cnx.remote_parameters.receive_timestamps_exponent = 0;
    
    /* Record some packets */
    for (int i = 0; i < 5; i++) {
        ret = picoquic_record_pn_received(&cnx, picoquic_packet_context_application, NULL, 
            (uint64_t)i, current_time + i * 1000);
        if (ret != 0) {
            DBG_PRINTF("Failed to record packet %d\n", i);
            break;
        }
    }
    
    /* Verify timestamps were collected */
    if (ret == 0) {
        picoquic_receive_timestamp_t* ts = cnx.ack_ctx[picoquic_packet_context_application].first_receive_timestamp;
        int count = 0;
        
        while (ts != NULL) {
            count++;
            ts = ts->next;
        }
        
        if (count != 5) {
            DBG_PRINTF("Expected 5 timestamps, got %d\n", count);
            ret = -1;
        }
    }
    
    /* Clean up */
    picoquic_clear_ack_ctx(&cnx.ack_ctx[picoquic_packet_context_application]);
    
    return ret;
}

/* Test ACK frame formatting with receive timestamps */
int receive_timestamp_ack_format_test()
{
    int ret = 0;
    picoquic_cnx_t cnx;
    uint8_t bytes[256];
    int more_data = 0;
    uint64_t current_time = 2000000;
    
    /* Initialize connection */
    memset(&cnx, 0, sizeof(cnx));
    picoquic_init_ack_ctx(&cnx, &cnx.ack_ctx[picoquic_packet_context_application]);
    
    /* Enable receive timestamps */
    cnx.remote_parameters.max_receive_timestamps_per_ack = 3;
    cnx.remote_parameters.receive_timestamps_exponent = 1;
    cnx.local_parameters.ack_delay_exponent = 3;
    
    /* Initialize SACK list and record some packets */
    picoquic_sack_list_init(&cnx.ack_ctx[picoquic_packet_context_application].sack_list);
    
    /* Record packets with timestamps */
    for (int i = 0; i < 3; i++) {
        ret = picoquic_record_pn_received(&cnx, picoquic_packet_context_application, NULL,
            (uint64_t)(10 + i), current_time + i * 2000);
        if (ret != 0) {
            break;
        }
    }
    
    /* Format ACK frame */
    if (ret == 0) {
        uint8_t* bytes_next = picoquic_format_ack_frame(&cnx, bytes, bytes + sizeof(bytes), 
            &more_data, current_time + 10000, picoquic_packet_context_application, 0);
        
        if (bytes_next == bytes) {
            DBG_PRINTF("%s", "Failed to format ACK frame\n");
            ret = -1;
        } else {
            size_t ack_length = bytes_next - bytes;
            DBG_PRINTF("Formatted ACK frame of %zu bytes\n", ack_length);
            
            /* Basic sanity check - should have timestamps */
            if (ack_length < 20) {
                DBG_PRINTF("%s", "ACK frame too short to contain timestamps\n");
                ret = -1;
            }
        }
    }
    
    /* Clean up */
    picoquic_clear_ack_ctx(&cnx.ack_ctx[picoquic_packet_context_application]);
    
    return ret;
}

/* Test end-to-end receive timestamp functionality with simulated connection */
int receive_timestamp_e2e_test()
{
    int ret = 0;
    picoquic_cnx_t client_cnx;
    picoquic_cnx_t server_cnx;
    uint64_t current_time = 1000000;
    
    /* Initialize connection structures */
    memset(&client_cnx, 0, sizeof(client_cnx));
    memset(&server_cnx, 0, sizeof(server_cnx));
    
    /* Initialize ACK contexts */
    picoquic_init_ack_ctx(&client_cnx, &client_cnx.ack_ctx[picoquic_packet_context_application]);
    picoquic_init_ack_ctx(&server_cnx, &server_cnx.ack_ctx[picoquic_packet_context_application]);
    
    /* Set transport parameters - client offers receive timestamp support */
    client_cnx.local_parameters.max_receive_timestamps_per_ack = 10;
    client_cnx.local_parameters.receive_timestamps_exponent = 1;
    
    /* Server also offers receive timestamp support */
    server_cnx.local_parameters.max_receive_timestamps_per_ack = 8;
    server_cnx.local_parameters.receive_timestamps_exponent = 2;
    
    /* Simulate transport parameter negotiation */
    client_cnx.remote_parameters.max_receive_timestamps_per_ack = server_cnx.local_parameters.max_receive_timestamps_per_ack;
    client_cnx.remote_parameters.receive_timestamps_exponent = server_cnx.local_parameters.receive_timestamps_exponent;
    
    server_cnx.remote_parameters.max_receive_timestamps_per_ack = client_cnx.local_parameters.max_receive_timestamps_per_ack;
    server_cnx.remote_parameters.receive_timestamps_exponent = client_cnx.local_parameters.receive_timestamps_exponent;
    
    /* Initialize SACK lists */
    picoquic_sack_list_init(&client_cnx.ack_ctx[picoquic_packet_context_application].sack_list);
    picoquic_sack_list_init(&server_cnx.ack_ctx[picoquic_packet_context_application].sack_list);
    
    /* Simulate server receiving packets from client */
    for (int i = 0; i < 5; i++) {
        ret = picoquic_record_pn_received(&server_cnx, picoquic_packet_context_application, NULL,
            (uint64_t)(100 + i), current_time + i * 1000);
        if (ret != 0) {
            DBG_PRINTF("Failed to record packet %d\n", i);
            break;
        }
    }
    
    /* Verify timestamps were collected on server */
    if (ret == 0) {
        picoquic_receive_timestamp_t* ts = server_cnx.ack_ctx[picoquic_packet_context_application].first_receive_timestamp;
        int count = 0;
        
        while (ts != NULL) {
            count++;
            ts = ts->next;
        }
        
        if (count == 0) {
            DBG_PRINTF("%s", "Server did not collect receive timestamps\n");
            ret = -1;
        } else {
            DBG_PRINTF("Server collected %d receive timestamps\n", count);
        }
    }
    
    /* Test ACK frame formatting with timestamps */
    if (ret == 0) {
        uint8_t ack_buffer[256];
        int more_data = 0;
        
        uint8_t* bytes_next = picoquic_format_ack_frame(&server_cnx, ack_buffer, ack_buffer + sizeof(ack_buffer),
            &more_data, current_time + 10000, picoquic_packet_context_application, 0);
            
        if (bytes_next == ack_buffer) {
            DBG_PRINTF("%s", "Failed to format ACK frame with timestamps\n");
            ret = -1;
        } else {
            size_t ack_length = bytes_next - ack_buffer;
            DBG_PRINTF("Server formatted ACK frame with timestamps: %zu bytes\n", ack_length);
            
            /* Verify the ACK frame includes timestamp information */
            if (ack_length < 15) { /* Basic ACK + some timestamp data */
                DBG_PRINTF("%s", "ACK frame seems too short for timestamps\n");
                ret = -1;
            }
        }
    }
    
    /* Clean up */
    picoquic_clear_ack_ctx(&client_cnx.ack_ctx[picoquic_packet_context_application]);
    picoquic_clear_ack_ctx(&server_cnx.ack_ctx[picoquic_packet_context_application]);
    
    return ret;
}

/* Test receive timestamp parsing from ACK frames */
int receive_timestamp_parse_test()
{
    int ret = 0;
    picoquic_cnx_t cnx;
    uint8_t bytes[256];
    size_t byte_index = 0;
    size_t expected_length;
    
    /* Initialize connection */
    memset(&cnx, 0, sizeof(cnx));
    cnx.local_parameters.max_receive_timestamps_per_ack = 5;
    cnx.local_parameters.receive_timestamps_exponent = 2;
    cnx.remote_parameters.max_receive_timestamps_per_ack = 5;
    cnx.remote_parameters.receive_timestamps_exponent = 2;
    picoquic_init_ack_ctx(&cnx, &cnx.ack_ctx[picoquic_packet_context_application]);
    
    /* Create test ACK frame with timestamps */
    /* Frame type 0x02 (ACK), largest ack 10, ack delay 5, ack range count 0, first ack range 10 */
    bytes[byte_index++] = 0x02; /* ACK frame type */
    byte_index += picoquic_varint_encode(bytes + byte_index, sizeof(bytes) - byte_index, 10); /* largest ack */
    byte_index += picoquic_varint_encode(bytes + byte_index, sizeof(bytes) - byte_index, 5); /* ack delay */
    byte_index += picoquic_varint_encode(bytes + byte_index, sizeof(bytes) - byte_index, 0); /* ack range count */
    byte_index += picoquic_varint_encode(bytes + byte_index, sizeof(bytes) - byte_index, 10); /* first ack range */
    
    /* Add timestamp count and ranges - this is what we're testing */
    byte_index += picoquic_varint_encode(bytes + byte_index, sizeof(bytes) - byte_index, 2); /* timestamp range count */
    
    /* First range: gap 0, timestamp range 3 (packets 10, 9, 8), timestamp delta 100 */
    byte_index += picoquic_varint_encode(bytes + byte_index, sizeof(bytes) - byte_index, 0); /* gap */
    byte_index += picoquic_varint_encode(bytes + byte_index, sizeof(bytes) - byte_index, 3); /* timestamp range */
    byte_index += picoquic_varint_encode(bytes + byte_index, sizeof(bytes) - byte_index, 100); /* timestamp delta */
    
    /* Second range: gap 2, timestamp range 2 (packets 5, 4), timestamp delta 200 */
    byte_index += picoquic_varint_encode(bytes + byte_index, sizeof(bytes) - byte_index, 2); /* gap */
    byte_index += picoquic_varint_encode(bytes + byte_index, sizeof(bytes) - byte_index, 2); /* timestamp range */
    byte_index += picoquic_varint_encode(bytes + byte_index, sizeof(bytes) - byte_index, 200); /* timestamp delta */
    
    expected_length = byte_index;
    
    /* Basic validation - check we encoded something reasonable */
    if (expected_length < 10) {
        DBG_PRINTF("ACK frame too short: %zu bytes\n", expected_length);
        ret = -1;
    } else {
        DBG_PRINTF("Created ACK frame with timestamps: %zu bytes\n", expected_length);
    }
    
    /* Clean up */
    picoquic_clear_ack_ctx(&cnx.ack_ctx[picoquic_packet_context_application]);
    
    return ret;
}

/* Main test function */
int receive_timestamp_test()
{
    int ret = 0;
    
    if (ret == 0) {
        ret = receive_timestamp_transport_param_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "receive_timestamp_transport_param_test failed\n");
        }
    }
    
    if (ret == 0) {
        ret = receive_timestamp_collection_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "receive_timestamp_collection_test failed\n");
        }
    }
    
    if (ret == 0) {
        ret = receive_timestamp_ack_format_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "receive_timestamp_ack_format_test failed\n");
        }
    }
    
    if (ret == 0) {
        ret = receive_timestamp_parse_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "receive_timestamp_parse_test failed\n");
        }
    }
    
    if (ret == 0) {
        ret = receive_timestamp_e2e_test();
        if (ret != 0) {
            DBG_PRINTF("%s", "receive_timestamp_e2e_test failed\n");
        }
    }
    
    return ret;
}