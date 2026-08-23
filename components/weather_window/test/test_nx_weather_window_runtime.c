/*
 * Gate W6.4 — durable claim / crash-injection tests for the persistence owner.
 *
 * FAKE STORAGE ONLY. The backend below is an in-memory model of the committed
 * W2 contract with per-call failure injection, so a power loss can be placed
 * at any step of the dual-slot commit sequence. No physical NVS, no flash, no
 * hardware, no network, no clock is touched anywhere in this file.
 *
 * "Reboot" is modelled the only way it can be modelled honestly: the COMMITTED
 * bytes survive, every staged-but-uncommitted byte is discarded, and all RAM
 * state is thrown away and rebuilt from those bytes.
 *
 * GATE W6.4.1. The durable work no longer runs on the caller's task: it runs on
 * the project's internal-RAM NVS owner. These tests therefore drive TWO sides —
 * a request side that may only ask, and an executor the test runs explicitly.
 * That is what makes a crash BETWEEN the request and its execution testable at
 * all, which the previous synchronous design could not represent.
 */

#include <string.h>

#include "unity.h"

#include "nx_weather_window.h"
#include "nx_weather_window_runtime.h"
#include "tuning_store.h"
#include "tuning_record.h"

/* ================================================================== */
/* Fake W2 backend with commit-stage failure injection                 */
/* ================================================================== */

#define FKW_KEYS 3
#define FKW_A 0
#define FKW_B 1
#define FKW_P 2

typedef struct {
    bool    present;
    size_t  len;
    uint8_t bytes[TUNING_RECORD_MAX_ENCODED];
} FkwVal;

static struct {
    FkwVal committed[FKW_KEYS];   /* survives a simulated power loss     */
    FkwVal staged[FKW_KEYS];      /* lost on power loss until commit()   */
    bool   open;
    int    fail_write_after;      /* -1 never; 0 fail next call          */
    int    fail_commit_after;
    int    fail_read_after;
    int    reads;
    int    writes;
    int    commits;
    int    last_slot;   /* FKW_A/FKW_B most recently staged */
} g_fkw;

static int fkw_index(const char *key)
{
    if (strcmp(key, TUNING_STORE_KEY_SLOT_A) == 0) { return FKW_A; }
    if (strcmp(key, TUNING_STORE_KEY_SLOT_B) == 0) { return FKW_B; }
    if (strcmp(key, TUNING_STORE_KEY_ACTIVE) == 0) { return FKW_P; }
    return -1;
}

static bool fkw_should_fail(int *counter)
{
    if (*counter < 0) { return false; }
    if (*counter == 0) { *counter = -1; return true; }
    (*counter)--;
    return false;
}

static int fkw_open(void *ctx) { (void)ctx; g_fkw.open = true; return TUNING_STORE_BACKEND_OK; }
static int fkw_close(void *ctx) { (void)ctx; g_fkw.open = false; return TUNING_STORE_BACKEND_OK; }

static int fkw_read(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *out_len)
{
    int i = fkw_index(key);
    (void)ctx;
    if (i < 0) { return TUNING_STORE_BACKEND_NOT_FOUND; }
    g_fkw.reads++;
    if (fkw_should_fail(&g_fkw.fail_read_after)) { return TUNING_STORE_BACKEND_IO; }
    /* A reader sees staged-then-committed, exactly as NVS does within a
     * session; only a simulated power loss discards the staged copy. */
    {
        const FkwVal *v = g_fkw.staged[i].present ? &g_fkw.staged[i]
                                                  : &g_fkw.committed[i];
        if (!v->present) { return TUNING_STORE_BACKEND_NOT_FOUND; }
        *out_len = v->len;
        if (v->len <= cap) { memcpy(buf, v->bytes, v->len); }
        return TUNING_STORE_BACKEND_OK;
    }
}

static int fkw_write(void *ctx, const char *key, const uint8_t *buf, size_t len)
{
    int i = fkw_index(key);
    (void)ctx;
    if (i < 0 || len > TUNING_RECORD_MAX_ENCODED) { return TUNING_STORE_BACKEND_IO; }
    g_fkw.writes++;
    if (fkw_should_fail(&g_fkw.fail_write_after)) { return TUNING_STORE_BACKEND_IO; }
    if (i == FKW_A || i == FKW_B) { g_fkw.last_slot = i; }
    g_fkw.staged[i].present = true;
    g_fkw.staged[i].len = len;
    memcpy(g_fkw.staged[i].bytes, buf, len);
    return TUNING_STORE_BACKEND_OK;
}

static int fkw_commit(void *ctx)
{
    int i;
    (void)ctx;
    g_fkw.commits++;
    if (fkw_should_fail(&g_fkw.fail_commit_after)) { return TUNING_STORE_BACKEND_IO; }
    for (i = 0; i < FKW_KEYS; i++) {
        if (g_fkw.staged[i].present) {
            g_fkw.committed[i] = g_fkw.staged[i];
            g_fkw.staged[i].present = false;
        }
    }
    return TUNING_STORE_BACKEND_OK;
}

