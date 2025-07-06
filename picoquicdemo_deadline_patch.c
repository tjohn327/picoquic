/*
* Deadline support extension for picoquicdemo
* 
* This file contains the modifications needed to add deadline support
* to the existing picoquicdemo application.
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

/* Global deadline configuration */
static deadline_config_t* g_deadline_configs = NULL;
static int g_enable_deadline_aware = 0;

/* Statistics tracking */
static struct {
    uint64_t streams_with_deadlines;
    uint64_t bytes_dropped_total;
    uint64_t gaps_received;
    uint64_t deadlines_missed;
} g_deadline_stats = {0};

/* Parse deadline configuration string */
/* Format: "stream_id:deadline_ms:is_hard,..." */
/* Example: "4:100:1,8:200:0" */
int parse_deadline_config(const char* config_str)
{
    if (config_str == NULL) {
        return -1;
    }
    
    /* Free existing config */
    while (g_deadline_configs != NULL) {
        deadline_config_t* next = g_deadline_configs->next;
        free(g_deadline_configs);
        g_deadline_configs = next;
    }
    
    deadline_config_t* last = NULL;
    char* config_copy = strdup(config_str);
    char* saveptr = NULL;
    char* token = strtok_r(config_copy, ",", &saveptr);
    
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
                g_deadline_configs = cfg;
            } else {
                last->next = cfg;
            }
            last = cfg;
            
            fprintf(stdout, "Deadline config: stream %lu = %lu ms (%s)\n",
                    stream_id, deadline_ms, is_hard ? "hard" : "soft");
        }
        
        token = strtok_r(NULL, ",", &saveptr);
    }
    
    free(config_copy);
    return 0;
}

/* Find deadline configuration for a stream */
deadline_config_t* find_deadline_config(uint64_t stream_id)
{
    deadline_config_t* cfg = g_deadline_configs;
    while (cfg != NULL) {
        if (cfg->stream_id == stream_id) {
            return cfg;
        }
        cfg = cfg->next;
    }
    return NULL;
}

/* Hook to be called when opening a stream */
void on_stream_open(picoquic_cnx_t* cnx, uint64_t stream_id)
{
    if (!g_enable_deadline_aware) {
        return;
    }
    
    deadline_config_t* cfg = find_deadline_config(stream_id);
    if (cfg != NULL) {
        int ret = picoquic_set_stream_deadline(cnx, stream_id, cfg->deadline_ms, cfg->is_hard);
        if (ret == 0) {
            fprintf(stdout, "[DEADLINE] Set deadline on stream %lu: %lu ms (%s)\n",
                    stream_id, cfg->deadline_ms, cfg->is_hard ? "hard" : "soft");
            g_deadline_stats.streams_with_deadlines++;
        } else {
            fprintf(stderr, "[DEADLINE] Failed to set deadline on stream %lu: %d\n", 
                    stream_id, ret);
        }
    }
}

/* Hook to be called on stream gaps */
void on_stream_gap(uint64_t stream_id, uint64_t gap_offset, size_t gap_length)
{
    g_deadline_stats.gaps_received++;
    g_deadline_stats.bytes_dropped_total += gap_length;
    
    fprintf(stdout, "[DEADLINE] Stream %lu: Gap detected at offset %lu, length %zu bytes\n",
            stream_id, gap_offset, gap_length);
}

/* Print deadline statistics */
void print_deadline_statistics()
{
    if (!g_enable_deadline_aware) {
        return;
    }
    
    fprintf(stdout, "\n=== Deadline Statistics ===\n");
    fprintf(stdout, "Streams with deadlines:  %lu\n", g_deadline_stats.streams_with_deadlines);
    fprintf(stdout, "Total gaps received:     %lu\n", g_deadline_stats.gaps_received);
    fprintf(stdout, "Total bytes dropped:     %lu\n", g_deadline_stats.bytes_dropped_total);
    
    if (g_deadline_stats.bytes_dropped_total > 0) {
        fprintf(stdout, "\nPartial reliability was activated - hard deadlines caused data drops.\n");
    }
}

/* Modified callback wrapper that handles deadline events */
int deadline_aware_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{
    /* Handle deadline-specific events */
    if (fin_or_event == picoquic_callback_stream_gap) {
        /* Gap notification - data was dropped due to deadline */
        uint64_t gap_offset = 0;
        if (length >= sizeof(uint64_t)) {
            gap_offset = *((uint64_t*)bytes);
        }
        on_stream_gap(stream_id, gap_offset, length);
        
        /* Don't pass gap events to the original callback */
        return 0;
    }
    
    /* For all other events, call the original callback */
    return picoquic_demo_client_callback(cnx, stream_id, bytes, length, 
                                        fin_or_event, callback_ctx, v_stream_ctx);
}

/* Enable deadline-aware streams on connection */
void enable_deadline_on_connection(picoquic_cnx_t* cnx)
{
    if (g_enable_deadline_aware && cnx != NULL) {
        cnx->local_parameters.enable_deadline_aware_streams = 1;
        fprintf(stdout, "[DEADLINE] Enabled deadline-aware streams on connection\n");
    }
}

/* Additional command line options for deadline support */
void print_deadline_usage()
{
    fprintf(stderr, "\nDeadline-aware streams options:\n");
    fprintf(stderr, "  -D                    Enable deadline-aware streams transport parameter\n");
    fprintf(stderr, "  -d config             Set deadlines on streams. Format: stream_id:deadline_ms:is_hard,...\n");
    fprintf(stderr, "                        Example: -d \"4:100:1,8:200:0\"\n");
    fprintf(stderr, "                        Sets 100ms hard deadline on stream 4, 200ms soft on stream 8\n");
}

/* Parse deadline-specific command line options */
int parse_deadline_options(int opt, char* optarg)
{
    switch (opt) {
    case 'D':
        g_enable_deadline_aware = 1;
        fprintf(stdout, "[DEADLINE] Deadline-aware streams ENABLED\n");
        return 1;
        
    case 'd':
        if (parse_deadline_config(optarg) != 0) {
            fprintf(stderr, "[DEADLINE] Failed to parse deadline configuration\n");
            return -1;
        }
        return 1;
        
    default:
        return 0;
    }
}

/* Cleanup deadline configurations */
void cleanup_deadline_configs()
{
    while (g_deadline_configs != NULL) {
        deadline_config_t* next = g_deadline_configs->next;
        free(g_deadline_configs);
        g_deadline_configs = next;
    }
}

/* 
 * Integration points with picoquicdemo.c:
 * 
 * 1. In main(), add to option string: "Dd:"
 * 2. In option parsing switch, call parse_deadline_options()
 * 3. After creating connection, call enable_deadline_on_connection()
 * 4. Replace callback with deadline_aware_callback
 * 5. In stream opening code, call on_stream_open()
 * 6. At program exit, call print_deadline_statistics() and cleanup_deadline_configs()
 */