# Deadline-Aware Streams Implementation

This document describes the implementation of a deadline-aware streams for picoquic. The implementation follows [draft-tjohn-quic-multipath-dmtp-01](https://datatracker.ietf.org/doc/draft-tjohn-quic-multipath-dmtp/) with a few deviations.

## Key Design Decisions

### Transport Parameter
The implementation retains the `enable_deadline_aware_streams` transport parameter from the draft specification. This zero-length flag is exchanged during the handshake to signal mutual support for deadline-aware behavior between endpoints. The parameter enables features such as path-specific probe replies and deadline-aware scheduling.

```c
/* In picoquic_internal.h */
unsigned int enable_deadline_aware_streams : 1;  /* Flag in cnx structure */

/* In tls_api.c - transport parameter encoding/decoding */
case picoquic_tp_enable_deadline_aware_streams:
    /* Zero-length parameter - presence indicates support */
```

### No DEADLINE_CONTROL Frame
The most significant deviation from the draft specification is the elimination of the DEADLINE_CONTROL frame. Instead of negotiating deadlines over the wire, the sender maintains local control. This design choice provides:
- Elimination of additional round trips for deadline establishment
- Reduced parsing overhead by avoiding new frame types
- Simplified implementation without new state machines
- Compatibility with any QUIC receiver, regardless of DMTP support

### Stream Infrastructure
Each deadline stream tracks several key pieces of information:

```c
/* Added to picoquic_stream_head_t */
uint64_t deadline_ms;           /* Relative deadline from enqueue time */
uint64_t expired_bytes;         /* Bytes we gave up on */
uint64_t expiry_threshold_abs;  /* Abort if expired_bytes exceeds this */
uint8_t expiry_threshold_pct;   /* Abort if expired % exceeds this */
int is_deadline_aware;          /* Is this a deadline stream? */

/* Added to picoquic_stream_queue_node_t */
uint64_t enqueue_time;          /* When this chunk was queued */
```

The implementation tracks per-chunk timing by storing the enqueue time with each data chunk. This enables intelligent transmission decisions and identification of expired data. For example:

```
Chunk enqueued at T=1000ms with 50ms deadline
At T=1040ms: Can still make it (10ms margin)
At T=1055ms: Skip it, already late
```

### Scheduling Algorithm
The implementation leverages picoquic's existing priority system rather than introducing a new scheduler. Deadline urgency is dynamically mapped to priority values.

This approach seamlessly integrates with the existing priority-ordered stream lists and selection logic. Urgency calculations based on remaining time drive priority updates during stream selection.

### Retransmissions
Retransmissions are performed on the fastest available path with sufficient congestion window capacity.

### Stream Abort Mechanism
When expired data exceeds configured thresholds, the stream is aborted using standard QUIC mechanisms:

```c
/* Check if we should abort (called after marking data expired) */
bool should_abort = false;

/* Absolute threshold check */
if (stream->expired_bytes >= stream->expiry_threshold_abs) {
    should_abort = true;
}

/* Percentage threshold check */
uint64_t total_sent = stream->expired_bytes + stream->sent_bytes;
if (total_sent > 0) {
    uint8_t expired_pct = (stream->expired_bytes * 100) / total_sent;
    if (expired_pct >= stream->expiry_threshold_pct) {
        should_abort = true;
    }
}

if (should_abort) {
    /* Send RESET_STREAM with our custom error code */
    picoquic_reset_stream(cnx, stream_id, PICOQUIC_ERROR_DEADLINE_EXCEEDED);
}
```

The receiver is notified through the standard stream reset callback, enabling appropriate handling of partial data delivery.

## API Design

```c
/* Create a new deadline-aware stream */
uint64_t picoquic_create_deadline_stream(
    picoquic_cnx_t* cnx,
    uint64_t deadline_ms,        /* Relative deadline for all data */
    uint64_t threshold_bytes,    /* Abort if this many bytes expire */
    uint8_t threshold_percent    /* Abort if this % expires */
);

```

## Multipath Integration

For multipath scenarios, the path selection algorithm incorporates deadline considerations. The system estimates arrival time for each available path and selects the optimal one:

```c
/* Path selection for deadline chunks */
for each available path p:
    /* Calculate Estimated Time of Arrival */
    eta[p] = current_time 
           + queued_bytes[p] / bandwidth[p]  /* Transmission time */
           + rtt[p] / 2;                     /* One-way delay estimate */
    
    if (eta[p] <= chunk_deadline) {
        candidate_paths.add(p);
    }

if (candidate_paths.empty()) {
    /* No path can meet deadline - expire the chunk */
    mark_expired(chunk);
} else {
    /* Pick path with smallest ETA */
    best_path = min_eta(candidate_paths);
    send_on_path(best_path, chunk);
}
```

The implementation currently employs the following heuristics:
- One-way delay (OWD) approximated as RTT/2
- Bandwidth estimates derived from congestion control
- Cross-path probing not yet implemented (planned enhancement)

## Testing Infrastructure

A comprehensive evaluation [framework](picoquictest/multipath_comprehensive_eval.c) validates performance across real-world scenarios using picoquic's simulation framework.

The evaluation harness extends `picoquictest` with 40 distinct test cases (5 application scenarios × 4 multipath configurations × vanilla vs deadline‑aware). Each run produces a csv file with the following metrics:

* **Deadline compliance** (% of chunks delivered before their deadline)
* **End‑to‑end latency** (95th‑percentile one‑way delay)
* **Throughput** and **per‑path utilisation**

### Highlights

| Metric                    | Aggregate result              |
| ------------------------- | ----------------------------- |
| Average compliance uplift | **+18 pp** across the matrix  |
| Median latency change     | **–1.2 ms** (lower is better) |
| Throughput impact         | **±0.01 Mbps** (negligible)   |

**Largest wins**

* **Live‑stream + Asymmetric** – +88 pp compliance, –35 ms tail latency.
* **Video‑conf + Asymmetric** – +60 pp compliance; conversation retains interactivity under congestion.
* **Mixed‑media + Sat/Terrestrial** – +50 pp compliance, –14 ms jitter on interactive flows.

## Current Limitations and Mitigation Strategies

1. **FEC**: Forward Error Correction not yet implemented.

2. **Scheduling Fairness**: Current priority mapping may result in starvation of lower-priority streams.
   - *Mitigation*: Careful priority range allocation
   - *Planned Enhancement*: Deficit round-robin (DRR) implementation within priority levels