static const TuningStoreBackendOps FKW_OPS = {
    .open = fkw_open, .read_blob = fkw_read, .write_blob = fkw_write,
    .commit = fkw_commit, .close = fkw_close,
};

static void fkw_bind_executor(void);

static void fkw_reset(void)
{
    memset(&g_fkw, 0, sizeof(g_fkw));
    g_fkw.fail_write_after = -1;
    g_fkw.fail_commit_after = -1;
    g_fkw.fail_read_after = -1;
    nx_weather_window_runtime_reset_for_test();
    fkw_bind_executor();
}

/* Discard everything a power loss would discard, then rebuild RAM state. */
static void fkw_power_loss(void)
{
    int i;
    for (i = 0; i < FKW_KEYS; i++) { g_fkw.staged[i].present = false; }
    g_fkw.fail_write_after = -1;
    g_fkw.fail_commit_after = -1;
    g_fkw.fail_read_after = -1;
    nx_weather_window_runtime_reset_for_test();
    fkw_bind_executor();
}

/* ================================================================== */
/* The FAKE PERSISTENCE OWNER (Gate W6.4.1)                            */
/* ================================================================== */

/*
 * In production the wake lands on nvs_task, whose stack is in internal RAM,
 * and that task calls nx_weather_window_runtime_execute().
 *
 * Here the wake is only COUNTED. The test then decides when — or whether — to
 * run the executor. That separation is the entire point: it is what lets a
 * crash be placed BETWEEN the request and its execution, which is a state the
 * old synchronous design could not even represent.
 */
static int  g_wakes;
static int  g_wake_refusals;
static bool g_wake_accepts = true;

static bool fkw_wake(void *ctx)
{
    (void)ctx;
    if (!g_wake_accepts) {
        g_wake_refusals++;
        return false;                 /* owner queue full: ADMISSION failure */
    }
    g_wakes++;
    return true;
}

static const NxWeatherWindowExecutorOps FKW_EXEC = { .wake = fkw_wake };

static void fkw_bind_executor(void)
{
    g_wakes = 0;
    g_wake_refusals = 0;
    g_wake_accepts = true;
    nx_weather_window_runtime_bind_executor(&FKW_EXEC, NULL);
}

/* Run the persistence owner exactly once, as nvs_task would on a wake. */
static void owner_run(void)
{
    nx_weather_window_runtime_execute();
}

/* ================================================================== */
/* Fixtures                                                            */
/* ================================================================== */

#define W64R_SLOTS 3u

static WeatherLocalDate mkday(uint16_t y, uint8_t m, uint8_t d)
{
    WeatherLocalDate x; x.year = y; x.month = m; x.day = d; return x;
}

static WeatherSchedulePlan plan_due(WeatherLocalDate day, int8_t slot)
{
    WeatherSchedulePlan p;
    memset(&p, 0, sizeof(p));
    p.decision = WEATHER_SCHEDULE_DUE;
    p.slot_index = slot;
    p.reason = WEATHER_SCHED_REASON_ON_TIME;
    weather_schedule_progress_init(&p.proposed_progress, &day);
    p.proposed_progress.executed_mask = (uint8_t)(1u << (uint8_t)slot);
    p.proposal_present = true;
    return p;
}

/* Bring the store up: request the load, then let the owner perform it. */
static bool store_up(void)
{
    (void)nx_weather_window_runtime_begin(&FKW_OPS, &g_fkw);
    owner_run();
    return nx_weather_window_runtime_begin(&FKW_OPS, &g_fkw);
}

/*
 * The production sequence, in the production order, ACROSS TICKS:
 *   tick 1  decide -> request the claim            -> WAIT
 *   owner   durable dual-slot transaction
 *   tick 2  consume the completion                 -> SUBMIT (once)
 *
 * Returns true only when an outbound submission became authorized, and hands
 * back the grant that names the window that was actually claimed.
 */
static bool service_window(WeatherLocalDate day, int8_t slot,
                           NxWeatherWindowGrant *out_grant)
{
    WeatherSchedulePlan  p = plan_due(day, slot);
    NxWeatherWindowGrant g;
    NxWeatherWindowGate  v;

    v = nx_weather_window_authorize(&p, W64R_SLOTS, &g);
    if (v != NX_WX_GATE_WAIT) {
        if (out_grant != NULL) { *out_grant = g; }
        return v == NX_WX_GATE_SUBMIT;
    }
    owner_run();
    v = nx_weather_window_authorize(&p, W64R_SLOTS, &g);
    if (out_grant != NULL) { *out_grant = g; }
    return v == NX_WX_GATE_SUBMIT;
}

/* ================================================================== */
/* A — store lifecycle classification                                  */
/* ================================================================== */

