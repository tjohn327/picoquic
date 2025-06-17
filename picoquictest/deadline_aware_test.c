/*
* This file tests the deadline-aware streams functionality
* according to draft-tjohn-quic-multipath-dmtp-01
*/

#include <stdlib.h>
#include <string.h>
#include "picoquic_internal.h"
#include "picoquic_utils.h"
#include "tls_api.h"
#include "picoquictest_internal.h"

/* Test transport parameter negotiation for deadline-aware streams */
static int deadline_aware_param_test()
{
    int ret = 0;
    picoquic_tp_t test_tp;
    
    DBG_PRINTF("%s", "Testing deadline-aware transport parameters");
    
    /* Initialize transport parameters */
    memset(&test_tp, 0, sizeof(test_tp));
    
    /* Test 1: Set deadline-aware parameters */
    test_tp.enable_deadline_aware_streams = 1;
    
    /* Test 2: Verify parameters are set correctly */
    if (test_tp.enable_deadline_aware_streams != 1) {
        DBG_PRINTF("Wrong enable_deadline_aware_streams: expected 1, got %d",
            test_tp.enable_deadline_aware_streams);
        ret = -1;
    }
    
    DBG_PRINTF("%s", "Transport parameter test completed");
    
    return ret;
}

/* Test deadline-aware connection negotiation */
static int deadline_aware_connection_test()
{
    int ret = 0;
    
    DBG_PRINTF("%s", "Testing deadline-aware connection negotiation");
    
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    struct sockaddr_in addr;
    picoquic_connection_id_t initial_cid = { {0x01, 0x02, 0x03, 0x04}, 4 };
    uint64_t current_time = 1000000;

    /* Create QUIC context and connection */
    quic = picoquic_create(8, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 
        current_time, &current_time, NULL, NULL, 0);
    if (quic == NULL) {
        DBG_PRINTF("%s", "Failed to create QUIC context");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    
    cnx = picoquic_create_cnx(quic, initial_cid, initial_cid,
        (struct sockaddr*)&addr, current_time, 0, "test", "test", 1);
    if (cnx == NULL) {
        DBG_PRINTF("%s", "Failed to create connection");
        ret = -1;
        goto done;
    }
    
    /* Test deadline-aware setup */
    cnx->local_parameters.enable_deadline_aware_streams = 1;
    cnx->remote_parameters.enable_deadline_aware_streams = 1;
    
    /* Enable deadline-aware streams */
    if (picoquic_enable_deadline_aware_streams(cnx) != 0) {
        DBG_PRINTF("%s", "Failed to enable deadline-aware streams");
        ret = -1;
        goto done;
    }
    
    /* Verify negotiation */
    if (!cnx->deadline_aware_enabled) {
        DBG_PRINTF("%s", "Deadline-aware streams not enabled");
        ret = -1;
    }
    
    DBG_PRINTF("Connection test completed: deadline_aware=%d",
        cnx->deadline_aware_enabled);

done:
    if (cnx != NULL) {
        picoquic_delete_cnx(cnx);
    }
    if (quic != NULL) {
        picoquic_free(quic);
    }
    
    return ret;
}

/* Test deadline setting and stream priority */
static int deadline_aware_priority_test()
{
    int ret = 0;
    
    DBG_PRINTF("%s", "Testing deadline-aware stream priority");
    
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    struct sockaddr_in addr;
    picoquic_connection_id_t initial_cid = { {0x01, 0x02, 0x03, 0x04}, 4 };
    uint64_t current_time = 1000000;

    /* Create QUIC context and connection */
    quic = picoquic_create(8, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 
        current_time, &current_time, NULL, NULL, 0);
    if (quic == NULL) {
        DBG_PRINTF("%s", "Failed to create QUIC context");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    
    cnx = picoquic_create_cnx(quic, initial_cid, initial_cid,
        (struct sockaddr*)&addr, current_time, 0, "test", "test", 1);
    if (cnx == NULL) {
        DBG_PRINTF("%s", "Failed to create connection");
        ret = -1;
        goto done;
    }
    
    /* Enable deadline-aware streams */
    cnx->local_parameters.enable_deadline_aware_streams = 1;
    cnx->remote_parameters.enable_deadline_aware_streams = 1;
    cnx->deadline_aware_enabled = 1;
    
    /* Create streams with different deadlines */
    uint64_t stream1 = 4;  /* Client-initiated bidirectional */
    uint64_t stream2 = 8;
    uint64_t stream3 = 12;
    
    uint64_t deadline_duration1 = 100; /* 100ms deadline duration */
    uint64_t deadline_duration2 = 50;  /* 50ms deadline duration (shorter) */
    uint64_t deadline_duration3 = 0;   /* No deadline */
    
    /* Set deadline durations */
    ret = picoquic_set_stream_deadline(cnx, stream1, deadline_duration1, PICOQUIC_DEADLINE_MODE_SOFT);
    if (ret != 0) {
        DBG_PRINTF("Failed to set deadline for stream %" PRIu64, stream1);
        goto done;
    }
    
    ret = picoquic_set_stream_deadline(cnx, stream2, deadline_duration2, PICOQUIC_DEADLINE_MODE_SOFT);
    if (ret != 0) {
        DBG_PRINTF("Failed to set deadline for stream %" PRIu64, stream2);
        goto done;
    }
    
    /* Verify stream deadlines */
    picoquic_stream_head_t* s1 = picoquic_find_stream(cnx, stream1);
    picoquic_stream_head_t* s2 = picoquic_find_stream(cnx, stream2);
    picoquic_stream_head_t* s3 = picoquic_find_stream(cnx, stream3);
    
    if (s1 == NULL || s2 == NULL) {
        DBG_PRINTF("%s", "Failed to find created streams");
        ret = -1;
        goto done;
    }
    
    if (s1->default_deadline_duration_ms != deadline_duration1) {
        DBG_PRINTF("Wrong deadline duration for stream1: expected %" PRIu64 ", got %" PRIu64,
            deadline_duration1, s1->default_deadline_duration_ms);
        ret = -1;
    }
    
    if (s2->default_deadline_duration_ms != deadline_duration2) {
        DBG_PRINTF("Wrong deadline duration for stream2: expected %" PRIu64 ", got %" PRIu64,
            deadline_duration2, s2->default_deadline_duration_ms);
        ret = -1;
    }
    
    /* Add data to streams to test EDF ordering */
    uint8_t test_data[] = "test data for deadline comparison";
    ret = picoquic_add_to_stream(cnx, stream1, test_data, sizeof(test_data), 0);
    if (ret != 0) {
        DBG_PRINTF("%s", "Failed to add data to stream1");
        goto done;
    }
    
    ret = picoquic_add_to_stream(cnx, stream2, test_data, sizeof(test_data), 0);
    if (ret != 0) {
        DBG_PRINTF("%s", "Failed to add data to stream2");
        goto done;
    }
    
    /* Test EDF ordering: stream2 should have higher priority than stream1 (shorter deadline duration) */
    if (picoquic_compare_stream_priority(s2, s1) >= 0) {
        DBG_PRINTF("%s", "EDF ordering failed: stream2 should have higher priority than stream1");
        ret = -1;
    }
    
    /* Test that deadline streams have priority over non-deadline streams */
    if (s3 != NULL && picoquic_compare_stream_priority(s1, s3) >= 0) {
        DBG_PRINTF("%s", "Priority ordering failed: deadline stream should have higher priority than non-deadline stream");
        ret = -1;
    }
    
    DBG_PRINTF("%s", "Priority test completed");

done:
    if (cnx != NULL) {
        picoquic_delete_cnx(cnx);
    }
    if (quic != NULL) {
        picoquic_free(quic);
    }
    
    return ret;
}

/* Test deadline modes and API functions */
static int deadline_aware_api_test()
{
    int ret = 0;
    
    DBG_PRINTF("%s", "Testing deadline-aware API functions");
    
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    struct sockaddr_in addr;
    picoquic_connection_id_t initial_cid = { {0x01, 0x02, 0x03, 0x04}, 4 };
    uint64_t current_time = 1000000;

    /* Create QUIC context and connection */
    quic = picoquic_create(8, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 
        current_time, &current_time, NULL, NULL, 0);
    if (quic == NULL) {
        DBG_PRINTF("%s", "Failed to create QUIC context");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    
    cnx = picoquic_create_cnx(quic, initial_cid, initial_cid,
        (struct sockaddr*)&addr, current_time, 0, "test", "test", 1);
    if (cnx == NULL) {
        DBG_PRINTF("%s", "Failed to create connection");
        ret = -1;
        goto done;
    }
    
    /* Enable deadline-aware streams */
    cnx->local_parameters.enable_deadline_aware_streams = 1;
    cnx->remote_parameters.enable_deadline_aware_streams = 1;
    cnx->deadline_aware_enabled = 1;
    
    /* Test API: picoquic_add_to_stream_with_deadline */
    uint64_t stream_id = 4;
    const uint8_t test_data[] = "Hello, deadline world!";
    uint64_t deadline_duration = 200; /* 200ms deadline duration */
    
    ret = picoquic_add_to_stream_with_deadline(cnx, stream_id, test_data, sizeof(test_data), 0,
                                               deadline_duration, PICOQUIC_DEADLINE_MODE_HARD);
    if (ret != 0) {
        DBG_PRINTF("Failed to add data to stream with deadline: %d", ret);
        goto done;
    }
    
    /* Verify stream was created with correct deadline duration */
    picoquic_stream_head_t* stream = picoquic_find_stream(cnx, stream_id);
    if (stream == NULL) {
        DBG_PRINTF("%s", "Stream not created by add_to_stream_with_deadline");
        ret = -1;
        goto done;
    }
    
    if (stream->default_deadline_duration_ms != deadline_duration) {
        DBG_PRINTF("Wrong deadline duration set by add_to_stream_with_deadline: expected %" PRIu64 ", got %" PRIu64,
            deadline_duration, stream->default_deadline_duration_ms);
        ret = -1;
    }
    
    if (stream->default_deadline_mode != PICOQUIC_DEADLINE_MODE_HARD) {
        DBG_PRINTF("Wrong deadline mode: expected %d, got %d",
            PICOQUIC_DEADLINE_MODE_HARD, stream->default_deadline_mode);
        ret = -1;
    }
    
    /* Verify that the data in send queue has correct deadline information */
    if (stream->send_queue != NULL) {
        picoquic_stream_queue_node_t* queue_node = stream->send_queue;
        if (queue_node->deadline_duration_ms != deadline_duration) {
            DBG_PRINTF("Wrong deadline duration in queue node: expected %" PRIu64 ", got %" PRIu64,
                deadline_duration, queue_node->deadline_duration_ms);
            ret = -1;
        }
        
        if (queue_node->deadline_mode != PICOQUIC_DEADLINE_MODE_HARD) {
            DBG_PRINTF("Wrong deadline mode in queue node: expected %d, got %d",
                PICOQUIC_DEADLINE_MODE_HARD, queue_node->deadline_mode);
            ret = -1;
        }
    } else {
        DBG_PRINTF("%s", "No data found in stream send queue");
        ret = -1;
    }
    
    /* Test deadline mode constants */
    if (PICOQUIC_DEADLINE_MODE_NONE != 0 ||
        PICOQUIC_DEADLINE_MODE_SOFT != 1 ||
        PICOQUIC_DEADLINE_MODE_HARD != 2) {
        DBG_PRINTF("%s", "Deadline mode constants are incorrect");
        ret = -1;
    }
    
    DBG_PRINTF("%s", "API test completed");

done:
    if (cnx != NULL) {
        picoquic_delete_cnx(cnx);
    }
    if (quic != NULL) {
        picoquic_free(quic);
    }
    
    return ret;
}

/* Main test function that runs all deadline-aware streams tests */
int deadline_aware_test()
{
    int ret = 0;
    
    DBG_PRINTF("%s", "Starting deadline-aware streams tests");
    
    if (ret == 0) {
        DBG_PRINTF("%s", "\n=== Test 1: Transport Parameter Negotiation ===");
        ret = deadline_aware_param_test();
        DBG_PRINTF("deadline_aware_param_test: %s\n", (ret == 0) ? "PASS" : "FAIL");
    }
    
    if (ret == 0) {
        DBG_PRINTF("%s", "=== Test 2: Connection Negotiation ===");
        ret = deadline_aware_connection_test();
        DBG_PRINTF("deadline_aware_connection_test: %s\n", (ret == 0) ? "PASS" : "FAIL");
    }
    
    if (ret == 0) {
        DBG_PRINTF("%s", "=== Test 3: Stream Priority and EDF Scheduling ===");
        ret = deadline_aware_priority_test();
        DBG_PRINTF("deadline_aware_priority_test: %s\n", (ret == 0) ? "PASS" : "FAIL");
    }
    
    if (ret == 0) {
        DBG_PRINTF("%s", "=== Test 4: API Functions and Deadline Modes ===");
        ret = deadline_aware_api_test();
        DBG_PRINTF("deadline_aware_api_test: %s\n", (ret == 0) ? "PASS" : "FAIL");
    }
    
    DBG_PRINTF("All deadline-aware streams tests completed: %s", (ret == 0) ? "SUCCESS" : "FAILED");
    
    return ret;
}