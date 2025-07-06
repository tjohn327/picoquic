/*

*/

#include "picoquic_internal.h"
#include "picoquic_utils.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/*
 * Deadline-aware streams implementation per draft-tjohn-quic-multipath-dmtp-01
 */

/* Initialize deadline context for a stream */
void picoquic_deadline_stream_init(picoquic_stream_head_t* stream)
{
    if (stream != NULL && stream->deadline_ctx == NULL) {
        stream->deadline_ctx = (picoquic_stream_deadline_t*)malloc(sizeof(picoquic_stream_deadline_t));
        if (stream->deadline_ctx != NULL) {
            memset(stream->deadline_ctx, 0, sizeof(picoquic_stream_deadline_t));
            picoquic_sack_list_init(&stream->deadline_ctx->dropped_ranges);
            picoquic_sack_list_init(&stream->deadline_ctx->receiver_dropped_ranges);
        }
    }
}

/* Clean up deadline context for a stream */
void picoquic_deadline_stream_free(picoquic_stream_head_t* stream)
{
    if (stream != NULL && stream->deadline_ctx != NULL) {
        picoquic_sack_list_free(&stream->deadline_ctx->dropped_ranges);
        picoquic_sack_list_free(&stream->deadline_ctx->receiver_dropped_ranges);
        free(stream->deadline_ctx);
        stream->deadline_ctx = NULL;
    }
}

/* Set deadline for a stream */
int picoquic_set_stream_deadline(picoquic_cnx_t* cnx, uint64_t stream_id, 
    uint64_t deadline_ms, int is_hard)
{
    int ret = 0;
    picoquic_stream_head_t* stream;
    
    /* Stream 0-3 are reserved for special use - don't allow deadline on them
     * Stream 0: Client-initiated bidirectional (often used for control)
     * Stream 1: Server-initiated bidirectional 
     * Stream 2: Client-initiated unidirectional
     * Stream 3: Server-initiated unidirectional
     * These streams may have special handling that conflicts with deadline logic.
     */
    if (stream_id < 4) {
        DBG_PRINTF("Cannot set deadline on reserved stream %lu (streams 0-3 are reserved)\n", 
            (unsigned long)stream_id);
        return PICOQUIC_ERROR_INVALID_STREAM_ID;
    }
    
    stream = picoquic_find_stream(cnx, stream_id);
    
    if (stream == NULL) {
        /* Create stream if it doesn't exist */
        stream = picoquic_create_stream(cnx, stream_id);
        if (stream == NULL) {
            ret = -1;
        }
    }
    
    if (ret == 0 && stream != NULL) {
        /* Initialize deadline context if needed */
        if (stream->deadline_ctx == NULL) {
            picoquic_deadline_stream_init(stream);
            if (stream->deadline_ctx == NULL) {
                ret = -1;
            }
        }
        
        if (ret == 0) {
            uint64_t current_time = picoquic_get_quic_time(cnx->quic);
            stream->deadline_ctx->deadline_ms = deadline_ms;
            stream->deadline_ctx->absolute_deadline = current_time + (deadline_ms * 1000); /* Convert ms to us */
            stream->deadline_ctx->deadline_type = is_hard ? 1 : 0;
            stream->deadline_ctx->deadline_enabled = 1;
            
            /* Queue DEADLINE_CONTROL frame for transmission */
            ret = picoquic_queue_deadline_control_frame(cnx, stream_id, deadline_ms);
        }
    }
    
    return ret;
}

/* Configure fairness parameters for deadline-aware scheduling */
void picoquic_set_deadline_fairness_params(picoquic_cnx_t* cnx,
    double min_non_deadline_share, uint64_t max_starvation_time_us)
{
    if (cnx != NULL && cnx->deadline_context != NULL) {
        /* Validate parameters */
        if (min_non_deadline_share >= 0.0 && min_non_deadline_share <= 1.0) {
            cnx->deadline_context->min_non_deadline_share = min_non_deadline_share;
        }
        if (max_starvation_time_us > 0) {
            cnx->deadline_context->max_starvation_time = max_starvation_time_us;
        }
    }
}

/* Queue a DEADLINE_CONTROL frame for transmission */
int picoquic_queue_deadline_control_frame(picoquic_cnx_t* cnx, uint64_t stream_id, 
    uint64_t deadline_ms)
{
    int ret = 0;
    size_t frame_size = 1 + /* frame type */
                       picoquic_encode_varint_length(stream_id) +
                       picoquic_encode_varint_length(deadline_ms);
    
    uint8_t* frame_buffer = (uint8_t*)malloc(frame_size);
    if (frame_buffer == NULL) {
        ret = -1;
    } else {
        uint8_t* bytes = frame_buffer;
        uint8_t* bytes_max = frame_buffer + frame_size;
        
        /* Encode the frame */
        *bytes++ = (uint8_t)picoquic_frame_type_deadline_control;
        bytes = picoquic_frames_varint_encode(bytes, bytes_max, stream_id);
        bytes = picoquic_frames_varint_encode(bytes, bytes_max, deadline_ms);
        
        /* Queue as misc frame for reliable delivery */
        ret = picoquic_queue_misc_frame(cnx, frame_buffer, frame_size, 0, picoquic_packet_context_application);
        
        if (ret != 0) {
            free(frame_buffer);
        }
    }
    
    return ret;
}