TEST_CASE("w64r: a virgin store is serviceable and is not corruption",
          "[weather_window_rt]")
{
    TuningScheduleWindowState w;

    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_EQUAL(NX_WX_WSTORE_READY_EMPTY, nx_weather_window_runtime_fact());
    TEST_ASSERT_TRUE(nx_weather_window_runtime_ready());
    TEST_ASSERT_TRUE(nx_weather_window_runtime_state(&w));
    TEST_ASSERT_FALSE(w.present);
    /* Loading alone must not write anything. */
    TEST_ASSERT_EQUAL_INT(0, g_fkw.writes);
}

TEST_CASE("w64r: the load is REQUESTED, never performed by the caller",
          "[weather_window_rt]")
{
    TuningScheduleWindowState w;

    fkw_reset();

    /*
     * THE GATE W6.4.1 PROPERTY, stated as a test. Before the owner has run,
     * begin() must have touched no flash at all and must NOT claim the store
     * is usable — an unknown authority is not an empty one.
     */
    TEST_ASSERT_FALSE(nx_weather_window_runtime_begin(&FKW_OPS, &g_fkw));
    TEST_ASSERT_EQUAL(NX_WX_WSTORE_NOT_LOADED, nx_weather_window_runtime_fact());
    TEST_ASSERT_FALSE(nx_weather_window_runtime_ready());
    TEST_ASSERT_FALSE(nx_weather_window_runtime_state(&w));
    TEST_ASSERT_EQUAL_INT(0, g_fkw.reads);
    TEST_ASSERT_EQUAL_INT(0, g_fkw.writes);
    TEST_ASSERT_EQUAL_INT(1, g_wakes);

    /* Only the owner touches flash. */
    owner_run();
    TEST_ASSERT_TRUE(g_fkw.reads > 0);
    TEST_ASSERT_TRUE(nx_weather_window_runtime_ready());
}

TEST_CASE("w64r: repeated ticks request the load exactly once",
          "[weather_window_rt]")
{
    int i;

    fkw_reset();
    for (i = 0; i < 20; i++) {
        (void)nx_weather_window_runtime_begin(&FKW_OPS, &g_fkw);
    }
    TEST_ASSERT_EQUAL_INT(1, g_wakes);
}

TEST_CASE("w64r: an unreadable store fails closed and permits no service",
          "[weather_window_rt]")
{
    NxWeatherWindowGrant g;

    fkw_reset();
    g_fkw.fail_read_after = 0;
    (void)nx_weather_window_runtime_begin(&FKW_OPS, &g_fkw);
    owner_run();

    TEST_ASSERT_EQUAL(NX_WX_WSTORE_UNAVAILABLE, nx_weather_window_runtime_fact());
    TEST_ASSERT_FALSE(nx_weather_window_runtime_ready());
    TEST_ASSERT_FALSE(service_window(mkday(2026, 7, 15), 1, &g));
    TEST_ASSERT_EQUAL_INT(0, g_fkw.writes);
}

TEST_CASE("w64r: a broken store stops being retried after a bounded budget",
          "[weather_window_rt]")
{
    int i;

    /*
     * WITHOUT THIS BOUND the pilot would queue a flash transaction to the
     * shared owner queue once per second, forever, ahead of real configuration
     * writes, on any device whose dedup authority is simply broken.
     */
    fkw_reset();
    for (i = 0; i < 40; i++) {
        g_fkw.fail_read_after = 0;
        (void)nx_weather_window_runtime_begin(&FKW_OPS, &g_fkw);
        owner_run();
    }
    TEST_ASSERT_EQUAL(NX_WX_WSTORE_INHIBITED, nx_weather_window_runtime_fact());
    TEST_ASSERT_FALSE(nx_weather_window_runtime_ready());
    TEST_ASSERT_TRUE(g_wakes <= 4);
}

TEST_CASE("w64r: both slots corrupt fails closed and erases nothing",
          "[weather_window_rt]")
{
    NxWeatherWindowGrant g;

    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_TRUE(service_window(mkday(2026, 7, 15), 0, &g));

    g_fkw.committed[FKW_A].bytes[0] ^= 0xFFu;
    g_fkw.committed[FKW_B].bytes[0] ^= 0xFFu;

    fkw_power_loss();
    (void)nx_weather_window_runtime_begin(&FKW_OPS, &g_fkw);
    owner_run();

    TEST_ASSERT_EQUAL(NX_WX_WSTORE_UNAVAILABLE, nx_weather_window_runtime_fact());
    TEST_ASSERT_FALSE(service_window(mkday(2026, 7, 15), 1, &g));
    /* Nothing was erased in an attempt to "repair". */
    TEST_ASSERT_TRUE(g_fkw.committed[FKW_P].present);
}

