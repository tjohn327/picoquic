/*
* picoquicdemo with deadline-aware streams support
* 
* This extends the standard picoquicdemo to support configuring
* deadlines on streams for comprehensive real-world testing.
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../picoquic/picoquic.h"
#include "../picoquic/picoquic_internal.h"
#include "../picohttp/democlient.h"

/* Deadline configuration structure */
typedef struct st_deadline_config_t {
    uint64_t stream_id;
    uint64_t deadline_ms;
    int is_hard;
    struct st_deadline_config_t* next;
} deadline_config_t;

/* Extended client context with deadline support */
typedef struct st_deadline_client_ctx_t {
    picoquic_demo_callback_ctx_t base_ctx;
    deadline_config_t* deadline_configs;
    int enable_deadline_aware;
    /* Statistics tracking */
    uint64_t streams_started;
    uint64_t streams_completed;
    uint64_t bytes_dropped;
    uint64_t deadlines_missed;
} deadline_client_ctx_t;

/* Parse deadline configuration string */
/* Format: "stream_id:deadline_ms:is_hard,..." */
/* Example: "4:100:1,8:200:0" */
static int parse_deadline_config(const char* config_str, deadline_config_t** config_list)
{
    if (config_str == NULL || config_list == NULL) {
        return -1;
    }
    
    *config_list = NULL;
    deadline_config_t* last = NULL;
    
    char* config_copy = strdup(config_str);
    char* token = strtok(config_copy, ",");
    
    while (token != NULL) {
        uint64_t stream_id, deadline_ms;
        int is_hard;
        
        if (sscanf(token, "%lu:%lu:%d", &stream_id, &deadline_ms, &is_hard) == 3) {
            deadline_config_t* cfg = (deadline_config_t*)malloc(sizeof(deadline_config_t));
            if (cfg == NULL) {
                free(config_copy);
                return -1;
            }
            
            cfg->stream_id = stream_id;
            cfg->deadline_ms = deadline_ms;
            cfg->is_hard = is_hard;
            cfg->next = NULL;
            
            if (last == NULL) {
                *config_list = cfg;
            } else {
                last->next = cfg;
            }
            last = cfg;
            
            fprintf(stdout, "Deadline config: stream %lu = %lu ms (%s)\n",
                    stream_id, deadline_ms, is_hard ? "hard" : "soft");
        }
        
        token = strtok(NULL, ",");
    }
    
    free(config_copy);
    return 0;
}

/* Find deadline configuration for a stream */
static deadline_config_t* find_deadline_config(deadline_config_t* list, uint64_t stream_id)
{
    while (list != NULL) {
        if (list->stream_id == stream_id) {
            return list;
        }
        list = list->next;
    }
    return NULL;
}

/* Extended callback with deadline support */
static int deadline_demo_client_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{
    deadline_client_ctx_t* ctx = (deadline_client_ctx_t*)callback_ctx;
    
    /* Handle deadline-specific events */
    switch (fin_or_event) {
    case picoquic_callback_stream_gap:
        /* Data was dropped due to deadline */
        ctx->bytes_dropped += length;
        fprintf(stdout, "Stream %lu: Dropped %zu bytes due to deadline (gap at offset %lu)\n",
                stream_id, length, *(uint64_t*)bytes);
        /* Continue processing - don't return error */
        return 0;
        
    case picoquic_callback_stream_fin:
        ctx->streams_completed++;
        break;
        
    default:
        break;
    }
    
    /* Call the base demo client callback */
    return picoquic_demo_client_callback(cnx, stream_id, bytes, length, 
                                         fin_or_event, &ctx->base_ctx, v_stream_ctx);
}

/* Hook into stream creation to set deadlines */
static int start_stream_with_deadline(picoquic_cnx_t* cnx, 
    deadline_client_ctx_t* ctx, uint64_t stream_id)
{
    int ret = 0;
    
    /* Find deadline configuration for this stream */
    deadline_config_t* cfg = find_deadline_config(ctx->deadline_configs, stream_id);
    if (cfg != NULL && ctx->enable_deadline_aware) {
        /* Set the deadline on the stream */
        ret = picoquic_set_stream_deadline(cnx, stream_id, cfg->deadline_ms, cfg->is_hard);
        if (ret == 0) {
            fprintf(stdout, "Set deadline on stream %lu: %lu ms (%s)\n",
                    stream_id, cfg->deadline_ms, cfg->is_hard ? "hard" : "soft");
        } else {
            fprintf(stderr, "Failed to set deadline on stream %lu: %d\n", stream_id, ret);
        }
    }
    
    ctx->streams_started++;
    return ret;
}