/* Parse DEADLINE_CONTROL frame */
const uint8_t* picoquic_parse_deadline_control_frame(picoquic_cnx_t* cnx,
    const uint8_t* bytes, const uint8_t* bytes_max, uint64_t current_time, int epoch)
{
    uint64_t stream_id = 0;
    uint64_t deadline_ms = 0;
    
    /* Skip frame type */
    bytes++;
    
    /* Parse stream ID */
    if ((bytes = picoquic_frames_varint_decode(bytes, bytes_max, &stream_id)) == NULL) {
        picoquic_connection_error(cnx, PICOQUIC_TRANSPORT_FRAME_FORMAT_ERROR, 
            picoquic_frame_type_deadline_control);
        return NULL;
    }
    
    /* Parse deadline */
    if ((bytes = picoquic_frames_varint_decode(bytes, bytes_max, &deadline_ms)) == NULL) {
        picoquic_connection_error(cnx, PICOQUIC_TRANSPORT_FRAME_FORMAT_ERROR, 
            picoquic_frame_type_deadline_control);
        return NULL;
    }
    
    /* Find or create the stream */
    picoquic_stream_head_t* stream = picoquic_find_stream(cnx, stream_id);
    if (stream == NULL) {
        stream = picoquic_create_missing_streams(cnx, stream_id, 1); /* is_remote = 1 */
        if (stream == NULL) {
            return NULL; /* Error already set by create_missing_streams */
        }
    }
    
    /* Initialize deadline context if needed */
    if (stream->deadline_ctx == NULL) {
        picoquic_deadline_stream_init(stream);
        if (stream->deadline_ctx == NULL) {
            picoquic_connection_error(cnx, PICOQUIC_TRANSPORT_INTERNAL_ERROR, 0);
            return NULL;
        }
    }
    
    /* Set the deadline information */
    stream->deadline_ctx->deadline_ms = deadline_ms;
    stream->deadline_ctx->absolute_deadline = current_time + (deadline_ms * 1000); /* Convert ms to us */
    stream->deadline_ctx->deadline_type = 0; /* Default to soft deadline, can be overridden by API */
    stream->deadline_ctx->deadline_enabled = 1;
    
    return bytes;
}

/* Format DEADLINE_CONTROL frame */
uint8_t* picoquic_format_deadline_control_frame(uint8_t* bytes, uint8_t* bytes_max,
    uint64_t stream_id, uint64_t deadline_ms)
{
    if ((bytes = picoquic_frames_uint8_encode(bytes, bytes_max, 
            (uint8_t)picoquic_frame_type_deadline_control)) == NULL) {
        return NULL;
    }
    
    if ((bytes = picoquic_frames_varint_encode(bytes, bytes_max, stream_id)) == NULL) {
        return NULL;
    }
    
    if ((bytes = picoquic_frames_varint_encode(bytes, bytes_max, deadline_ms)) == NULL) {
        return NULL;
    }
    
    return bytes;
}

/* Skip STREAM_DATA_DROPPED frame */
const uint8_t* picoquic_skip_stream_data_dropped_frame(const uint8_t* bytes, const uint8_t* bytes_max)
{
    /* Frame type has already been skipped by frames.c */
    
    /* Skip stream ID */
    if ((bytes = picoquic_frames_varint_skip(bytes, bytes_max)) == NULL) {
        return NULL;
    }
    
    /* Skip offset */
    if ((bytes = picoquic_frames_varint_skip(bytes, bytes_max)) == NULL) {
        return NULL;
    }
    
    /* Skip length */
    if ((bytes = picoquic_frames_varint_skip(bytes, bytes_max)) == NULL) {
        return NULL;
    }
    
    return bytes;
}

/* Check if data can meet deadline on given path */
int picoquic_can_meet_deadline(picoquic_cnx_t* cnx, picoquic_stream_head_t* stream,
    picoquic_path_t* path, uint64_t current_time)
{
    if (stream == NULL || stream->deadline_ctx == NULL || 
        !stream->deadline_ctx->deadline_enabled) {
        return 1; /* No deadline, can always meet it */
    }
    
    /* Calculate time remaining until deadline */
    if (current_time >= stream->deadline_ctx->absolute_deadline) {
        return 0; /* Deadline already passed */
    }
    
    uint64_t time_remaining = stream->deadline_ctx->absolute_deadline - current_time;
    
    /* Estimate transmission time based on path RTT */
    /* Use smoothed RTT as estimate for one-way delay (divide by 2) */
    uint64_t estimated_transmission_time = path->smoothed_rtt / 2;
    
    /* Add some margin for processing and queuing delays */
    estimated_transmission_time += PICOQUIC_ACK_DELAY_MIN;
    
    return (estimated_transmission_time < time_remaining);
}