TEST_CASE("w64r: ONE corrupt slot recovers the other and stays suppressed",
          "[weather_window_rt]")
{
    NxWeatherWindowGrant g;
    TuningScheduleWindowState w;
    uint8_t                   inactive;

    /*
     * The whole reason W2 keeps two slots. One damaged copy must cost nothing:
     * the surviving committed record is still the authority, the window it
     * recorded is still spent, and no erase or rewrite is attempted.
     */
    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    /* TWO claims, so the dual-slot algorithm has populated BOTH slots and the
     * pointer names the newer one. */
    TEST_ASSERT_TRUE(service_window(mkday(2026, 7, 15), 0, &g));
    TEST_ASSERT_TRUE(service_window(mkday(2026, 7, 15), 1, &g));
    TEST_ASSERT_TRUE(nx_weather_window_runtime_state(&w));
    TEST_ASSERT_EQUAL_UINT8(0x03u, w.served_mask);
    TEST_ASSERT_TRUE(g_fkw.committed[FKW_A].present);
    TEST_ASSERT_TRUE(g_fkw.committed[FKW_B].present);

    /* Damage the OLDER slot — the one the pointer does not name. */
    inactive = (g_fkw.last_slot == FKW_A) ? (uint8_t)FKW_B : (uint8_t)FKW_A;
    g_fkw.committed[inactive].bytes[3] ^= 0xFFu;

    fkw_power_loss();
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_EQUAL(NX_WX_WSTORE_READY_RECOVERED,
                      nx_weather_window_runtime_fact());
    TEST_ASSERT_TRUE(nx_weather_window_runtime_state(&w));
    TEST_ASSERT_TRUE(w.present);
    TEST_ASSERT_EQUAL_UINT8(0x03u, w.served_mask);

    /* The recovered windows are still spent. */
    TEST_ASSERT_FALSE(service_window(mkday(2026, 7, 15), 0, &g));
    TEST_ASSERT_FALSE(service_window(mkday(2026, 7, 15), 1, &g));
}

/* ================================================================== */
/* B — the durable claim, across ticks                                 */
/* ================================================================== */

