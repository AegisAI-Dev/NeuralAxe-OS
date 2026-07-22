#ifndef POOL_TIME_H_
#define POOL_TIME_H_

#include <stdint.h>
#include <stdbool.h>

/*
 * NeuralAxe trusted-time foundation for timed pool sessions
 * (Phase 2M.1B, Gate B2). Board 601 / BM1370 only.
 *
 * This header is the PURE time domain: abstract clock contract, trusted-time
 * predicate, SNTP anchor validation, overflow-safe monotonic and UTC deadline
 * helpers, bounded synchronization-window policy and the pure reboot/recovery
 * time decision. Nothing here touches ESP-IDF, networking, NVS, FreeRTOS,
 * heap, logging or global mutable state.
 *
 * Architectural sources of truth (committed Phase 2M.1A + Gate B1):
 *   docs/NEURALAXE_PHASE_2M1A_TIMED_POOL_SESSIONS_AUDIT.md          (esp. §7)
 *   docs/NEURALAXE_PHASE_2M1A_TIMED_POOL_SESSIONS_STATE_MACHINE.md
 *   docs/NEURALAXE_PHASE_2M1A_TIMED_POOL_SESSIONS_SECURITY.md       (T16/T17)
 *   docs/NEURALAXE_PHASE_2M1B_B1_FSM_FOUNDATION_REPORT.md
 *
 * TRUST MODEL (non-negotiable):
 *  - "Trusted time" in this module means EXACTLY: time accepted by the
 *    NeuralAxe scheduler trust policy after an SNTP callback in the current
 *    boot. It is an OPERATIONAL trust verdict, not a cryptographic one.
 *  - Pool-supplied Stratum ntime is NEVER a trusted time source for session
 *    deadlines. The existing cosmetic settimeofday() driven by ntime
 *    (SYSTEM_notify_new_ntime) is untouched and is invisible to this module.
 *  - A raw system wall clock is NOT trusted merely because it holds a
 *    plausible epoch. Trusted status requires an SNTP synchronization
 *    completed DURING THE CURRENT BOOT, captured as an anchor pairing the
 *    SNTP-synchronized UTC epoch with the monotonic clock at that instant.
 *  - Trusted UTC estimates derive ONLY from anchor_epoch + monotonic_elapsed.
 *    They never re-read time()/gettimeofday(), so later settimeofday() calls
 *    (e.g. from a Stratum job) cannot move the trusted scheduler clock.
 *  - Trusted time never moves backwards within one boot: a resynchronization
 *    that would regress the trusted estimate is rejected and the previous
 *    anchor is kept.
 *  - After reboot no previous in-memory anchor survives; time is untrusted
 *    until a fresh SNTP synchronization completes. Recovery waits a BOUNDED
 *    window for trust and then fails safe toward restore.
 *
 * TRUST-BOUNDARY PRECISION (honest limits of the above):
 *  1. Snapshot VALIDITY — what pure code here can and does check: flags,
 *     status, epoch sanity band, sync generation, monotonic/anchor-age
 *     consistency, overflow, required-minimum epoch, internal field
 *     consistency. This is CONSISTENCY CHECKING, not authentication.
 *  2. Operational PROVENANCE — snapshots are intended to be obtained through
 *     pool_time_snapshot() (or the *_from_clock helpers) from the
 *     provider-owned accepted anchor. That is an internal API contract.
 *  3. Cryptographic AUTHENTICITY — ABSENT. A PoolTimeSnapshot is a plain C
 *     value struct with no MAC, signature, opaque handle or capability. Code
 *     already executing inside the firmware can manufacture a fully
 *     self-consistent snapshot that no pure check can detect. Nothing in
 *     this module claims otherwise.
 *
 * NETWORK-TRUST RESIDUAL (documented, not hidden by the word "trusted"):
 *  - Standard SNTP and DNS as used by this design are NOT cryptographically
 *    authenticated (no NTS, no DNSSEC). The anchor prevents later Stratum
 *    settimeofday() manipulation, and the sanity band + anti-regression
 *    checks detect classes of error — but they do NOT authenticate the
 *    remote NTP server or the network path. Malicious DNS/NTP manipulation
 *    is a documented network-trust residual (bounded by the sanity band,
 *    monotonic-only same-boot timing, the bounded sync window and fail-safe
 *    restore). It does not block the local board-601 MVP.
 *
 * SECURITY INVARIANT: no password, pool account, wallet, hostname or address
 * field exists anywhere in this model, and no machine-readable token ever
 * carries one.
 */

