/*
* Author: Claude (assistant to Tony)
* Copyright (c) 2025, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "picoquic_internal.h"
#include "picoquic_utils.h"
#include <stdlib.h>
#include <string.h>

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
                    /* Drop unsent data */
                    uint64_t dropped_offset_start = stream->sent_offset;
                    uint64_t dropped_offset_end = UINT64_MAX; /* Drop everything after sent_offset */
                    
                    /* Record dropped range */
                    picoquic_update_sack_list(&stream->deadline_ctx->dropped_ranges,
                        dropped_offset_start, dropped_offset_end, current_time);
                    
                    stream->deadline_ctx->bytes_dropped += (dropped_offset_end - dropped_offset_start);
                    stream->deadline_ctx->deadlines_missed++;
                    
                    /* Mark stream as finished if all data is dropped */
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