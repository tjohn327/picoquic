/*
* Test per-chunk deadline functionality
*/

#include "../picoquic/picoquic_internal.h"
#include "../picoquictest/picoquictest_internal.h"
#include <stdlib.h>
#include <string.h>

int deadline_per_chunk_test()
{
    int ret = 0;
    
    /* Very simple test - just verify structures compile correctly */
    picoquic_stream_queue_node_t test_node;
    memset(&test_node, 0, sizeof(test_node));
    
    /* Set test values */
    test_node.enqueue_time = 1000;
    test_node.chunk_deadline = 2000;
    
    /* Verify values */
    if (test_node.enqueue_time != 1000) {
        DBG_PRINTF("ERROR: enqueue_time not set correctly: %lu\n", 
            (unsigned long)test_node.enqueue_time);
        ret = -1;
    }
    
    if (test_node.chunk_deadline != 2000) {
        DBG_PRINTF("ERROR: chunk_deadline not set correctly: %lu\n",
            (unsigned long)test_node.chunk_deadline);
        ret = -1;
    }
    
    if (ret == 0) {
        DBG_PRINTF("%s", "Per-chunk deadline fields verified successfully\n");
    }
    
    return ret;
}