#define POOL_TIME_MODEL_VERSION 1u

/*
 * Compiled epoch sanity band (UTC seconds). An SNTP-synchronized epoch outside
 * this band is rejected as untrusted regardless of source:
 *   min: 2025-01-01T00:00:00Z  (firmware demonstrably built after this)
 *   max: 2100-01-01T00:00:00Z  (far-future ceiling; also below the lwIP SNTP
 *                               era-convention limit of 2104)
 */
#define POOL_TIME_EPOCH_MIN_S 1735689600ull
#define POOL_TIME_EPOCH_MAX_S 4102444800ull

/*
 * Session duration bounds (seconds): 15 minutes .. 24 hours. These mirror the
 * committed Gate B1 pool_session bounds (asserted equal in the test suite
 * without coupling the production components).
 */
#define POOL_TIME_MIN_DURATION_S 900u
#define POOL_TIME_MAX_DURATION_S 86400u

/*
 * Bounded post-reboot SNTP synchronization window (seconds): how long recovery
 * may wait for fresh trusted time before a fail-safe restore decision.
 * Phase 2M.1A §7: proposed default 10 minutes, hard maximum 15 minutes.
 */
#define POOL_TIME_SYNC_WAIT_DEFAULT_S 600u
#define POOL_TIME_SYNC_WAIT_MAX_S     900u

#define POOL_TIME_US_PER_S 1000000ull

/* ------------------------------------------------------------------ */
/* Machine-readable time status codes                                  */
/* ------------------------------------------------------------------ */

/*
 * Stable machine codes (Phase 2M.1B B2 Stage 11). Public tokens only — no
 * server hostname, DNS detail, pool identifier, address or raw ESP-IDF error
 * text is ever embedded in these or their string forms.
 */
typedef enum {
    TIME_OK = 0,
    TIME_ERR_INVALID_ARGUMENT,
    TIME_ERR_NOT_INITIALIZED,
    TIME_ERR_NOT_SYNCED,
    TIME_ERR_SYNC_PENDING,
    TIME_ERR_SYNC_TIMEOUT,
    TIME_ERR_SNTP_INIT,
    TIME_ERR_SNTP_START,
    TIME_ERR_SNTP_STOP,
    TIME_ERR_INVALID_SERVER_CONFIG,
    TIME_ERR_INVALID_WAIT_WINDOW,
    TIME_ERR_EPOCH_BELOW_MIN,
    TIME_ERR_EPOCH_ABOVE_MAX,
    TIME_ERR_EPOCH_BEFORE_REQUIRED_MIN,
    TIME_ERR_MONOTONIC_REGRESSION,
    TIME_ERR_TRUST_REGRESSION,
    TIME_ERR_DURATION_INVALID,
    TIME_ERR_OVERFLOW,
    TIME_ERR_NO_SOURCE_SNAPSHOT,
    TIME_ERR_NO_PERSISTED_DEADLINE,
    TIME_ERR_RECOVERY_REQUIRED,
    POOL_TIME_ERR__COUNT
} PoolTimeError;

/* Synchronization-status metadata carried by the anchor (not a trust source). */
typedef enum {
    POOL_TIME_SYNC_STATUS_NONE = 0,   /* no synchronization attempted          */
    POOL_TIME_SYNC_STATUS_PENDING,    /* service started, no completed sync    */
    POOL_TIME_SYNC_STATUS_COMPLETED,  /* at least one accepted sync this boot  */
    POOL_TIME_SYNC_STATUS__COUNT
} PoolTimeSyncStatus;

/* ------------------------------------------------------------------ */
/* Time-domain models                                                  */
/* ------------------------------------------------------------------ */