TEST_CASE("w64r: a due window is durably claimed before it may be submitted",
          "[weather_window_rt]")
{
    WeatherSchedulePlan  p = plan_due(mkday(2026, 7, 15), 1);
    NxWeatherWindowGrant g;
    TuningScheduleWindowState w;

    fkw_reset();
    TEST_ASSERT_TRUE(store_up());

    /* Tick 1: the claim is only REQUESTED. Nothing is authorized yet. */
    TEST_ASSERT_EQUAL(NX_WX_GATE_WAIT,
                      nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    TEST_ASSERT_EQUAL_UINT32(0u, g.generation);
    TEST_ASSERT_EQUAL_INT(0, g_fkw.writes);   /* the caller wrote nothing */

    /* The owner performs the transaction. */
    owner_run();
    TEST_ASSERT_TRUE(g_fkw.writes > 0);

    /* Tick 2: and only now is exactly one submission authorized. */
    TEST_ASSERT_EQUAL(NX_WX_GATE_SUBMIT,
                      nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    TEST_ASSERT_TRUE(g.generation != 0u);
    TEST_ASSERT_EQUAL_UINT16(2026u, g.date.year);
    TEST_ASSERT_EQUAL_UINT8(15u, g.date.day);
    TEST_ASSERT_EQUAL_INT(1, g.slot_index);
    TEST_ASSERT_EQUAL_UINT8(0x02u, g.served_mask);

    TEST_ASSERT_TRUE(nx_weather_window_runtime_state(&w));
    TEST_ASSERT_EQUAL_UINT8(0x02u, w.served_mask);
}

TEST_CASE("w64r: the grant is spent exactly once",
          "[weather_window_rt]")
{
    WeatherSchedulePlan  p = plan_due(mkday(2026, 7, 15), 1);
    NxWeatherWindowGrant g;
    int                  i;

    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_EQUAL(NX_WX_GATE_WAIT,
                      nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    owner_run();
    TEST_ASSERT_EQUAL(NX_WX_GATE_SUBMIT,
                      nx_weather_window_authorize(&p, W64R_SLOTS, &g));

    /* Every later tick for the same window is a refusal, forever. */
    for (i = 0; i < 50; i++) {
        TEST_ASSERT_EQUAL(NX_WX_GATE_REFUSE,
                          nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    }
}

TEST_CASE("w64r: a duplicate tick cannot enqueue a duplicate job",
          "[weather_window_rt]")
{
    WeatherSchedulePlan  p = plan_due(mkday(2026, 7, 15), 1);
    NxWeatherWindowGrant g;
    int                  i;

    fkw_reset();
    TEST_ASSERT_TRUE(store_up());

    /* Twenty ticks while the transaction is in flight: ONE job, ONE wake. */
    g_wakes = 0;                          /* discount the load wake */
    for (i = 0; i < 20; i++) {
        TEST_ASSERT_EQUAL(NX_WX_GATE_WAIT,
                          nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    }
    TEST_ASSERT_EQUAL_INT(1, g_wakes);

    owner_run();
    TEST_ASSERT_EQUAL(NX_WX_GATE_SUBMIT,
                      nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    /* Exactly one durable transaction happened. */
    TEST_ASSERT_EQUAL_INT(2, g_fkw.commits);
}

TEST_CASE("w64r: thousands of ticks cost at most one claim per slot",
          "[weather_window_rt]")
{
    NxWeatherWindowGrant g;
    WeatherLocalDate     day = mkday(2026, 7, 15);
    int                  i;
    int                  submits = 0;

    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    for (i = 0; i < 3000; i++) {
        int8_t slot = (int8_t)(i % 3);
        WeatherSchedulePlan p = plan_due(day, slot);
        if (nx_weather_window_authorize(&p, W64R_SLOTS, &g) == NX_WX_GATE_WAIT) {
            owner_run();
            if (nx_weather_window_authorize(&p, W64R_SLOTS, &g) ==
                NX_WX_GATE_SUBMIT) {
                submits++;
            }
        }
    }
    TEST_ASSERT_EQUAL_INT(3, submits);
    TEST_ASSERT_EQUAL_INT(6, g_fkw.commits);   /* 2 commits per claim */
}

/* ================================================================== */
/* C — crash points A..F (Gate W6.4.1)                                 */
/* ================================================================== */

TEST_CASE("w64r: CRASH A — before the request, nothing is claimed",
          "[weather_window_rt]")
{
    NxWeatherWindowGrant g;

    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    /* Power lost before any tick even decided. */
    fkw_power_loss();
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_EQUAL(NX_WX_WSTORE_READY_EMPTY,
                      nx_weather_window_runtime_fact());
    /* The window is still open, exactly as it should be. */
    TEST_ASSERT_TRUE(service_window(mkday(2026, 7, 15), 1, &g));
}

TEST_CASE("w64r: CRASH B — a queued job that never ran leaves no claim",
          "[weather_window_rt]")
{
    WeatherSchedulePlan  p = plan_due(mkday(2026, 7, 15), 1);
    NxWeatherWindowGrant g;

    /*
     * A state the OLD synchronous design could not even represent: the request
     * exists, the owner has not run it, and power is lost. Recovery must see
     * NO durable claim — and must not invent one from the fact a request was
     * made. Never infer durability from the attempt.
     */
    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_EQUAL(NX_WX_GATE_WAIT,
                      nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    TEST_ASSERT_EQUAL_INT(0, g_fkw.writes);

    fkw_power_loss();                       /* the job dies with the RAM */
    TEST_ASSERT_TRUE(store_up());
    {
        TuningScheduleWindowState w;
        TEST_ASSERT_TRUE(nx_weather_window_runtime_state(&w));
        TEST_ASSERT_FALSE(w.present);       /* no false served state */
    }
    /* And the window may still be served exactly once. */
    TEST_ASSERT_TRUE(service_window(mkday(2026, 7, 15), 1, &g));
}

TEST_CASE("w64r: CRASH C — power loss at EVERY commit stage never duplicates",
          "[weather_window_rt]")
{
    int stage;

    for (stage = 0; stage < 6; stage++) {
        NxWeatherWindowGrant g;
        WeatherSchedulePlan  p = plan_due(mkday(2026, 7, 15), 1);
        bool                 served_before;
        bool                 served_after;

        fkw_reset();
        TEST_ASSERT_TRUE(store_up());

        /* Fail the transaction at successively later stages. */
        g_fkw.fail_write_after = stage;
        g_fkw.fail_commit_after = stage;

        served_before = false;
        if (nx_weather_window_authorize(&p, W64R_SLOTS, &g) == NX_WX_GATE_WAIT) {
            owner_run();
            served_before = (nx_weather_window_authorize(&p, W64R_SLOTS, &g) ==
                             NX_WX_GATE_SUBMIT);
        }

        fkw_power_loss();
        /*
         * store_up() may legitimately FAIL here. An interrupted first commit
         * against a virgin store can leave a slot with no pointer naming it,
         * and W2's answer to that is to refuse rather than guess. That is the
         * correct fail-closed behaviour, not a defect, so the assertion below
         * is the property this test actually owns — never the serviceability.
         */
        (void)store_up();
        served_after = service_window(mkday(2026, 7, 15), 1, &g);

        /* The window may be served before OR after, but NEVER both. */
        TEST_ASSERT_FALSE_MESSAGE(served_before && served_after,
                                  "a window was served twice across a crash");
    }
}

TEST_CASE("w64r: CRASH D — durable commit, completion lost, still suppressed",
          "[weather_window_rt]")
{
    WeatherSchedulePlan  p = plan_due(mkday(2026, 7, 15), 1);
    NxWeatherWindowGrant g;

    /*
     * The owner committed. Power is lost before the observation task ever
     * consumed the completion, so no submission happened and the RAM credit is
     * gone. Recovery must find the claim and suppress the window: one optional
     * recommendation is lost, which is the accepted at-most-once cost.
     */
    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_EQUAL(NX_WX_GATE_WAIT,
                      nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    owner_run();                            /* durable */
    TEST_ASSERT_TRUE(g_fkw.commits >= 2);

    fkw_power_loss();                       /* completion never consumed */
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_EQUAL(NX_WX_WSTORE_READY_RECOVERED,
                      nx_weather_window_runtime_fact());
    TEST_ASSERT_FALSE(service_window(mkday(2026, 7, 15), 1, &g));
}

TEST_CASE("w64r: CRASH E — completion consumed, crash before submit, suppressed",
          "[weather_window_rt]")
{
    WeatherSchedulePlan  p = plan_due(mkday(2026, 7, 15), 1);
    NxWeatherWindowGrant g;

    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_EQUAL(NX_WX_GATE_WAIT,
                      nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    owner_run();
    /* The grant is issued — the caller was authorized — and then power dies
     * before it composed or sent anything. */
    TEST_ASSERT_EQUAL(NX_WX_GATE_SUBMIT,
                      nx_weather_window_authorize(&p, W64R_SLOTS, &g));

    fkw_power_loss();
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_FALSE(service_window(mkday(2026, 7, 15), 1, &g));
}

TEST_CASE("w64r: CRASH F — crash after submit before result, no duplicate",
          "[weather_window_rt]")
{
    WeatherSchedulePlan  p = plan_due(mkday(2026, 7, 15), 1);
    NxWeatherWindowGrant g;

    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_EQUAL(NX_WX_GATE_WAIT,
                      nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    owner_run();
    TEST_ASSERT_EQUAL(NX_WX_GATE_SUBMIT,
                      nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    /* (the caller submits here; the result never arrives) */

    fkw_power_loss();
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_FALSE(service_window(mkday(2026, 7, 15), 1, &g));
}

/* ================================================================== */
/* D — transaction failure and the fail-passive latch                  */
/* ================================================================== */

TEST_CASE("w64r: a persistence failure permits ZERO outbound submission",
          "[weather_window_rt]")
{
    WeatherSchedulePlan  p = plan_due(mkday(2026, 7, 15), 1);
    NxWeatherWindowGrant g;

    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    g_fkw.fail_write_after = 0;

    TEST_ASSERT_EQUAL(NX_WX_GATE_WAIT,
                      nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    owner_run();
    TEST_ASSERT_EQUAL(NX_WX_GATE_REFUSE,
                      nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    TEST_ASSERT_EQUAL_UINT32(0u, g.generation);
}

TEST_CASE("w64r: a failed transaction is not retried at tick rate",
          "[weather_window_rt]")
{
    WeatherSchedulePlan  p = plan_due(mkday(2026, 7, 15), 1);
    NxWeatherWindowGrant g;
    int                  i;
    int                  commits_after_failure;

    /*
     * Fail passive. Retrying a failed durable claim once per second would hammer
     * flash and could, on an UNCERTAIN commit, race the very ambiguity only a
     * reboot can resolve.
     */
    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    g_fkw.fail_write_after = 0;
    g_wakes = 0;                          /* discount the load wake */
    TEST_ASSERT_EQUAL(NX_WX_GATE_WAIT,
                      nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    owner_run();
    (void)nx_weather_window_authorize(&p, W64R_SLOTS, &g);
    commits_after_failure = g_fkw.commits;

    for (i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL(NX_WX_GATE_REFUSE,
                          nx_weather_window_authorize(&p, W64R_SLOTS, &g));
        owner_run();
    }
    TEST_ASSERT_EQUAL_INT(commits_after_failure, g_fkw.commits);
    TEST_ASSERT_EQUAL_INT(1, g_wakes);
}

TEST_CASE("w64r: a reboot re-derives eligibility after a failed transaction",
          "[weather_window_rt]")
{
    WeatherSchedulePlan  p = plan_due(mkday(2026, 7, 15), 1);
    NxWeatherWindowGrant g;

    /* The latch is RAM-only, so W2 recovery — not a remembered verdict — is
     * the authority on the next boot. Nothing durable was written, so the
     * window is legitimately still open. */
    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    g_fkw.fail_write_after = 0;
    TEST_ASSERT_EQUAL(NX_WX_GATE_WAIT,
                      nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    owner_run();
    TEST_ASSERT_EQUAL(NX_WX_GATE_REFUSE,
                      nx_weather_window_authorize(&p, W64R_SLOTS, &g));

    fkw_power_loss();
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_TRUE(service_window(mkday(2026, 7, 15), 1, &g));
}

/* ================================================================== */
/* E — owner admission failure is NOT a persistence failure            */
/* ================================================================== */

TEST_CASE("w64r: a refused wake writes nothing and stays retryable",
          "[weather_window_rt]")
{
    WeatherSchedulePlan  p = plan_due(mkday(2026, 7, 15), 1);
    NxWeatherWindowGrant g;
    int                  i;

    fkw_reset();
    TEST_ASSERT_TRUE(store_up());

    /* The owner queue is full: every tick refuses, and nothing is claimed. */
    g_wake_accepts = false;
    for (i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL(NX_WX_GATE_REFUSE,
                          nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    }
    TEST_ASSERT_EQUAL_INT(0, g_fkw.writes);
    TEST_ASSERT_TRUE(g_wake_refusals >= 10);

    /* Admission recovers, and the window — still due — is served normally. */
    g_wake_accepts = true;
    TEST_ASSERT_TRUE(service_window(mkday(2026, 7, 15), 1, &g));
}

TEST_CASE("w64r: a refused wake never leaves a phantom pending job",
          "[weather_window_rt]")
{
    WeatherSchedulePlan p = plan_due(mkday(2026, 7, 15), 1);
    NxWeatherWindowGrant g;
    NxWeatherWindowDiag  d;

    /*
     * If the slot stayed reserved after a refused wake, the machine would be
     * wedged for the rest of the boot: every later tick would see a job
     * pending, return WAIT, and no completion would ever arrive.
     */
    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    g_wake_accepts = false;
    TEST_ASSERT_EQUAL(NX_WX_GATE_REFUSE,
                      nx_weather_window_authorize(&p, W64R_SLOTS, &g));

    nx_weather_window_runtime_observe(&d);
    TEST_ASSERT_EQUAL(NX_WX_WJOB_NONE, (NxWeatherWindowJobKind)d.job_pending);
    TEST_ASSERT_TRUE(d.enqueue_busy_count >= 1u);
}

/* ================================================================== */
/* F — worker refusal AFTER a durable claim                            */
/* ================================================================== */

TEST_CASE("w64r: a worker refusal after a durable claim cannot reopen it",
          "[weather_window_rt]")
{
    WeatherSchedulePlan  p = plan_due(mkday(2026, 7, 15), 1);
    NxWeatherWindowGrant g;
    int                  i;

    /*
     * The claim is durable and the caller is authorized — and then the W6.3
     * worker refuses the submission (busy, not started, in flight). The window
     * is SPENT anyway. One lost recommendation is acceptable; a duplicate
     * outbound service is not.
     */
    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_EQUAL(NX_WX_GATE_WAIT,
                      nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    owner_run();
    TEST_ASSERT_EQUAL(NX_WX_GATE_SUBMIT,
                      nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    /* ---- the caller's submission is refused downstream; nothing rolls back */

    for (i = 0; i < 20; i++) {
        TEST_ASSERT_EQUAL(NX_WX_GATE_REFUSE,
                          nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    }

    /* And it stays spent across a reboot. */
    fkw_power_loss();
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_FALSE(service_window(mkday(2026, 7, 15), 1, &g));
}

/* ================================================================== */
/* G — startup ordering, both permutations                             */
/* ================================================================== */

TEST_CASE("w64r: persistence ready BEFORE the first due window converges",
          "[weather_window_rt]")
{
    NxWeatherWindowGrant g;

    fkw_reset();
    TEST_ASSERT_TRUE(store_up());          /* authority first */
    TEST_ASSERT_TRUE(nx_weather_window_runtime_ready());
    TEST_ASSERT_TRUE(service_window(mkday(2026, 7, 15), 1, &g));
    TEST_ASSERT_EQUAL_INT(1, g.slot_index);
}

TEST_CASE("w64r: a due window BEFORE persistence is ready fetches nothing",
          "[weather_window_rt]")
{
    WeatherSchedulePlan  p = plan_due(mkday(2026, 7, 15), 1);
    NxWeatherWindowGrant g;
    int                  i;

    /*
     * The other permutation: trusted time and a due schedule arrive first,
     * while the store is still unknown. There must be NO race to fetch — the
     * window waits for the authority, and both orders converge on exactly one
     * submission.
     */
    fkw_reset();
    (void)nx_weather_window_runtime_begin(&FKW_OPS, &g_fkw);   /* load queued */

    for (i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL(NX_WX_GATE_REFUSE,
                          nx_weather_window_authorize(&p, W64R_SLOTS, &g));
    }
    TEST_ASSERT_EQUAL_INT(0, g_fkw.writes);

    owner_run();                                   /* authority arrives */
    TEST_ASSERT_TRUE(nx_weather_window_runtime_ready());
    TEST_ASSERT_TRUE(service_window(mkday(2026, 7, 15), 1, &g));
    TEST_ASSERT_EQUAL_INT(1, g.slot_index);
}

/* ================================================================== */
/* H — day handling and record preservation                            */
/* ================================================================== */

TEST_CASE("w64r: a later slot survives a reboot and remains eligible once",
          "[weather_window_rt]")
{
    NxWeatherWindowGrant g;
    TuningScheduleWindowState w;

    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_TRUE(service_window(mkday(2026, 7, 15), 0, &g));

    fkw_power_loss();
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_FALSE(service_window(mkday(2026, 7, 15), 0, &g));  /* spent */
    TEST_ASSERT_TRUE(service_window(mkday(2026, 7, 15), 2, &g));   /* fresh */

    TEST_ASSERT_TRUE(nx_weather_window_runtime_state(&w));
    TEST_ASSERT_EQUAL_UINT8(0x05u, w.served_mask);
}

TEST_CASE("w64r: a new service day is not suppressed by yesterday",
          "[weather_window_rt]")
{
    NxWeatherWindowGrant g;
    TuningScheduleWindowState w;

    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_TRUE(service_window(mkday(2026, 7, 15), 1, &g));
    TEST_ASSERT_TRUE(service_window(mkday(2026, 7, 16), 1, &g));

    TEST_ASSERT_TRUE(nx_weather_window_runtime_state(&w));
    TEST_ASSERT_EQUAL_UINT8(16u, w.day);
    TEST_ASSERT_EQUAL_UINT8(0x02u, w.served_mask);   /* the day replaced it */
}

TEST_CASE("w64r: a backward service day cannot reopen a spent window",
          "[weather_window_rt]")
{
    NxWeatherWindowGrant g;

    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_TRUE(service_window(mkday(2026, 7, 16), 1, &g));
    /* Trusted time moved backwards across a reboot. */
    TEST_ASSERT_FALSE(service_window(mkday(2026, 7, 15), 1, &g));
}

TEST_CASE("w64r: a claim preserves every other persisted policy field",
          "[weather_window_rt]")
{
    NxWeatherWindowGrant g;
    TuningStore          s;
    TuningPolicyRecord   rec;

    fkw_reset();
    memset(&rec, 0, sizeof(rec));
    tuning_record_init_state(&rec);
    rec.latest_trusted_epoch_s        = 1750000000ull;
    rec.consecutive_recovery_failures = 2u;
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_init(&s, &FKW_OPS, &g_fkw));
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&s, &rec));
    tuning_store_deinit(&s);

    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_TRUE(service_window(mkday(2026, 7, 15), 1, &g));

    memset(&rec, 0, sizeof(rec));
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_init(&s, &FKW_OPS, &g_fkw));
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_load(&s, &rec, NULL));
    tuning_store_deinit(&s);
    TEST_ASSERT_EQUAL_UINT64(1750000000ull, rec.latest_trusted_epoch_s);
    TEST_ASSERT_EQUAL_UINT8(2u, rec.consecutive_recovery_failures);
    TEST_ASSERT_TRUE(rec.window.present);
    TEST_ASSERT_EQUAL_UINT8(0x02u, rec.window.served_mask);
}

TEST_CASE("w64r: a pre-W6.4 record recovers and does NOT suppress a window",
          "[weather_window_rt]")
{
    NxWeatherWindowGrant g;
    TuningStore          s;
    TuningPolicyRecord   rec;

    fkw_reset();
    memset(&rec, 0, sizeof(rec));
    tuning_record_init_state(&rec);
    memset(&rec.window, 0, sizeof(rec.window));
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_init(&s, &FKW_OPS, &g_fkw));
    TEST_ASSERT_EQUAL(TUNING_STORE_OK, tuning_store_commit_record(&s, &rec));
    tuning_store_deinit(&s);

    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_EQUAL(NX_WX_WSTORE_READY_RECOVERED,
                      nx_weather_window_runtime_fact());
    TEST_ASSERT_TRUE(service_window(mkday(2026, 7, 15), 1, &g));
}

/* ================================================================== */
/* I — the executor is a no-op without work, and diagnostics are bounded */
/* ================================================================== */

TEST_CASE("w64r: a spurious wake performs no transaction",
          "[weather_window_rt]")
{
    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    {
        int w = g_fkw.writes;
        int c = g_fkw.commits;
        owner_run();
        owner_run();
        owner_run();
        TEST_ASSERT_EQUAL_INT(w, g_fkw.writes);
        TEST_ASSERT_EQUAL_INT(c, g_fkw.commits);
    }
}

TEST_CASE("w64r: diagnostics are bounded tokens and saturate",
          "[weather_window_rt]")
{
    NxWeatherWindowDiag d;
    NxWeatherWindowGrant g;

    fkw_reset();
    TEST_ASSERT_TRUE(store_up());
    TEST_ASSERT_TRUE(service_window(mkday(2026, 7, 15), 1, &g));

    nx_weather_window_runtime_observe(&d);
    TEST_ASSERT_EQUAL(NX_WX_WSTORE_READY_RECOVERED,
                      (NxWeatherWindowStoreFact)d.fact);
    TEST_ASSERT_TRUE(d.ready);
    TEST_ASSERT_TRUE(d.day_present);
    TEST_ASSERT_EQUAL_UINT8(0x02u, d.served_mask);
    TEST_ASSERT_EQUAL_UINT32(1u, d.claim_count);
    TEST_ASSERT_TRUE(d.executed_count >= 2u);   /* load + claim */
    TEST_ASSERT_EQUAL(NX_WX_WJOB_NONE, (NxWeatherWindowJobKind)d.job_pending);
    TEST_ASSERT_FALSE(d.submit_credit);         /* already spent */

    /* No pointer or buffer may exist in the observation. */
    TEST_ASSERT_TRUE(sizeof(d) < 128u);
}