/* Select best path for deadline-aware stream with composite scoring */
picoquic_path_t* picoquic_select_path_for_deadline(picoquic_cnx_t* cnx,
    picoquic_stream_head_t* stream, uint64_t current_time)
{
    if (!cnx->is_multipath_enabled || stream == NULL || 
        stream->deadline_ctx == NULL || !stream->deadline_ctx->deadline_enabled) {
        /* Use default path selection */
        return cnx->path[0];
    }
    
    picoquic_path_t* best_path = NULL;
    double best_score = 0;
    
    /* Calculate deadline urgency */
    uint64_t time_to_deadline = (stream->deadline_ctx->absolute_deadline > current_time) ?
        stream->deadline_ctx->absolute_deadline - current_time : 0;
    
    /* Estimate stream data size (remaining to send) */
    uint64_t stream_bytes = 0;
    if (stream->send_queue != NULL) {
        picoquic_stream_queue_node_t* node = stream->send_queue;
        while (node != NULL) {
            stream_bytes += (node->length - node->offset);
            node = node->next_stream_data;
        }
    }
    
    /* If no data to send, return default path */
    if (stream_bytes == 0) {
        return cnx->path[0];
    }
    
    /* Iterate through all available paths */
    for (int i = 0; i < cnx->nb_paths; i++) {
        picoquic_path_t* path = cnx->path[i];
        
        /* Skip paths that are not ready */
        if (path->path_is_demoted || !path->rtt_is_initialized) {
            continue;
        }
        
        /* Calculate available congestion window */
        uint64_t available_cwnd = (path->cwin > path->bytes_in_transit) ? 
            (path->cwin - path->bytes_in_transit) : 0;
        
        /* Skip paths with no available congestion window */
        if (available_cwnd < PICOQUIC_MIN_SEGMENT_SIZE) {
            continue;
        }
        
        /* Calculate effective bandwidth considering congestion */
        uint64_t effective_bandwidth = path->bandwidth_estimate;
        if (effective_bandwidth == 0) {
            /* Estimate bandwidth from RTT and CWND if not measured */
            effective_bandwidth = (path->cwin * 1000000) / path->smoothed_rtt;
        }
        
        /* Adjust bandwidth based on congestion state */
        double congestion_factor = (double)available_cwnd / path->cwin;
        effective_bandwidth = (uint64_t)(effective_bandwidth * congestion_factor);
        
        /* Calculate transmission time */
        uint64_t transmission_time = 0;
        if (effective_bandwidth > 0) {
            transmission_time = (stream_bytes * 8 * 1000000) / effective_bandwidth;
        } else {
            /* Fallback: assume minimum bandwidth */
            transmission_time = (stream_bytes * 8 * 1000000) / 100000; /* 100 Kbps min */
        }
        
        /* Calculate total delivery time */
        uint64_t total_delivery_time = path->smoothed_rtt + transmission_time;
        
        /* Check if path can meet deadline */
        int can_meet_deadline = (total_delivery_time < time_to_deadline);
        
        /* Calculate composite score */
        double score = 0;
        
        if (can_meet_deadline || time_to_deadline == 0) {
            /* RTT component (normalized to milliseconds, inverse for scoring) */
            double rtt_score = 1000.0 / (path->smoothed_rtt / 1000.0 + 1.0);
            
            /* Bandwidth component (normalized to Mbps) */
            double bw_score = effective_bandwidth / 1000000.0;
            if (bw_score > 100.0) bw_score = 100.0; /* Cap at 100 Mbps */
            
            /* Loss rate component */
            double loss_rate = 0;
            if (path->bytes_sent > 0) {
                loss_rate = (double)path->total_bytes_lost / path->bytes_sent;
            }
            double loss_penalty = 1.0 - (loss_rate * 10.0); /* 10x penalty for loss */
            if (loss_penalty < 0.1) loss_penalty = 0.1; /* Min score */
            
            /* Congestion component */
            double congestion_score = congestion_factor;
            
            /* Deadline urgency bonus for paths that can meet deadline */
            double deadline_bonus = can_meet_deadline ? 2.0 : 1.0;
            
            /* Composite score with weights */
            score = (rtt_score * 0.3 +          /* 30% weight on RTT */
                    bw_score * 0.3 +            /* 30% weight on bandwidth */
                    loss_penalty * 0.2 +        /* 20% weight on loss */
                    congestion_score * 0.2) *   /* 20% weight on congestion */
                    deadline_bonus;             /* 2x bonus if can meet deadline */
            
            /* Penalty for paths with recent losses */
            if (current_time - path->last_loss_event_detected < 10 * path->smoothed_rtt) {
                score *= 0.5; /* Recent loss penalty */
            }
        } else {
            /* Path cannot meet deadline - give minimal score based on RTT only */
            score = 0.1 / (path->smoothed_rtt / 1000000.0 + 1.0);
        }
        
        if (score > best_score) {
            best_score = score;
            best_path = path;
        }
    }
    
    /* If no path selected, fall back to lowest RTT path */
    if (best_path == NULL && cnx->nb_paths > 0) {
        best_path = cnx->path[0];
        for (int i = 1; i < cnx->nb_paths; i++) {
            if (!cnx->path[i]->path_is_demoted &&
                cnx->path[i]->smoothed_rtt < best_path->smoothed_rtt) {
                best_path = cnx->path[i];
            }
        }
    }
    
    return best_path;
}

/* Initialize deadline context for connection */
void picoquic_init_deadline_context(picoquic_cnx_t* cnx)
{
    if (cnx->deadline_context == NULL) {
        cnx->deadline_context = (picoquic_deadline_context_t*)malloc(sizeof(picoquic_deadline_context_t));
        if (cnx->deadline_context != NULL) {
            memset(cnx->deadline_context, 0, sizeof(picoquic_deadline_context_t));
            /* Set deadline aware if both sides support it */
            cnx->deadline_context->deadline_aware_enabled = 
                (cnx->local_parameters.enable_deadline_aware_streams && 
                 cnx->remote_parameters.enable_deadline_aware_streams);
            cnx->deadline_context->deadline_scheduling_active = 
                cnx->deadline_context->deadline_aware_enabled;
            
            /* Initialize fairness parameters */
            cnx->deadline_context->min_non_deadline_share = 0.2;  /* 20% minimum for non-deadline */
            cnx->deadline_context->max_starvation_time = 50000;   /* 50ms max starvation */
            cnx->deadline_context->window_start_time = picoquic_get_quic_time(cnx->quic);
            
            /* Initialize path metrics */
            for (int i = 0; i < 16; i++) {
                memset(&cnx->deadline_context->path_metrics[i], 0, 
                    sizeof(cnx->deadline_context->path_metrics[i]));
            }
        }
    }
}

/* Free deadline context */
void picoquic_free_deadline_context(picoquic_cnx_t* cnx)
{
    if (cnx->deadline_context != NULL) {
        free(cnx->deadline_context);
        cnx->deadline_context = NULL;
    }
}