/*
 * The SNTP-to-monotonic anchor. Captured ONLY on an actual SNTP
 * synchronization callback: the server-supplied UTC instant paired with
 * esp_timer_get_time() read at that moment. It contains no hostname, no
 * address, no pool data and no secret.
 */
typedef struct {
    bool               valid;                    /* anchor holds an accepted sync   */
    bool               sync_completed_this_boot; /* accepted sync in CURRENT boot   */
    uint32_t           generation;               /* saturating; bumps per accept    */
    uint64_t           epoch_us_at_sync;         /* UTC microseconds at sync        */
    uint64_t           monotonic_us_at_sync;     /* monotonic microseconds at sync  */
    PoolTimeSyncStatus sync_status;              /* metadata only, never a trust in */
    PoolTimeError      last_error;               /* last accept/reject code         */
} PoolTimeAnchor;

/*
 * Trust policy. All bounds are explicit and bounded; `required_min_epoch_s`
 * (0 = unused) is a caller-supplied anti-regression floor: trusted time
 * earlier than it is rejected.
 *
 * GATE B3/B4 PREREQUISITE (exact): after a reboot, a floor of only the
 * original verified_start_epoch is too weak — an erroneous or malicious time
 * source could repeatedly report a value near the session start and extend
 * the apparent remaining duration on every reboot. Gate B3 must therefore
 * persist a monotonically-advancing "latest accepted trusted epoch" (e.g.
 * refreshed on the TARGET_ACTIVE heartbeat) plus bounded reboot/recovery
 * counters, and Gate B4 must supply that persisted floor — not merely
 * verified_start_epoch — as `required_min_epoch_s`. This B2 contract already
 * accepts any such stronger floor; nothing here prevents it. B2 itself
 * persists nothing.
 */
typedef struct {
    uint64_t min_epoch_s;          /* compiled sanity floor                    */
    uint64_t max_epoch_s;          /* compiled sanity ceiling                  */
    uint64_t required_min_epoch_s; /* cross-reboot floor; 0 disables           */
    uint32_t sync_wait_default_s;  /* default bounded sync window              */
    uint32_t sync_wait_max_s;      /* hard maximum bounded sync window         */
} PoolTimeTrustPolicy;

/*
 * One coherent read of the clock. `trusted` is true ONLY when the full
 * trusted-time predicate passes; `status` carries the machine-readable reason
 * otherwise. No string fields exist in this struct by design.
 */
typedef struct {
    uint64_t      monotonic_now_us;
    bool          trusted;
    uint64_t      trusted_epoch_s;  /* whole seconds; valid only when trusted  */
    uint64_t      trusted_utc_us;   /* microsecond precision; only when trusted*/
    uint64_t      anchor_age_us;    /* monotonic_now - anchor monotonic        */
    uint32_t      sync_generation;
    PoolTimeError status;           /* TIME_OK exactly when trusted            */
} PoolTimeSnapshot;

/* ------------------------------------------------------------------ */
/* Abstract clock contract                                             */
/* ------------------------------------------------------------------ */

/*
 * Injectable clock: an ops table plus context pointer. Implementations must be
 * safe against concurrent anchor writers (read_anchor returns a consistent
 * copy, never a torn one). The production implementation is the SNTP provider
 * (pool_time_sntp.h); tests use deterministic fakes.
 */
typedef struct {
    /* Monotonic microseconds since boot. Never wall-clock derived. */
    uint64_t (*monotonic_us)(void *ctx);
    /*
     * Copy the currently published anchor into *out. Returns false when the
     * provider is not initialized (no anchor state exists at all); returns
     * true otherwise, including when the copied anchor is not yet valid.
     */
    bool (*read_anchor)(void *ctx, PoolTimeAnchor *out);
} PoolTimeClockOps;

typedef struct {
    const PoolTimeClockOps *ops;
    void                   *ctx;
} PoolTimeClock;

/* ------------------------------------------------------------------ */
/* Deadline models                                                     */
/* ------------------------------------------------------------------ */

