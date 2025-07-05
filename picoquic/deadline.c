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
        }
    }
}

/* Clean up deadline context for a stream */
void picoquic_deadline_stream_free(picoquic_stream_head_t* stream)
{
    if (stream != NULL && stream->deadline_ctx != NULL) {
        picoquic_sack_list_free(&stream->deadline_ctx->dropped_ranges);
        free(stream->deadline_ctx);
        stream->deadline_ctx = NULL;
    }
}

/* Set deadline for a stream */
int picoquic_set_stream_deadline(picoquic_cnx_t* cnx, uint64_t stream_id, 
    uint64_t deadline_ms, int is_hard)
{
    int ret = 0;
    picoquic_stream_head_t* stream = picoquic_find_stream(cnx, stream_id);
    
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

/* Select best path for deadline-aware stream */
picoquic_path_t* picoquic_select_path_for_deadline(picoquic_cnx_t* cnx,
    picoquic_stream_head_t* stream, uint64_t current_time)
{
    if (!cnx->is_multipath_enabled || stream == NULL || 
        stream->deadline_ctx == NULL || !stream->deadline_ctx->deadline_enabled) {
        /* Use default path selection */
        return cnx->path[0];
    }
    
    picoquic_path_t* best_path = NULL;
    uint64_t best_score = UINT64_MAX;
    
    /* Iterate through all available paths */
    for (int i = 0; i < cnx->nb_paths; i++) {
        picoquic_path_t* path = cnx->path[i];
        
        /* Skip paths that are not ready */
        if (path->path_is_demoted) {
            continue;
        }
        
        /* Check if path can meet deadline */
        if (!picoquic_can_meet_deadline(cnx, stream, path, current_time)) {
            continue;
        }
        
        /* Score based on RTT (lower is better) */
        uint64_t score = path->smoothed_rtt;
        
        /* Adjust score based on congestion window availability */
        if (path->bytes_in_transit >= path->cwin) {
            score = score * 2; /* Penalize congested paths */
        }
        
        if (score < best_score) {
            best_score = score;
            best_path = path;
        }
    }
    
    /* If no path can meet deadline, still return the fastest path */
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

/* Find ready stream using EDF (Earliest Deadline First) scheduling */
picoquic_stream_head_t* picoquic_find_ready_stream_edf(picoquic_cnx_t* cnx, picoquic_path_t* path_x)
{
    picoquic_stream_head_t* stream = cnx->first_output_stream;
    picoquic_stream_head_t* best_stream = NULL;
    uint64_t earliest_deadline = UINT64_MAX;
    uint64_t current_time = picoquic_get_quic_time(cnx->quic);
    
    /* First pass: find stream with earliest deadline that has data to send */
    while (stream != NULL) {
        picoquic_stream_head_t* next_stream = stream->next_output_stream;
        int has_data = 0;
        
        /* Check if stream has data to send (same logic as original function) */
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
            /* For deadline-aware streams, use EDF */
            if (stream->deadline_ctx != NULL && stream->deadline_ctx->deadline_enabled) {
                /* Skip if deadline already passed and it's a hard deadline */
                if (current_time >= stream->deadline_ctx->absolute_deadline && 
                    stream->deadline_ctx->deadline_type == 1) {
                    /* This data should be dropped, not sent */
                    has_data = 0;
                } else if (stream->deadline_ctx->absolute_deadline < earliest_deadline) {
                    earliest_deadline = stream->deadline_ctx->absolute_deadline;
                    best_stream = stream;
                }
            } else if (best_stream == NULL || 
                      (best_stream->deadline_ctx == NULL || !best_stream->deadline_ctx->deadline_enabled)) {
                /* Non-deadline streams are considered after deadline streams */
                /* Use original priority/round-robin logic for non-deadline streams */
                if ((stream->stream_priority & 1) != 0) {
                    /* FIFO processing */
                    best_stream = stream;
                } else if (stream->last_time_data_sent < best_stream->last_time_data_sent) {
                    /* Round-robin */
                    best_stream = stream;
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
    
    return best_stream;
}

/* Skip stream data in the send queue that falls within dropped ranges */
void picoquic_skip_dropped_stream_data(picoquic_stream_head_t* stream)
{
    if (stream == NULL || stream->deadline_ctx == NULL || stream->send_queue == NULL) {
        return;
    }
    
    picoquic_stream_queue_node_t* current = stream->send_queue;
    picoquic_stream_queue_node_t* prev = NULL;
    
    while (current != NULL) {
        uint64_t node_start = current->offset;
        uint64_t node_end = current->offset + current->length;
        uint64_t skip_offset = current->offset;
        
        /* Check if any part of this node is dropped */
        picosplay_node_t* drop_node = picosplay_first(&stream->deadline_ctx->dropped_ranges.ack_tree);
        
        while (drop_node != NULL && skip_offset < node_end) {
            picoquic_sack_item_t* drop_range = (picoquic_sack_item_t*)((char*)drop_node - offsetof(struct st_picoquic_sack_item_t, node));
            
            /* If drop range is completely after current node, we're done */
            if (drop_range->start_of_sack_range >= node_end) {
                break;
            }
            
            /* If drop range overlaps with current position */
            if (drop_range->end_of_sack_range > skip_offset && 
                drop_range->start_of_sack_range < node_end) {
                
                /* Calculate how much to skip */
                uint64_t skip_start = (drop_range->start_of_sack_range > skip_offset) ? 
                                     drop_range->start_of_sack_range : skip_offset;
                uint64_t skip_end = (drop_range->end_of_sack_range < node_end) ? 
                                   drop_range->end_of_sack_range : node_end;
                
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
                    
                    /* Continue with next node without updating prev */
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
        
        prev = current;
        current = current->next_stream_data;
        
next_node:
        ; /* Empty statement for label */
    }
}