/* Check deadlines and drop expired data */
void picoquic_check_stream_deadlines(picoquic_cnx_t* cnx, uint64_t current_time)
{
    if (!cnx->deadline_context || !cnx->deadline_context->deadline_aware_enabled) {
        return;
    }
    
    /* Iterate through all streams */
    picoquic_stream_head_t* stream = picoquic_first_stream(cnx);
    
    while (stream != NULL) {
        picoquic_stream_head_t* next_stream = picoquic_next_stream(stream);
        
        if (stream->deadline_ctx != NULL && stream->deadline_ctx->deadline_enabled) {
            /* Check if deadline has passed */
            if (current_time >= stream->deadline_ctx->absolute_deadline) {
                if (stream->deadline_ctx->deadline_type == 1) { /* Hard deadline */
                    /* Calculate dropped bytes based on queued data */
                    uint64_t dropped_bytes = 0;
                    
                    /* Drop all queued data in send queue */
                    if (stream->send_queue != NULL) {
                        picoquic_stream_queue_node_t* node = stream->send_queue;
                        while (node != NULL) {
                            dropped_bytes += (node->length - node->offset);
                            node = node->next_stream_data;
                        }
                        
                        /* Free the send queue */
                        picoquic_stream_queue_node_t* current = stream->send_queue;
                        while (current != NULL) {
                            picoquic_stream_queue_node_t* next = current->next_stream_data;
                            if (current->bytes != NULL) {
                                free(current->bytes);
                            }
                            free(current);
                            current = next;
                        }
                        stream->send_queue = NULL;
                    }
                    
                    /* Record dropped range */
                    if (dropped_bytes > 0) {
                        uint64_t dropped_offset_start = stream->sent_offset;
                        uint64_t dropped_offset_end = stream->sent_offset + dropped_bytes;
                        
                        picoquic_update_sack_list(&stream->deadline_ctx->dropped_ranges,
                            dropped_offset_start, dropped_offset_end, current_time);
                        
                        stream->deadline_ctx->bytes_dropped += dropped_bytes;
                        
                        /* Queue STREAM_DATA_DROPPED frame to inform receiver */
                        picoquic_queue_stream_data_dropped_frame(cnx, stream->stream_id,
                            dropped_offset_start, dropped_bytes);
                    }
                    
                    stream->deadline_ctx->deadlines_missed++;
                    
                    /* If active stream, disable it */
                    if (stream->is_active) {
                        stream->is_active = 0;
                    }
                    
                    /* Mark stream as finished */
                    stream->fin_requested = 1;
                    
                    /* Notify application if callback is set */
                    if (cnx->deadline_context->on_deadline_missed != NULL) {
                        cnx->deadline_context->on_deadline_missed(cnx, stream->stream_id,
                            cnx->deadline_context->deadline_callback_ctx);
                    }
                }
                
                /* Disable deadline after it has passed */
                stream->deadline_ctx->deadline_enabled = 0;
            }
        }
        
        stream = next_stream;
    }
}

/* Check if stream data at given offset should be skipped due to deadline drop */
int picoquic_is_stream_data_dropped(picoquic_stream_head_t* stream, uint64_t offset, uint64_t length)
{
    if (stream == NULL || stream->deadline_ctx == NULL || 
        stream->deadline_ctx->dropped_ranges.ack_tree.root == NULL) {
        return 0; /* No dropped ranges */
    }
    
    /* Check if the data range overlaps with any dropped range */
    uint64_t data_end = offset + length;
    picosplay_node_t* node = picosplay_first(&stream->deadline_ctx->dropped_ranges.ack_tree);
    
    while (node != NULL) {
        picoquic_sack_item_t* sack = (picoquic_sack_item_t*)((char*)node - offsetof(struct st_picoquic_sack_item_t, node));
        
        /* Check for overlap */
        if (sack->start_of_sack_range < data_end && offset < sack->end_of_sack_range) {
            return 1; /* Data overlaps with dropped range */
        }
        
        /* If dropped range starts after our data ends, no need to check further */
        if (sack->start_of_sack_range >= data_end) {
            break;
        }
        
        node = picosplay_next(node);
    }
    
    return 0;
}

/* Deadline proximity threshold: streams within 10ms of each other 
 * are considered to have similar urgency and are scheduled round-robin */
#define DEADLINE_PROXIMITY_THRESHOLD_US 10000

/* Find the first non-expired chunk in a stream's send queue */
static picoquic_stream_queue_node_t* picoquic_find_first_valid_chunk(picoquic_stream_head_t* stream, uint64_t current_time)
{
    if (stream == NULL || stream->send_queue == NULL || stream->deadline_ctx == NULL) {
        return stream ? stream->send_queue : NULL;
    }
    
    /* For soft deadlines or non-deadline streams, all chunks are valid */
    if (!stream->deadline_ctx->deadline_enabled || stream->deadline_ctx->deadline_type == 0) {
        return stream->send_queue;
    }
    
    /* For hard deadlines, skip expired chunks */
    picoquic_stream_queue_node_t* chunk = stream->send_queue;
    while (chunk != NULL && 
           chunk->chunk_deadline != UINT64_MAX &&
           current_time >= chunk->chunk_deadline) {
        chunk = chunk->next_stream_data;
    }
    
    return chunk;
}