/* Boot-scoped monotonic deadline. No wall-clock dependency of any kind. */
typedef struct {
    bool     valid;
    uint64_t armed_at_monotonic_us;
    uint64_t deadline_monotonic_us;
    uint32_t requested_duration_s;
} PoolTimeMonotonicDeadline;

/* Result of one deadline evaluation. */
typedef struct {
    bool          expired;      /* true when now >= deadline (or fail-safe)    */
    uint64_t      remaining_us; /* 0 when expired                              */
    uint32_t      remaining_s;  /* ceiling of remaining_us; 0 only if expired  */
    PoolTimeError status;
} PoolTimeDeadlineCheck;

/*
 * Cross-reboot UTC deadline. Creatable ONLY from a trusted snapshot (SNTP
 * anchor from the current boot) — never from raw system time, Stratum ntime,
 * an untrusted snapshot or a previous-boot value.
 */
typedef struct {
    bool     valid;
    uint64_t deadline_epoch_s;
    uint32_t requested_duration_s;
    uint32_t created_generation; /* sync generation the deadline derives from  */
} PoolTimeUtcDeadline;

/* ------------------------------------------------------------------ */
/* Reboot/recovery time decision (pure; consumed by Gate B4 later)     */
/* ------------------------------------------------------------------ */

typedef enum {
    POOL_TIME_DECISION_WAIT_FOR_TRUSTED_TIME = 0,
    POOL_TIME_DECISION_RESUME_TARGET_WITH_REMAINING_TIME,
    POOL_TIME_DECISION_RESTORE_DUE,
    POOL_TIME_DECISION_FAIL_SAFE_RESTORE,
    POOL_TIME_DECISION_RECOVERY_REQUIRED,
    POOL_TIME_DECISION__COUNT
} PoolTimeRecoveryDecision;

/*
 * Time-layer inputs at boot recovery. This deliberately models NO pool state,
 * NO NVS layout and NO secrets — only the timing facts Gate B4 will hold.
 */
typedef struct {
    bool     source_snapshot_valid;       /* the persisted restore identity OK */
    bool     utc_deadline_valid;          /* a persisted UTC deadline exists   */
    uint64_t deadline_epoch_s;
    bool     verified_start_epoch_valid;  /* persisted verified-start exists   */
    uint64_t verified_start_epoch_s;
    bool     time_trusted;                /* fresh SNTP trust THIS boot        */
    uint64_t trusted_epoch_s;             /* only meaningful when trusted      */
    uint32_t sync_wait_elapsed_s;         /* bounded-window progress           */
    uint32_t sync_wait_limit_s;           /* bounded-window limit              */
} PoolTimeRecoveryInput;

typedef struct {
    PoolTimeRecoveryDecision decision;
    bool          remaining_valid;        /* remaining_s carries a value       */
    uint64_t      remaining_s;
    PoolTimeError status;
    bool          requires_trusted_time;  /* progress still depends on trust   */
    bool          terminal_for_time;      /* the time layer is done waiting    */
} PoolTimeRecoveryResult;

/* ------------------------------------------------------------------ */
/* Pure API                                                            */
/* ------------------------------------------------------------------ */

/* Fill the compiled default trust policy (band + bounded sync window). */
void pool_time_trust_policy_defaults(PoolTimeTrustPolicy *p);

/* Structural validity of a policy (bounds ordered, window bounded, non-zero). */
bool pool_time_trust_policy_valid(const PoolTimeTrustPolicy *p);

/*
 * Validate a candidate anchor for first acceptance. Checks: completed-this-
 * boot flag, epoch sanity band. Returns TIME_OK when acceptable.
 */
PoolTimeError pool_time_validate_anchor(const PoolTimeAnchor *candidate,
                                        const PoolTimeTrustPolicy *policy);

/*
 * Validate a resynchronization candidate against the currently accepted
 * anchor. A candidate that would move the implied trusted estimate backwards
 * (at the candidate's own monotonic instant) is rejected with
 * TIME_ERR_TRUST_REGRESSION; a monotonic value older than the current
 * anchor's is rejected with TIME_ERR_MONOTONIC_REGRESSION. When `current` is
 * NULL or not valid this degrades to pool_time_validate_anchor().
 */