/* Modified stream start function that hooks deadline setting */
int picoquic_demo_client_start_streams_deadline(picoquic_cnx_t* cnx,
    picoquic_demo_callback_ctx_t* callback_ctx, uint64_t fin_stream_id)
{
    int ret = 0;
    
    /* Get our extended context */
    deadline_client_ctx_t* deadline_ctx = (deadline_client_ctx_t*)((char*)callback_ctx - offsetof(deadline_client_ctx_t, base_ctx));
    
    /* Start streams as normal */
    ret = picoquic_demo_client_start_streams(cnx, callback_ctx, fin_stream_id);
    
    /* Set deadlines on any newly started streams */
    if (ret == 0 && deadline_ctx->enable_deadline_aware) {
        for (size_t i = 0; i < callback_ctx->nb_demo_streams; i++) {
            if (callback_ctx->demo_stream[i].stream_id != PICOQUIC_DEMO_STREAM_ID_INITIAL &&
                !callback_ctx->demo_stream[i].is_open) {
                /* This stream will be opened, set deadline if configured */
                start_stream_with_deadline(cnx, deadline_ctx, callback_ctx->demo_stream[i].stream_id);
            }
        }
    }
    
    return ret;
}

/* Print deadline statistics */
static void print_deadline_stats(deadline_client_ctx_t* ctx)
{
    fprintf(stdout, "\n=== Deadline Statistics ===\n");
    fprintf(stdout, "Streams started:    %lu\n", ctx->streams_started);
    fprintf(stdout, "Streams completed:  %lu\n", ctx->streams_completed);
    fprintf(stdout, "Bytes dropped:      %lu\n", ctx->bytes_dropped);
    fprintf(stdout, "Deadlines missed:   %lu\n", ctx->deadlines_missed);
    
    if (ctx->bytes_dropped > 0) {
        fprintf(stdout, "\nPartial reliability was activated - hard deadlines caused data drops.\n");
    }
}

/* Extended usage function */
static void usage_deadline()
{
    fprintf(stderr, "\nDeadline-aware streams options:\n");
    fprintf(stderr, "  -D                    Enable deadline-aware streams transport parameter\n");
    fprintf(stderr, "  -d config             Set deadlines on streams. Format: stream_id:deadline_ms:is_hard,...\n");
    fprintf(stderr, "                        Example: -d \"4:100:1,8:200:0\"\n");
    fprintf(stderr, "                        Sets 100ms hard deadline on stream 4, 200ms soft on stream 8\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  # Basic deadline test\n");
    fprintf(stderr, "  ./picoquicdemo_deadline -D -d \"0:100:0\" localhost 4443 \"0:index.html\"\n");
    fprintf(stderr, "\n  # Multiple streams with different deadlines\n");
    fprintf(stderr, "  ./picoquicdemo_deadline -D -d \"0:50:1,4:200:0\" localhost 4443 \"0:file1.bin;4:file2.bin\"\n");
    fprintf(stderr, "\n  # Multipath with deadlines\n");
    fprintf(stderr, "  ./picoquicdemo_deadline -D -d \"0:100:1\" -A \"192.168.1.100/2,192.168.2.100/3\" server 4443\n");
}

/* Modified main function with deadline support */
int main(int argc, char** argv)
{
    int ret = 0;
    char* deadline_config_str = NULL;
    int enable_deadline_aware = 0;
    deadline_config_t* deadline_configs = NULL;
    
    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-D") == 0) {
            enable_deadline_aware = 1;
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            deadline_config_str = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage_deadline();
            return 0;
        }
    }
    
    /* Parse deadline configuration if provided */
    if (deadline_config_str != NULL) {
        ret = parse_deadline_config(deadline_config_str, &deadline_configs);
        if (ret != 0) {
            fprintf(stderr, "Failed to parse deadline configuration\n");
            return -1;
        }
    }
    
    /* TODO: Integrate with main picoquicdemo flow */
    /* This would require modifying the actual picoquicdemo.c */
    /* For now, this shows the structure needed */
    
    fprintf(stdout, "Deadline-aware picoquicdemo extension loaded\n");
    if (enable_deadline_aware) {
        fprintf(stdout, "Deadline-aware streams ENABLED\n");
    }
    
    /* Clean up */
    while (deadline_configs != NULL) {
        deadline_config_t* next = deadline_configs->next;
        free(deadline_configs);
        deadline_configs = next;
    }
    
    return ret;
}