/* Find ready stream using EDF (Earliest Deadline First) scheduling with fairness */
picoquic_stream_head_t* picoquic_find_ready_stream_edf(picoquic_cnx_t* cnx, picoquic_path_t* path_x)
{
    picoquic_stream_head_t* stream = cnx->first_output_stream;
    picoquic_stream_head_t* best_stream = NULL;
    picoquic_stream_head_t* best_non_deadline_stream = NULL;
    picoquic_stream_head_t* earliest_not_sent_recently = NULL;
    uint64_t earliest_deadline = UINT64_MAX;
    uint64_t current_time = picoquic_get_quic_time(cnx->quic);
    uint64_t oldest_send_time = UINT64_MAX;
    
    /* Debug logging */
    static int debug_counter = 0;
    int should_debug = (debug_counter++ % 100 == 0); /* Debug every 100 calls */
    
    /* Check if fairness enforcement is needed */
    int force_non_deadline = 0;
    if (cnx->deadline_context != NULL) {
        /* Check for starvation timeout */
        if (cnx->deadline_context->last_non_deadline_scheduled > 0 &&
            current_time > cnx->deadline_context->last_non_deadline_scheduled + 
            cnx->deadline_context->max_starvation_time) {
            force_non_deadline = 1;
        }
        
        /* Check bandwidth share in current window */
        uint64_t window_duration = current_time - cnx->deadline_context->window_start_time;
        if (window_duration > 100000) { /* 100ms window */
            uint64_t total_bytes = cnx->deadline_context->deadline_bytes_sent + 
                                 cnx->deadline_context->non_deadline_bytes_sent;
            if (total_bytes > 0) {
                double non_deadline_share = (double)cnx->deadline_context->non_deadline_bytes_sent / total_bytes;
                if (non_deadline_share < cnx->deadline_context->min_non_deadline_share) {
                    force_non_deadline = 1;
                }
            }
            /* Reset window */
            cnx->deadline_context->window_start_time = current_time;
            cnx->deadline_context->deadline_bytes_sent = 0;
            cnx->deadline_context->non_deadline_bytes_sent = 0;
        }
    }
    
    /* First pass: find streams with data to send */
    while (stream != NULL) {
        picoquic_stream_head_t* next_stream = stream->next_output_stream;
        int has_data = 0;
        
        /* Check if stream has data to send */
        has_data = (cnx->maxdata_remote > cnx->data_sent && 
                   stream->sent_offset < stream->maxdata_remote && 
                   (stream->is_active ||
                    (stream->send_queue != NULL && stream->send_queue->length > stream->send_queue->offset) ||
                    (stream->fin_requested && !stream->fin_sent)));
                    
        /* Check path affinity if multipath is enabled */
        if (has_data && path_x != NULL && stream->affinity_path != path_x && stream->affinity_path != NULL) {
            has_data = 0;
        }
        
        /* Check stream ID limits */
        if (has_data && stream->sent_offset == 0) {
            if (IS_CLIENT_STREAM_ID(stream->stream_id) == cnx->client_mode) {
                if (stream->stream_id > ((IS_BIDIR_STREAM_ID(stream->stream_id)) ? 
                    cnx->max_stream_id_bidir_remote : cnx->max_stream_id_unidir_remote)) {
                    has_data = 0;
                }
            }
        }
        
        /* Handle urgent requests (reset/stop_sending) with highest priority */
        if ((stream->reset_requested && !stream->reset_sent) ||
            (stream->stop_sending_requested && !stream->stop_sending_sent)) {
            /* These take immediate precedence */
            best_stream = stream;
            break;
        }
        
        if (has_data) {
            if (should_debug) {
                printf("[EDF]   Stream %lu: has_data=1, deadline_ctx=%p, deadline_enabled=%d\n", 
                    stream->stream_id, (void*)stream->deadline_ctx,
                    stream->deadline_ctx ? stream->deadline_ctx->deadline_enabled : 0);
            }
            /* For deadline-aware streams */
            if (stream->deadline_ctx != NULL && stream->deadline_ctx->deadline_enabled) {
                /* Find the first non-expired chunk for per-chunk deadlines */
                picoquic_stream_queue_node_t* first_valid_chunk = picoquic_find_first_valid_chunk(stream, current_time);
                
                /* If all chunks are expired, stream has no valid data */
                if (stream->send_queue != NULL && first_valid_chunk == NULL) {
                    has_data = 0;
                    if (should_debug) {
                        DBG_PRINTF("  Stream %lu: all chunks expired, no data to send\n", stream->stream_id);
                    }
                }
                
                /* Process deadline stream if it still has data and we're not forcing non-deadline */
                if (has_data && !force_non_deadline) {
                    /* Get the deadline of the first non-expired chunk */
                    uint64_t effective_deadline = stream->deadline_ctx->absolute_deadline;
                    if (first_valid_chunk != NULL && first_valid_chunk->chunk_deadline != UINT64_MAX) {
                        effective_deadline = first_valid_chunk->chunk_deadline;
                    }
                    
                    if (should_debug) {
                        printf("[EDF]     Processing deadline stream %lu, effective_deadline=%lu, earliest=%lu\n",
                            stream->stream_id, effective_deadline, earliest_deadline);
                    }
                    /* Check if this stream has a deadline close to the earliest we've seen */
                    uint64_t deadline_threshold = (earliest_deadline == UINT64_MAX) ? UINT64_MAX : 
                                                  (earliest_deadline + DEADLINE_PROXIMITY_THRESHOLD_US);
                    if (effective_deadline <= deadline_threshold) {
                        /* This stream is in the same urgency group */
                        if (effective_deadline < earliest_deadline) {
                            /* New earliest deadline - reset the group */
                            earliest_deadline = effective_deadline;
                            oldest_send_time = stream->last_time_data_sent;
                            earliest_not_sent_recently = stream;
                            if (should_debug) {
                                printf("[EDF]     Stream %lu: NEW earliest deadline (%lu), last_send=%lu\n",
                                    stream->stream_id, effective_deadline, 
                                    stream->last_time_data_sent);
                            }
                        } else if (stream->last_time_data_sent <= oldest_send_time) {
                            /* Same urgency group, but sent less recently or equal - round robin */
                            oldest_send_time = stream->last_time_data_sent;
                            earliest_not_sent_recently = stream;
                            if (should_debug) {
                                printf("[EDF]     Stream %lu: same group, older send time (%lu <= %lu)\n",
                                    stream->stream_id, stream->last_time_data_sent, oldest_send_time);
                            }
                        }
                    }
                }
            } else {
                /* Non-deadline stream */
                if (best_non_deadline_stream == NULL) {
                    best_non_deadline_stream = stream;
                } else if ((stream->stream_priority & 1) != 0) {
                    /* FIFO processing */
                    best_non_deadline_stream = stream;
                } else if (stream->last_time_data_sent < best_non_deadline_stream->last_time_data_sent) {
                    /* Round-robin */
                    best_non_deadline_stream = stream;
                }
            }
        } else if (((stream->fin_requested && stream->fin_sent) || 
                    (stream->reset_requested && stream->reset_sent)) && 
                   (!stream->stop_sending_requested || stream->stop_sending_sent)) {
            /* Remove exhausted streams */
            picoquic_remove_output_stream(cnx, stream);
            picoquic_delete_stream_if_closed(cnx, stream);
        } else {
            /* Update flow control blocking indicators */
            if (stream->is_active ||
                (stream->send_queue != NULL && stream->send_queue->length > stream->send_queue->offset)) {
                if (stream->sent_offset >= stream->maxdata_remote) {
                    cnx->stream_blocked = 1;
                } else if (cnx->maxdata_remote <= cnx->data_sent) {
                    cnx->flow_blocked = 1;
                }
            }
        }
        
        stream = next_stream;
    }
    
    /* Select the best stream based on the analysis */
    if (should_debug) {
        printf("[EDF] Selection: force_non_deadline=%d, earliest_not_sent_recently=%p (stream %lu), best_non_deadline=%p (stream %lu)\n",
            force_non_deadline,
            (void*)earliest_not_sent_recently, 
            earliest_not_sent_recently ? earliest_not_sent_recently->stream_id : 0,
            (void*)best_non_deadline_stream,
            best_non_deadline_stream ? best_non_deadline_stream->stream_id : 0);
    }
    
    if (force_non_deadline && best_non_deadline_stream != NULL) {
        best_stream = best_non_deadline_stream;
    } else if (earliest_not_sent_recently != NULL) {
        best_stream = earliest_not_sent_recently;
    } else if (best_non_deadline_stream != NULL) {
        best_stream = best_non_deadline_stream;
    }
    
    /* Update fairness tracking and last send time */
    if (cnx->deadline_context != NULL && best_stream != NULL) {
        /* Update last send time for round-robin */
        best_stream->last_time_data_sent = current_time;
        
        if (best_stream->deadline_ctx != NULL && best_stream->deadline_ctx->deadline_enabled) {
            /* Selected a deadline stream */
            cnx->deadline_context->deadline_bytes_sent += PICOQUIC_MIN_SEGMENT_SIZE; /* Approximate */
        } else {
            /* Selected a non-deadline stream */
            cnx->deadline_context->non_deadline_bytes_sent += PICOQUIC_MIN_SEGMENT_SIZE; /* Approximate */
            cnx->deadline_context->last_non_deadline_scheduled = current_time;
        }
    }
    
    return best_stream;
}