PoolTimeError pool_time_validate_reanchor(const PoolTimeAnchor *current,
                                          const PoolTimeAnchor *candidate,
                                          const PoolTimeTrustPolicy *policy);

/*
 * THE exact trusted-time predicate (Phase 2M.1B B2 Stage 5). TIME_OK iff ALL:
 *   1. an SNTP synchronization completed this boot (anchor valid + flag);
 *   2. the anchor epoch lies within [min_epoch_s .. max_epoch_s];
 *   3. monotonic_now_us >= anchor monotonic;
 *   4. anchor + elapsed arithmetic does not overflow;
 *   5. the derived epoch does not exceed max_epoch_s;
 *   6. when required_min_epoch_s > 0, derived epoch >= required_min_epoch_s.
 * On TIME_OK, *out_trusted_utc_us receives anchor_epoch + elapsed (µs).
 * Never reads any platform clock; pure in all inputs.
 */
PoolTimeError pool_time_evaluate_trust(const PoolTimeAnchor *anchor,
                                       uint64_t monotonic_now_us,
                                       const PoolTimeTrustPolicy *policy,
                                       uint64_t *out_trusted_utc_us);

/*
 * Compose one coherent snapshot from an abstract clock. *out is always fully
 * written (zeroed on failure paths). The returned code equals out->status.
 * A NULL/malformed clock yields an untrusted snapshot with
 * TIME_ERR_NOT_INITIALIZED — it never crashes and never guesses.
 */
PoolTimeError pool_time_snapshot(const PoolTimeClock *clock,
                                 const PoolTimeTrustPolicy *policy,
                                 PoolTimeSnapshot *out);

/*
 * Arm a boot-scoped monotonic deadline: monotonic_now_us + duration_s.
 * Duration must lie in [POOL_TIME_MIN_DURATION_S .. POOL_TIME_MAX_DURATION_S];
 * seconds-to-microseconds conversion and the addition are overflow-checked.
 * Suitable for the future FSM intent ARM_MONOTONIC_DEADLINE (not executed
 * or connected in B2).
 */
PoolTimeError pool_time_arm_monotonic_deadline(uint64_t monotonic_now_us,
                                               uint32_t duration_s,
                                               PoolTimeMonotonicDeadline *out);

/*
 * Evaluate a monotonic deadline. Expired exactly when now >= deadline.
 * remaining_s is the ceiling of remaining_us (1 µs remaining reports 1 s;
 * 0 only when expired). A monotonic regression (now earlier than the arming
 * instant) FAILS SAFE: status TIME_ERR_MONOTONIC_REGRESSION with
 * expired=true, so callers drive toward restore, never toward extra target
 * time.
 */
PoolTimeError pool_time_check_monotonic_deadline(const PoolTimeMonotonicDeadline *d,
                                                 uint64_t monotonic_now_us,
                                                 PoolTimeDeadlineCheck *out);

/*
 * Create a cross-reboot UTC deadline from a trusted snapshot:
 * deadline_epoch_s = trusted_epoch_s + duration_s (overflow-checked).
 *
 * TRUST CONTRACT (read carefully):
 *  - This helper accepts ONLY snapshots returned by pool_time_snapshot()
 *    (operational provenance — an API contract, not a cryptographic one).
 *  - It REVALIDATES every field available to it against `policy`:
 *    trusted flag, TIME_OK status, sync generation >= 1, epoch sanity band,
 *    required-minimum epoch, second/microsecond field agreement,
 *    anchor-age-vs-uptime consistency, duration bounds and overflow.
 *  - This is CONSISTENCY CHECKING and misuse resistance. It is NOT
 *    authentication: a fully self-consistent, caller-fabricated snapshot
 *    cannot be detected by pure code (no MAC/signature/capability exists).
 *    Callers that hold a provider should prefer
 *    pool_time_create_utc_deadline_from_clock(), which reads the
 *    provider-owned accepted anchor itself.
 */
