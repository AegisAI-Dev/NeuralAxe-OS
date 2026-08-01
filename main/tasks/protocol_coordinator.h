#ifndef PROTOCOL_COORDINATOR_H_
#define PROTOCOL_COORDINATOR_H_

#include "global_state.h"

// Initialize the coordinator (call once from main before starting the task)
void protocol_coordinator_init(GlobalState *gs);

// Main coordinator task — manages protocol lifecycle and fallback
void protocol_coordinator_task(void *pvParameters);

// Called by protocol tasks to signal connection failure
void protocol_coordinator_notify_failure(void);

// Called by protocol tasks once they've completed a successful setup
// (V1: STRATUM_RESULT_SETUP accepted, V2: handshake + channel opened).
// Resets the "all pools unreachable" failure counter and clears pools_unavailable.
void protocol_coordinator_notify_success(void);

// V1 task checks this to know when to shut down gracefully
bool protocol_coordinator_v1_should_shutdown(void);

// V1 task calls this right before deleting itself
void protocol_coordinator_v1_exited(void);

// V2 task checks this to know when to shut down gracefully
bool protocol_coordinator_v2_should_shutdown(void);

// V2 task calls this right before deleting itself
void protocol_coordinator_v2_exited(void);

#ifdef CONFIG_NX_TIMED_SESSIONS_EXECUTION
// NeuralAxe Gate B7 controlled protocol hooks. Only the timed-session
// executor (single B6 owner task) may call these, and only while the
// coordinator task itself is NOT running — the boot barrier withheld it in
// every posture where the executor acts. They reuse the coordinator's
// internal start/stop/event machinery so exactly one protocol engine exists.

// Bounded protocol event bits returned by nx_protocol_ctrl_poll_events().
#define NX_PROTOCOL_EVT_FAILED         (1u << 0)
#define NX_PROTOCOL_EVT_SETUP_SUCCESS  (1u << 1)
#define NX_PROTOCOL_EVT_TASK_EXITED    (1u << 2)

typedef struct {
    uint64_t work_received;
    uint64_t shares_accepted;
    uint64_t shares_rejected;
    uint32_t queue_depth;
} nx_protocol_counters_t;

// Start one controlled stratum task for the CURRENT primary configuration
// (is_using_fallback is pinned false; stale events are drained; the job
// queue and share stats are reset so counters baseline a fresh generation).
// Refused while the coordinator task or another controlled task is running.
bool nx_protocol_ctrl_start(stratum_protocol_t protocol);

// Bounded controlled stop handshake. True once the task provably exited.
bool nx_protocol_ctrl_stop(void);

// True while a controlled stratum task is believed running.
bool nx_protocol_ctrl_running(void);

// Drain pending coordinator events into NX_PROTOCOL_EVT_* bits.
uint32_t nx_protocol_ctrl_poll_events(void);

// Sample the audited counters (never resets them).
void nx_protocol_ctrl_counters(nx_protocol_counters_t *out);

// Post-COMPLETE handoff: start the production coordinator task so normal
// source operation (failover/pause/heartbeat) resumes. One-shot; refused if
// the coordinator task already exists or a controlled task is running.
bool nx_protocol_ctrl_handoff_to_coordinator(void);
#endif // CONFIG_NX_TIMED_SESSIONS_EXECUTION

#endif // PROTOCOL_COORDINATOR_H_