/* Skip stream data in the send queue that falls within dropped ranges */
void picoquic_skip_dropped_stream_data(picoquic_stream_head_t* stream)
{
    if (stream == NULL || stream->deadline_ctx == NULL || stream->send_queue == NULL) {
        return;
    }
    
    picoquic_cnx_t* cnx = stream->cnx;
    uint64_t current_time = picoquic_get_quic_time(cnx->quic);
    picoquic_stream_queue_node_t* current = stream->send_queue;
    picoquic_stream_queue_node_t* prev = NULL;
    uint64_t stream_offset = stream->sent_offset;
    
    while (current != NULL) {
        int should_drop = 0;
        uint64_t drop_start = 0;
        uint64_t drop_length = 0;
        
        /* First, check if this chunk has expired based on per-chunk deadline */
        if (stream->deadline_ctx->deadline_type == 1 && /* hard deadline */
            current->chunk_deadline != UINT64_MAX &&
            current_time >= current->chunk_deadline) {
            /* This chunk has expired - drop it entirely */
            should_drop = 1;
            drop_start = stream_offset;
            drop_length = current->length - current->offset;
            
            DBG_PRINTF("Stream %lu: Dropping expired chunk at offset %lu, length %lu (deadline was %lu, now %lu)\n",
                stream->stream_id, drop_start, drop_length, 
                current->chunk_deadline, current_time);
        }
        
        if (should_drop) {
            /* Record the dropped range */
            picoquic_update_sack_list(&stream->deadline_ctx->dropped_ranges,
                drop_start, drop_start + drop_length, current_time);
            
            /* Queue STREAM_DATA_DROPPED frame */
            if (cnx->remote_parameters.enable_deadline_aware_streams) {
                picoquic_queue_stream_data_dropped_frame(cnx, stream->stream_id,
                    drop_start, drop_length);
            }
            
            /* Update statistics */
            stream->deadline_ctx->bytes_dropped += drop_length;
            
            /* Remove the expired chunk */
            picoquic_stream_queue_node_t* to_remove = current;
            current = current->next_stream_data;
            
            if (prev == NULL) {
                stream->send_queue = current;
            } else {
                prev->next_stream_data = current;
            }
            
            if (to_remove->bytes != NULL) {
                free(to_remove->bytes);
            }
            free(to_remove);
            
            /* Update the stream offset for the dropped data */
            stream_offset += drop_length;
        } else {
            /* Also check against existing dropped ranges (for compatibility) */
            uint64_t node_start = current->offset;
            uint64_t node_end = current->offset + current->length;
            uint64_t skip_offset = current->offset;
            
            /* Check if any part of this node is in dropped ranges */
            picosplay_node_t* drop_node = picosplay_first(&stream->deadline_ctx->dropped_ranges.ack_tree);
            
            while (drop_node != NULL && skip_offset < node_end) {
                picoquic_sack_item_t* drop_range = (picoquic_sack_item_t*)((char*)drop_node - offsetof(struct st_picoquic_sack_item_t, node));
                
                /* If drop range is completely after current node, we're done */
                if (drop_range->start_of_sack_range >= stream_offset + node_end - node_start) {
                    break;
                }
                
                /* If drop range overlaps with current position */
                if (drop_range->end_of_sack_range > stream_offset + skip_offset - node_start && 
                    drop_range->start_of_sack_range < stream_offset + node_end - node_start) {
                    
                    /* Calculate how much to skip */
                    uint64_t skip_start = (drop_range->start_of_sack_range > stream_offset + skip_offset - node_start) ? 
                                         drop_range->start_of_sack_range - stream_offset + node_start : skip_offset;
                    uint64_t skip_end = (drop_range->end_of_sack_range < stream_offset + node_end - node_start) ? 
                                       drop_range->end_of_sack_range - stream_offset + node_start : node_end;
                    
                    if (skip_start == node_start && skip_end == node_end) {
                        /* Entire node is dropped, remove it */
                        picoquic_stream_queue_node_t* to_remove = current;
                        current = current->next_stream_data;
                        
                        if (prev == NULL) {
                            stream->send_queue = current;
                        } else {
                            prev->next_stream_data = current;
                        }
                        
                        if (to_remove->bytes != NULL) {
                            free(to_remove->bytes);
                        }
                        free(to_remove);
                        
                        /* Update stream offset and continue without updating prev */
                        stream_offset += node_end - node_start;
                        goto next_node;
                    } else if (skip_start == node_start) {
                        /* Drop from beginning, adjust offset */
                        uint64_t skip_amount = skip_end - skip_start;
                        current->offset += skip_amount;
                        current->length -= skip_amount;
                        skip_offset = skip_end;
                    } else {
                        /* Drop in middle or end - for now, just advance skip_offset */
                        skip_offset = skip_end;
                    }
                }
                
                drop_node = picosplay_next(drop_node);
            }
            
            /* Move to next chunk */
            stream_offset += current->length - current->offset;
            prev = current;
            current = current->next_stream_data;
        }
        
next_node:
        ; /* Empty statement for label */
    }
}