PoolTimeError pool_time_create_utc_deadline(const PoolTimeSnapshot *snap,
                                            const PoolTimeTrustPolicy *policy,
                                            uint32_t duration_s,
                                            PoolTimeUtcDeadline *out);

/*
 * Preferred misuse-resistant path: take the snapshot directly from the
 * provider-owned anchor via the abstract clock (no caller-supplied snapshot
 * at all), then create the UTC deadline from it under the same contract.
 * On success *out_snap (optional, may be NULL) receives the snapshot used.
 */
PoolTimeError pool_time_create_utc_deadline_from_clock(const PoolTimeClock *clock,
                                                       const PoolTimeTrustPolicy *policy,
                                                       uint32_t duration_s,
                                                       PoolTimeSnapshot *out_snap,
                                                       PoolTimeUtcDeadline *out);

/*
 * The pure reboot/recovery time decision (Phase 2M.1B B2 Stage 10).
 * Decides only the TIME layer:
 *   - no valid source snapshot            -> RECOVERY_REQUIRED
 *   - no valid persisted UTC deadline     -> FAIL_SAFE_RESTORE (no waiting)
 *   - trusted and now >= deadline         -> RESTORE_DUE
 *   - trusted and now <  deadline         -> RESUME_TARGET_WITH_REMAINING_TIME
 *   - untrusted, window not yet elapsed   -> WAIT_FOR_TRUSTED_TIME
 *   - untrusted, window elapsed           -> FAIL_SAFE_RESTORE
 *   - trusted but earlier than persisted verified-start -> treated as
 *     untrusted (bounded wait, then FAIL_SAFE_RESTORE)
 *   - corrupt/inconsistent timing input   -> FAIL_SAFE_RESTORE when the source
 *     snapshot is trustworthy, RECOVERY_REQUIRED otherwise; never resume.
 * Executes nothing; mutates nothing; models no pool state.
 */
PoolTimeRecoveryResult pool_time_decide_recovery(const PoolTimeRecoveryInput *in,
                                                 const PoolTimeTrustPolicy *policy);

/* Stable machine tokens (diagnostics; dot-free; never carry data). */
const char *pool_time_error_str(PoolTimeError e);
const char *pool_time_decision_str(PoolTimeRecoveryDecision d);
const char *pool_time_sync_status_str(PoolTimeSyncStatus s);

/* ------------------------------------------------------------------ */
/* Compile-time guards                                                 */
/* ------------------------------------------------------------------ */
_Static_assert(POOL_TIME_EPOCH_MIN_S == 1735689600ull,
               "sanity floor must be 2025-01-01T00:00:00Z");
_Static_assert(POOL_TIME_EPOCH_MAX_S == 4102444800ull,
               "sanity ceiling must be 2100-01-01T00:00:00Z");
_Static_assert(POOL_TIME_EPOCH_MIN_S < POOL_TIME_EPOCH_MAX_S,
               "epoch sanity band inverted");
_Static_assert(POOL_TIME_MIN_DURATION_S < POOL_TIME_MAX_DURATION_S,
               "duration bounds inverted");
_Static_assert(POOL_TIME_MAX_DURATION_S == 86400u, "max duration must be 24h");
/* The seconds-to-microseconds conversion of any valid duration is statically
 * overflow-free; arming then only needs the runtime addition guard. */
_Static_assert((unsigned long long)POOL_TIME_MAX_DURATION_S
                   <= 0xFFFFFFFFFFFFFFFFull / POOL_TIME_US_PER_S,
               "duration-to-microseconds conversion could overflow");
_Static_assert(POOL_TIME_SYNC_WAIT_DEFAULT_S <= POOL_TIME_SYNC_WAIT_MAX_S,
               "sync window default exceeds hard maximum");
_Static_assert(POOL_TIME_SYNC_WAIT_MAX_S == 900u,
               "sync window hard maximum must be 15 minutes");
_Static_assert(POOL_TIME_ERR__COUNT == 21, "error count changed — review tokens/tests");
_Static_assert(POOL_TIME_DECISION__COUNT == 5, "decision count changed — review tests");

#endif /* POOL_TIME_H_ */