/* Queue a STREAM_DATA_DROPPED frame for transmission */
int picoquic_queue_stream_data_dropped_frame(picoquic_cnx_t* cnx, uint64_t stream_id,
    uint64_t offset, uint64_t length)
{
    int ret = 0;
    size_t frame_size = 1 + /* frame type */
                       picoquic_encode_varint_length(stream_id) +
                       picoquic_encode_varint_length(offset) +
                       picoquic_encode_varint_length(length);
    
    uint8_t* frame_buffer = (uint8_t*)malloc(frame_size);
    if (frame_buffer == NULL) {
        ret = -1;
    } else {
        uint8_t* bytes = frame_buffer;
        uint8_t* bytes_max = frame_buffer + frame_size;
        
        /* Encode the frame */
        *bytes++ = (uint8_t)picoquic_frame_type_stream_data_dropped;
        bytes = picoquic_frames_varint_encode(bytes, bytes_max, stream_id);
        bytes = picoquic_frames_varint_encode(bytes, bytes_max, offset);
        bytes = picoquic_frames_varint_encode(bytes, bytes_max, length);
        
        /* Queue as misc frame for reliable delivery */
        ret = picoquic_queue_misc_frame(cnx, frame_buffer, frame_size, 0, picoquic_packet_context_application);
        
        if (ret != 0) {
            free(frame_buffer);
        }
    }
    
    return ret;
}

/* Parse STREAM_DATA_DROPPED frame */
const uint8_t* picoquic_parse_stream_data_dropped_frame(picoquic_cnx_t* cnx,
    const uint8_t* bytes, const uint8_t* bytes_max, uint64_t current_time)
{
    uint64_t stream_id = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
    
    /* Frame type has already been parsed by frames.c */
    
    /* Parse stream ID, offset, and length */
    if ((bytes = picoquic_frames_varint_decode(bytes, bytes_max, &stream_id)) == NULL) {
        picoquic_connection_error(cnx, PICOQUIC_TRANSPORT_FRAME_FORMAT_ERROR,
            picoquic_frame_type_stream_data_dropped);
        return NULL;
    }
    if ((bytes = picoquic_frames_varint_decode(bytes, bytes_max, &offset)) == NULL) {
        picoquic_connection_error(cnx, PICOQUIC_TRANSPORT_FRAME_FORMAT_ERROR,
            picoquic_frame_type_stream_data_dropped);
        return NULL;
    }
    if ((bytes = picoquic_frames_varint_decode(bytes, bytes_max, &length)) == NULL) {
        picoquic_connection_error(cnx, PICOQUIC_TRANSPORT_FRAME_FORMAT_ERROR,
            picoquic_frame_type_stream_data_dropped);
        return NULL;
    }
    
    /* Find or create stream */
    picoquic_stream_head_t* stream = picoquic_find_stream(cnx, stream_id);
    if (stream == NULL) {
        stream = picoquic_create_missing_streams(cnx, stream_id, 1); /* is_remote = 1 */
        if (stream == NULL) {
            return NULL; /* Error already set */
        }
    }
    
    /* Initialize deadline context if needed */
    if (stream->deadline_ctx == NULL) {
        picoquic_deadline_stream_init(stream);
    }
    
    if (stream->deadline_ctx != NULL) {
        /* Record the dropped range */
        picoquic_update_sack_list(&stream->deadline_ctx->receiver_dropped_ranges,
            offset, offset + length, current_time);
        
        /* Mark that we need to check if more data can be delivered */
        stream->is_output_stream = 1;
    }
    
    return bytes;
}

/* Skip over dropped ranges to find next valid offset */
uint64_t picoquic_skip_dropped_ranges(picoquic_sack_list_t* dropped_ranges, uint64_t offset)
{
    if (dropped_ranges == NULL || dropped_ranges->ack_tree.root == NULL) {
        return offset;
    }
    
    picosplay_node_t* node = picosplay_first(&dropped_ranges->ack_tree);
    
    while (node != NULL) {
        picoquic_sack_item_t* dropped = (picoquic_sack_item_t*)((char*)node - offsetof(struct st_picoquic_sack_item_t, node));
        
        if (dropped->end_of_sack_range <= offset) {
            /* This dropped range is before our offset */
            node = picosplay_next(node);
            continue;
        }
        
        if (dropped->start_of_sack_range <= offset && offset < dropped->end_of_sack_range) {
            /* We're in a dropped range, skip to the end */
            offset = dropped->end_of_sack_range;
        } else {
            /* The dropped range is after our offset */
            break;
        }
        
        node = picosplay_next(node);
    }
    
    return offset;
}

/* Helper function to find the earliest deadline for specific data range in a stream */
static uint64_t picoquic_get_packet_data_deadline(picoquic_stream_head_t* stream, 
    uint64_t offset, size_t length)
{
    uint64_t earliest = UINT64_MAX;
    uint64_t end_offset = offset + length;
    
    if (stream == NULL || stream->deadline_ctx == NULL || !stream->deadline_ctx->deadline_enabled) {
        return earliest;
    }
    
    /* If per-chunk deadlines aren't being used, return stream deadline */
    if (stream->send_queue == NULL) {
        return stream->deadline_ctx->absolute_deadline;
    }
    
    /* Find chunks that overlap with the packet data range */
    picoquic_stream_queue_node_t* node = stream->send_queue;
    while (node != NULL) {
        uint64_t chunk_start = node->offset;
        uint64_t chunk_end = node->offset + node->length;
        
        /* Check if this chunk overlaps with the packet data */
        if (chunk_start < end_offset && chunk_end > offset) {
            /* This chunk is at least partially in the packet */
            if (node->chunk_deadline != UINT64_MAX && node->chunk_deadline < earliest) {
                earliest = node->chunk_deadline;
            }
        }
        
        /* If we've passed the relevant range, we can stop */
        if (chunk_start >= end_offset) {
            break;
        }
        
        node = node->next_stream_data;
    }
    
    /* If no chunk deadlines found, fall back to stream deadline */
    if (earliest == UINT64_MAX && stream->deadline_ctx->absolute_deadline != 0) {
        earliest = stream->deadline_ctx->absolute_deadline;
    }
    
    return earliest;
}

/* Update packet deadline tracking based on stream data in packet */
void picoquic_update_packet_deadline_info(picoquic_cnx_t* cnx, picoquic_packet_t* packet, uint64_t current_time)
{
    if (!cnx->deadline_context || !cnx->deadline_context->deadline_aware_enabled || packet == NULL) {
        return;
    }
    
    /* Initialize deadline tracking */
    packet->earliest_deadline = UINT64_MAX;
    packet->contains_deadline_data = 0;
    
    /* If packet has stream data, check the specific chunk deadlines */
    if (packet->data_repeat_stream_id != (uint64_t)-1) {
        picoquic_stream_head_t* stream = picoquic_find_stream(cnx, packet->data_repeat_stream_id);
        if (stream != NULL && stream->deadline_ctx != NULL && stream->deadline_ctx->deadline_enabled) {
            packet->contains_deadline_data = 1;
            
            /* Use per-chunk deadline if we have offset/length info */
            if (packet->data_repeat_stream_data_length > 0) {
                packet->earliest_deadline = picoquic_get_packet_data_deadline(stream,
                    packet->data_repeat_stream_offset, packet->data_repeat_stream_data_length);
            } else {
                /* Fallback to stream deadline if no specific range info */
                packet->earliest_deadline = stream->deadline_ctx->absolute_deadline;
            }
        }
    }
    
    /* Alternative: parse packet bytes to find all stream frames and check their deadlines */
    /* This would be more comprehensive but also more expensive */
}

/* Check if packet contains expired deadline data that shouldn't be retransmitted */
int picoquic_should_skip_packet_retransmit(picoquic_cnx_t* cnx, picoquic_packet_t* packet, uint64_t current_time)
{
    if (!cnx->deadline_context || !cnx->deadline_context->deadline_aware_enabled || 
        !packet->contains_deadline_data) {
        /* No deadline data or deadline awareness disabled - normal retransmission */
        return 0;
    }
    
    /* Check if the deadline has expired */
    if (current_time >= packet->earliest_deadline) {
        /* Deadline has passed - check if it's a hard deadline */
        if (packet->data_repeat_stream_id != (uint64_t)-1) {
            picoquic_stream_head_t* stream = picoquic_find_stream(cnx, packet->data_repeat_stream_id);
            if (stream != NULL && stream->deadline_ctx != NULL && 
                stream->deadline_ctx->deadline_type == 1) {
                /* Hard deadline - need to check if ALL chunks in packet are expired */
                if (packet->data_repeat_stream_data_length > 0) {
                    /* For per-chunk deadlines, we've already computed the earliest deadline
                     * If that's expired and it's a hard deadline, skip retransmission */
                    return 1;
                } else {
                    /* No specific range, use stream-level deadline */
                    return (current_time >= stream->deadline_ctx->absolute_deadline) ? 1 : 0;
                }
            }
        }
    }
    
    return 0;
}

/* Select best path for deadline-aware retransmission */
picoquic_path_t* picoquic_select_path_for_retransmit(picoquic_cnx_t* cnx, picoquic_packet_t* packet, 
    uint64_t current_time)
{
    if (!cnx->deadline_context || !cnx->deadline_context->deadline_aware_enabled ||
        !packet->contains_deadline_data || !cnx->is_multipath_enabled) {
        /* Use original path for non-deadline or non-multipath connections */
        return packet->send_path;
    }
    
    /* Find the stream to get its properties */
    picoquic_stream_head_t* stream = NULL;
    if (packet->data_repeat_stream_id != (uint64_t)-1) {
        stream = picoquic_find_stream(cnx, packet->data_repeat_stream_id);
    }
    
    if (stream == NULL || stream->deadline_ctx == NULL) {
        return packet->send_path;
    }
    
    /* Use our existing path selection logic for deadline streams */
    return picoquic_select_path_for_deadline(cnx, stream, current_time);
}