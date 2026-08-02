/*
 * Deterministic tests for the Gate B10.2 read-only store preflight: the pure
 * classifier, the read-only backend contract, the zero-mutation proofs, the
 * boot posture and the privacy properties.
 *
 * NOTHING here touches physical hardware, a real NVS partition, a serial
 * port, a network, a pool or a device. The backend is exercised through the
 * committed Gate B3 store with a synthetic in-memory NVS model, and every
 * identity in a fixture is a synthetic "*.example" value.
 */

#include <string.h>
#include "unity.h"
#include "pool_session_preflight.h"
#include "pool_session_store.h"
#include "pool_session_record.h"
#include "pool_session.h"

static NxTpsPreflightInput  g_in;
static NxTpsPreflightResult g_res;
static char                 g_line[256];

/* ================================================================= */
/* Helpers                                                            */
/* ================================================================= */

/* The healthy "nothing has ever been committed here" input. */
static void pf_input_empty(NxTpsPreflightInput *in)
{
    memset(in, 0, sizeof(*in));
    in->model_version     = NX_TPS_PREFLIGHT_MODEL_VERSION;
    in->namespace_present = true;
    in->store_loaded      = true;
    in->store_result      = (uint8_t)STORE_EMPTY;
}

static void pf_input_record(NxTpsPreflightInput *in, PoolSessionState st)
{
    memset(in, 0, sizeof(*in));
    in->model_version     = NX_TPS_PREFLIGHT_MODEL_VERSION;
    in->namespace_present = true;
    in->store_loaded      = true;
    in->store_result      = (uint8_t)STORE_OK;
    in->record_kind       = (uint8_t)POOL_RECORD_KIND_SESSION;
    in->session_state     = (uint8_t)st;
    in->record_valid      = true;
}

/* ================================================================= */
/* A. Pure classification                                             */
/* ================================================================= */

TEST_CASE("b102 cls: an absent namespace classifies EMPTY", "[pool_preflight]")
{
    pf_input_empty(&g_in);
    g_in.namespace_present = false;
    g_in.store_loaded      = false; /* the loader is never entered */

    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_EMPTY, g_res.outcome);
    TEST_ASSERT_TRUE(g_res.permits_pilot);
    TEST_ASSERT_FALSE(g_res.namespace_present);
}

TEST_CASE("b102 cls: an empty store classifies EMPTY", "[pool_preflight]")
{
    pf_input_empty(&g_in);
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_EMPTY, g_res.outcome);
    TEST_ASSERT_TRUE(g_res.permits_pilot);
}

TEST_CASE("b102 cls: a committed tombstone classifies CLEARED", "[pool_preflight]")
{
    pf_input_empty(&g_in);
    g_in.store_result = (uint8_t)STORE_CLEARED;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_CLEARED, g_res.outcome);
    TEST_ASSERT_TRUE(g_res.permits_pilot);

    /* Defence in depth: a TOMBSTONE kind reported alongside STORE_OK is also
     * CLEARED, so a future loader change cannot widen what counts as a live
     * session record. */
    pf_input_record(&g_in, POOL_STATE_IDLE);
    g_in.record_kind = (uint8_t)POOL_RECORD_KIND_TOMBSTONE;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_CLEARED, g_res.outcome);
}

TEST_CASE("b102 cls: a live session record blocks the pilot", "[pool_preflight]")
{
    const PoolSessionState live[] = {
        POOL_STATE_PREPARING, POOL_STATE_TARGET_SNAPSHOT_COMMITTED,
        POOL_STATE_APPLYING_TARGET, POOL_STATE_VERIFYING_TARGET,
        POOL_STATE_TARGET_ACTIVE, POOL_STATE_RESTORE_DUE,
        POOL_STATE_APPLYING_RESTORE, POOL_STATE_VERIFYING_RESTORE,
    };
    unsigned i;

    for (i = 0u; i < sizeof(live) / sizeof(live[0]); i++) {
        pf_input_record(&g_in, live[i]);
        nx_tps_preflight_classify(&g_in, &g_res);
        TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_RECORD_PRESENT, g_res.outcome);
        TEST_ASSERT_FALSE(g_res.permits_pilot);
    }
}

TEST_CASE("b102 cls: an unacknowledged terminal record classifies TERMINAL_PENDING",
          "[pool_preflight]")
{
    int found = 0;
    int i;

    for (i = 0; i < (int)POOL_STATE__COUNT; i++) {
        if (!pool_state_is_terminal((PoolSessionState)i)) {
            continue;
        }
        found++;
        pf_input_record(&g_in, (PoolSessionState)i);
        nx_tps_preflight_classify(&g_in, &g_res);
        TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_TERMINAL_PENDING, g_res.outcome);
        TEST_ASSERT_FALSE(g_res.permits_pilot);
    }
    TEST_ASSERT_TRUE(found > 0); /* the committed predicate really has terminals */
}

TEST_CASE("b102 cls: an unresolved restore obligation always blocks",
          "[pool_preflight]")
{
    /* Even on the two otherwise-permitting results. */
    pf_input_empty(&g_in);
    g_in.restore_required = true;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_FALSE(g_res.permits_pilot);

    pf_input_empty(&g_in);
    g_in.store_result     = (uint8_t)STORE_CLEARED;
    g_in.restore_required = true;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_FALSE(g_res.permits_pilot);
}

TEST_CASE("b102 cls: every untrustworthy store result classifies CORRUPT",
          "[pool_preflight]")
{
    const PoolStoreResult bad[] = {
        STORE_CORRUPT, STORE_INVALID_RECORD, STORE_ACTIVE_POINTER_INVALID,
        STORE_ACTIVE_SLOT_INVALID, STORE_RECOVERY_REQUIRED, STORE_READBACK_MISMATCH,
    };
    unsigned i;

    for (i = 0u; i < sizeof(bad) / sizeof(bad[0]); i++) {
        pf_input_empty(&g_in);
        g_in.store_result = (uint8_t)bad[i];
        nx_tps_preflight_classify(&g_in, &g_res);
        TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_CORRUPT, g_res.outcome);
        TEST_ASSERT_FALSE(g_res.permits_pilot);
    }
}

TEST_CASE("b102 cls: schema, uncertain and IO results map to their own outcomes",
          "[pool_preflight]")
{
    pf_input_empty(&g_in);
    g_in.store_result = (uint8_t)STORE_UNSUPPORTED_SCHEMA;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_UNSUPPORTED_SCHEMA, g_res.outcome);

    pf_input_empty(&g_in);
    g_in.store_result = (uint8_t)STORE_COMMIT_UNCERTAIN;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_COMMIT_UNCERTAIN, g_res.outcome);

    pf_input_empty(&g_in);
    g_in.store_result = (uint8_t)STORE_IO_ERROR;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_IO_ERROR, g_res.outcome);

    pf_input_empty(&g_in);
    g_in.store_result = (uint8_t)STORE_NOT_INITIALIZED;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_IO_ERROR, g_res.outcome);
}

TEST_CASE("b102 cls: a decoded-but-invalid record classifies CORRUPT",
          "[pool_preflight]")
{
    pf_input_record(&g_in, POOL_STATE_TARGET_ACTIVE);
    g_in.record_valid = false;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_CORRUPT, g_res.outcome);
}

TEST_CASE("b102 cls: an unknown record kind or state classifies CORRUPT",
          "[pool_preflight]")
{
    pf_input_record(&g_in, POOL_STATE_TARGET_ACTIVE);
    g_in.record_kind = 77;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_CORRUPT, g_res.outcome);

    pf_input_record(&g_in, POOL_STATE_TARGET_ACTIVE);
    g_in.session_state = 200;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_CORRUPT, g_res.outcome);
}

TEST_CASE("b102 cls: impossible store results fail closed", "[pool_preflight]")
{
    const PoolStoreResult impossible[] = {
        STORE_INVALID_ARGUMENT, STORE_GENERATION_EXHAUSTED, STORE_STATE_CONFLICT,
    };
    unsigned i;

    for (i = 0u; i < sizeof(impossible) / sizeof(impossible[0]); i++) {
        pf_input_empty(&g_in);
        g_in.store_result = (uint8_t)impossible[i];
        nx_tps_preflight_classify(&g_in, &g_res);
        TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_INTERNAL_ERROR, g_res.outcome);
        TEST_ASSERT_FALSE(g_res.permits_pilot);
    }

    /* Out of range entirely. */
    pf_input_empty(&g_in);
    g_in.store_result = 250;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_INTERNAL_ERROR, g_res.outcome);
}

TEST_CASE("b102 cls: a loader that never ran fails closed", "[pool_preflight]")
{
    pf_input_empty(&g_in);
    g_in.store_loaded = false; /* namespace present but nothing was read */
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_INTERNAL_ERROR, g_res.outcome);
    TEST_ASSERT_FALSE(g_res.permits_pilot);
}

TEST_CASE("b102 cls: NULL input and model mismatch fail closed", "[pool_preflight]")
{
    nx_tps_preflight_classify(NULL, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_INTERNAL_ERROR, g_res.outcome);
    TEST_ASSERT_FALSE(g_res.permits_pilot);

    pf_input_empty(&g_in);
    g_in.model_version = NX_TPS_PREFLIGHT_MODEL_VERSION + 1u;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_INTERNAL_ERROR, g_res.outcome);

    nx_tps_preflight_classify(&g_in, NULL); /* must not crash */
}

TEST_CASE("b102 cls: any attempted mutation fails the whole verdict closed",
          "[pool_preflight]")
{
    /* Even a perfectly empty store: if the preflight tried to write, its
     * reading is not trustworthy either. */
    pf_input_empty(&g_in);
    g_in.write_attempts = 1u;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_INTERNAL_ERROR, g_res.outcome);
    TEST_ASSERT_FALSE(g_res.permits_pilot);

    pf_input_empty(&g_in);
    g_in.erase_attempts = 1u;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_INTERNAL_ERROR, g_res.outcome);

    pf_input_empty(&g_in);
    g_in.commit_attempts = 1u;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_INTERNAL_ERROR, g_res.outcome);
}

TEST_CASE("b102 cls: a zeroed result blocks", "[pool_preflight]")
{
    memset(&g_res, 0, sizeof(g_res));
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_INTERNAL_ERROR, g_res.outcome);
    TEST_ASSERT_FALSE(nx_tps_preflight_permits_pilot(g_res.outcome));

    nx_tps_preflight_result_init(&g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_INTERNAL_ERROR, g_res.outcome);
    TEST_ASSERT_FALSE(g_res.permits_pilot);
    nx_tps_preflight_result_init(NULL); /* must not crash */
}

TEST_CASE("b102 cls: classification is deterministic and input-immutable",
          "[pool_preflight]")
{
    NxTpsPreflightInput  before;
    NxTpsPreflightResult a, b;
    int                  i;

    pf_input_record(&g_in, POOL_STATE_TARGET_ACTIVE);
    before = g_in;
    for (i = 0; i < 10; i++) {
        nx_tps_preflight_classify(&g_in, &a);
        nx_tps_preflight_classify(&g_in, &b);
        TEST_ASSERT_EQUAL_INT(0, memcmp(&a, &b, sizeof(a)));
    }
    TEST_ASSERT_EQUAL_INT(0, memcmp(&before, &g_in, sizeof(g_in)));
}

TEST_CASE("b102 cls: only EMPTY and CLEARED ever permit a pilot", "[pool_preflight]")
{
    int i;
    int permitting = 0;

    for (i = 0; i < (int)NX_TPS_PREFLIGHT__COUNT; i++) {
        if (nx_tps_preflight_permits_pilot((NxTpsPreflightOutcome)i)) {
            permitting++;
            TEST_ASSERT_TRUE(i == (int)NX_TPS_PREFLIGHT_EMPTY ||
                             i == (int)NX_TPS_PREFLIGHT_CLEARED);
        }
    }
    TEST_ASSERT_EQUAL_INT(2, permitting);
    /* Out-of-range values never permit. */
    TEST_ASSERT_FALSE(nx_tps_preflight_permits_pilot((NxTpsPreflightOutcome)9999));
}

/* ================================================================= */
/* A2. Whole-boot NVS mutation posture and non-destructive init       */
/* ================================================================= */

/*
 * The inspector is read-only for nx_tps, and in preflight posture the
 * DESTRUCTIVE NVS recovery is compiled out entirely: nvs_config_init() no
 * longer calls nvs_flash_erase(), and the boot gate initializes NVS without
 * recovery and refuses to continue boot when that fails. These cases pin the
 * resulting classification contract.
 */

TEST_CASE("b102 openfail: an unreadable namespace never reports EMPTY",
          "[pool_preflight]")
{
    /*
     * THE fail-open this component exists to prevent. An nvs_open failure that
     * is not "does not exist" also leaves namespace_present false. If the two
     * shared a representation, a flash fault or an out-of-memory would classify
     * EMPTY with permits_pilot=true — a pilot authorized on a store nobody read.
     */
    pf_input_empty(&g_in);
    g_in.namespace_present     = false; /* as a failed open leaves it */
    g_in.namespace_open_failed = true;
    g_in.store_loaded          = true;
    g_in.store_result          = (uint8_t)STORE_IO_ERROR;

    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_IO_ERROR, g_res.outcome);
    TEST_ASSERT_FALSE(g_res.permits_pilot);
}

TEST_CASE("b102 openfail: open failure outranks apparent absence",
          "[pool_preflight]")
{
    /* Absence alone permits; absence PLUS a failed open must not. */
    pf_input_empty(&g_in);
    g_in.namespace_present = false;
    g_in.store_loaded      = false;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_EMPTY, g_res.outcome);
    TEST_ASSERT_TRUE(g_res.permits_pilot);

    g_in.namespace_open_failed = true;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_IO_ERROR, g_res.outcome);
    TEST_ASSERT_FALSE(g_res.permits_pilot);
}

TEST_CASE("b102 openfail: the backend separates absent from unreadable",
          "[pool_preflight]")
{
    NxTpsPreflightNvsBackend b;

    nx_tps_preflight_nvs_init(&b);
    TEST_ASSERT_FALSE(b.open_failed);

    /* A genuinely absent namespace is NOT an open failure. */
    b.namespace_present = false;
    b.open_failed       = false;
    TEST_ASSERT_TRUE(nx_tps_preflight_nvs_namespace_absent(&b));
    TEST_ASSERT_FALSE(b.open_failed);
}

TEST_CASE("b102 nvsinit: an init failure blocks and never reports EMPTY",
          "[pool_preflight]")
{
    /* The dangerous shape: NVS unusable, so nothing can be read. A naive
     * inspector could see "no namespace" and call it EMPTY. */
    pf_input_empty(&g_in);
    g_in.namespace_present = false;
    g_in.store_loaded      = false;
    g_in.nvs_init_failed   = true;

    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_NVS_INIT_FAILED, g_res.outcome);
    TEST_ASSERT_FALSE(g_res.permits_pilot);
    TEST_ASSERT_TRUE(g_res.nvs_init_failed);
    TEST_ASSERT_EQUAL_STRING("TPS_PREFLIGHT_BLOCKED_NVS_INIT",
                             nx_tps_preflight_outcome_token(g_res.outcome));
}

TEST_CASE("b102 nvsinit: an init failure blocks whatever the store appears to say",
          "[pool_preflight]")
{
    const PoolStoreResult every[] = {
        STORE_EMPTY, STORE_CLEARED, STORE_OK, STORE_CORRUPT, STORE_IO_ERROR,
        STORE_UNSUPPORTED_SCHEMA, STORE_COMMIT_UNCERTAIN, STORE_RECOVERY_REQUIRED,
    };
    unsigned i;

    for (i = 0u; i < sizeof(every) / sizeof(every[0]); i++) {
        pf_input_empty(&g_in);
        g_in.store_result    = (uint8_t)every[i];
        g_in.nvs_init_failed = true;
        nx_tps_preflight_classify(&g_in, &g_res);
        TEST_ASSERT_FALSE(g_res.permits_pilot);
        TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_NVS_INIT_FAILED, g_res.outcome);
    }
}

TEST_CASE("b102 nvsinit: a broken inspector outranks an unusable NVS",
          "[pool_preflight]")
{
    /* Both faults at once: the more severe finding must win, because a
     * counter-positive inspector cannot be trusted to report anything. */
    pf_input_empty(&g_in);
    g_in.nvs_init_failed = true;
    g_in.write_attempts  = 1u;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_INTERNAL_ERROR, g_res.outcome);
    TEST_ASSERT_FALSE(g_res.permits_pilot);
}

TEST_CASE("b102 nvsinit: the summary reports the NVS-init fact",
          "[pool_preflight]")
{
    pf_input_empty(&g_in);
    g_in.nvs_init_failed = true;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_TRUE(nx_tps_preflight_format(&g_res, 3u, g_line,
                                             (uint32_t)sizeof(g_line)) > 0u);
    TEST_ASSERT_NOT_NULL(strstr(g_line, "outcome=TPS_PREFLIGHT_BLOCKED_NVS_INIT"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "pilot=0"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "nvsinit=0"));   /* 0 = NOT usable */
    /* No raw ESP-IDF error text or number leaks into the line. */
    TEST_ASSERT_NULL(strstr(g_line, "ESP_ERR"));
    TEST_ASSERT_NULL(strstr(g_line, "0x"));
}

TEST_CASE("b102 nvsinit: a healthy boot reports NVS usable", "[pool_preflight]")
{
    pf_input_empty(&g_in);
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_FALSE(g_res.nvs_init_failed);
    TEST_ASSERT_TRUE(nx_tps_preflight_format(&g_res, 1u, g_line,
                                             (uint32_t)sizeof(g_line)) > 0u);
    TEST_ASSERT_NOT_NULL(strstr(g_line, "nvsinit=1"));
}

TEST_CASE("b102 boot: unrelated-namespace writes do not affect the verdict",
          "[pool_preflight]")
{
    /*
     * nvs_config_init() writes the "main" namespace on the migration and
     * first-boot-default paths — but ONLY AFTER the boot gate has already
     * classified. Those touch no nx_tps key, so a healthy store classifies
     * normally and the inspector's own counters stay zero.
     */
    pf_input_empty(&g_in);
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_EMPTY, g_res.outcome);
    TEST_ASSERT_EQUAL_UINT32(0u, g_res.write_attempts);
    TEST_ASSERT_EQUAL_UINT32(0u, g_res.erase_attempts);
    TEST_ASSERT_EQUAL_UINT32(0u, g_res.commit_attempts);

    pf_input_record(&g_in, POOL_STATE_TARGET_ACTIVE);
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_RECORD_PRESENT, g_res.outcome);
    TEST_ASSERT_EQUAL_UINT32(0u, g_res.write_attempts);
}

TEST_CASE("b102 boot: the gate is one-shot and its verdict is never revised",
          "[pool_preflight]")
{
    NxTpsPreflightResult before, after;
    bool                 gate_a, gate_b;
    int                  i;

    gate_a = nx_tps_preflight_boot_gate();
    nx_tps_preflight_run_once(&before);
    for (i = 0; i < 10; i++) {
        gate_b = nx_tps_preflight_boot_gate();
        nx_tps_preflight_run_once(&after);
        TEST_ASSERT_EQUAL(gate_a, gate_b);
        TEST_ASSERT_EQUAL(before.outcome, after.outcome);
        TEST_ASSERT_EQUAL(before.permits_pilot, after.permits_pilot);
    }
    TEST_ASSERT_TRUE(nx_tps_preflight_run_count() <= 1u);
}

TEST_CASE("b102 boot: a blocking gate result never permits a pilot",
          "[pool_preflight]")
{
    /* Whatever the gate answers on this build, the two facts that matter hold:
     * a false gate means boot must stop, and no gate answer can turn a
     * blocking verdict into a permitting one. */
    NxTpsPreflightResult r;

    nx_tps_preflight_run_once(&r);
    if (!nx_tps_preflight_boot_gate()) {
        TEST_ASSERT_FALSE(r.permits_pilot);
    }
    /* permits_pilot is derived solely from the outcome, on every path. */
    TEST_ASSERT_EQUAL(nx_tps_preflight_permits_pilot(r.outcome), r.permits_pilot);
}

/* ================================================================= */
/* F. Privacy and reporting                                           */
/* ================================================================= */

TEST_CASE("b102 fmt: every outcome token is stable, bounded and dot-free",
          "[pool_preflight]")
{
    int i;

    for (i = 0; i < (int)NX_TPS_PREFLIGHT__COUNT; i++) {
        const char *t = nx_tps_preflight_outcome_token((NxTpsPreflightOutcome)i);
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_TRUE(strlen(t) > 0u);
        TEST_ASSERT_NULL(strchr(t, '.'));
        TEST_ASSERT_NULL(strchr(t, ' '));
        TEST_ASSERT_NULL(strchr(t, '='));
    }
    /* An unknown value resolves to a BLOCKING token, never a permitting one. */
    TEST_ASSERT_EQUAL_STRING("TPS_PREFLIGHT_INTERNAL_ERROR",
                             nx_tps_preflight_outcome_token((NxTpsPreflightOutcome)9999));
}

TEST_CASE("b102 fmt: the required token vocabulary is exactly as specified",
          "[pool_preflight]")
{
    TEST_ASSERT_EQUAL_STRING("TPS_PREFLIGHT_EMPTY",
        nx_tps_preflight_outcome_token(NX_TPS_PREFLIGHT_EMPTY));
    TEST_ASSERT_EQUAL_STRING("TPS_PREFLIGHT_CLEARED",
        nx_tps_preflight_outcome_token(NX_TPS_PREFLIGHT_CLEARED));
    TEST_ASSERT_EQUAL_STRING("TPS_PREFLIGHT_BLOCKED_RECORD",
        nx_tps_preflight_outcome_token(NX_TPS_PREFLIGHT_RECORD_PRESENT));
    TEST_ASSERT_EQUAL_STRING("TPS_PREFLIGHT_BLOCKED_TERMINAL",
        nx_tps_preflight_outcome_token(NX_TPS_PREFLIGHT_TERMINAL_PENDING));
    TEST_ASSERT_EQUAL_STRING("TPS_PREFLIGHT_BLOCKED_UNCERTAIN",
        nx_tps_preflight_outcome_token(NX_TPS_PREFLIGHT_COMMIT_UNCERTAIN));
    TEST_ASSERT_EQUAL_STRING("TPS_PREFLIGHT_BLOCKED_CORRUPT",
        nx_tps_preflight_outcome_token(NX_TPS_PREFLIGHT_CORRUPT));
    TEST_ASSERT_EQUAL_STRING("TPS_PREFLIGHT_BLOCKED_SCHEMA",
        nx_tps_preflight_outcome_token(NX_TPS_PREFLIGHT_UNSUPPORTED_SCHEMA));
    TEST_ASSERT_EQUAL_STRING("TPS_PREFLIGHT_BLOCKED_IO",
        nx_tps_preflight_outcome_token(NX_TPS_PREFLIGHT_IO_ERROR));
    TEST_ASSERT_EQUAL_STRING("TPS_PREFLIGHT_INTERNAL_ERROR",
        nx_tps_preflight_outcome_token(NX_TPS_PREFLIGHT_INTERNAL_ERROR));
}

TEST_CASE("b102 fmt: the summary line carries no store content", "[pool_preflight]")
{
    int i;

    for (i = 0; i < (int)NX_TPS_PREFLIGHT__COUNT; i++) {
        nx_tps_preflight_result_init(&g_res);
        g_res.outcome         = (NxTpsPreflightOutcome)i;
        g_res.permits_pilot   = nx_tps_preflight_permits_pilot(g_res.outcome);
        g_res.namespace_present = true;
        g_res.read_attempts   = 4294967295u;
        TEST_ASSERT_TRUE(nx_tps_preflight_format(&g_res, 4294967295u, g_line,
                                                 (uint32_t)sizeof(g_line)) > 0u);
        /* No namespace, key name, identity, credential or error string. */
        TEST_ASSERT_NULL(strstr(g_line, POOL_STORE_NVS_NAMESPACE));
        TEST_ASSERT_NULL(strstr(g_line, POOL_STORE_KEY_SLOT_A));
        TEST_ASSERT_NULL(strstr(g_line, POOL_STORE_KEY_SLOT_B));
        TEST_ASSERT_NULL(strstr(g_line, POOL_STORE_KEY_ACTIVE));
        TEST_ASSERT_NULL(strstr(g_line, "example"));
        TEST_ASSERT_NULL(strchr(g_line, '.'));
        TEST_ASSERT_NULL(strchr(g_line, ':'));
        TEST_ASSERT_NULL(strchr(g_line, '@'));
        TEST_ASSERT_NULL(strstr(g_line, "ESP_ERR"));
        TEST_ASSERT_NULL(strstr(g_line, "generation"));
        TEST_ASSERT_NULL(strstr(g_line, "epoch"));
    }
}

TEST_CASE("b102 fmt: the summary always reports the zero-mutation counters",
          "[pool_preflight]")
{
    nx_tps_preflight_result_init(&g_res);
    g_res.outcome       = NX_TPS_PREFLIGHT_EMPTY;
    g_res.permits_pilot = true;
    g_res.read_attempts = 3u;
    TEST_ASSERT_TRUE(nx_tps_preflight_format(&g_res, 7u, g_line,
                                             (uint32_t)sizeof(g_line)) > 0u);
    TEST_ASSERT_NOT_NULL(strstr(g_line, "TPS_PREFLIGHT_COMPLETE"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "outcome=TPS_PREFLIGHT_EMPTY"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "pilot=1"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "reads=3"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "w=0"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "e=0"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "c=0"));
    TEST_ASSERT_NOT_NULL(strstr(g_line, "up=7"));
}

TEST_CASE("b102 fmt: a too-small buffer yields an empty line, never a truncated one",
          "[pool_preflight]")
{
    char small[12];

    nx_tps_preflight_result_init(&g_res);
    TEST_ASSERT_EQUAL_UINT32(0u, nx_tps_preflight_format(&g_res, 0u, small,
                                                         (uint32_t)sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small);
    TEST_ASSERT_EQUAL_UINT32(0u, nx_tps_preflight_format(NULL, 0u, small,
                                                         (uint32_t)sizeof(small)));
    TEST_ASSERT_EQUAL_UINT32(0u, nx_tps_preflight_format(&g_res, 0u, NULL, 64u));

    /* The worst case fits the committed bound. */
    nx_tps_preflight_result_init(&g_res);
    g_res.outcome         = NX_TPS_PREFLIGHT_COMMIT_UNCERTAIN; /* longest token */
    g_res.read_attempts   = 4294967295u;
    g_res.write_attempts  = 4294967295u;
    g_res.erase_attempts  = 4294967295u;
    g_res.commit_attempts = 4294967295u;
    {
        char bounded[NX_TPS_PREFLIGHT_LINE_MAX];
        uint32_t n = nx_tps_preflight_format(&g_res, 4294967295u, bounded,
                                             (uint32_t)sizeof(bounded));
        TEST_ASSERT_TRUE(n > 0u);
        TEST_ASSERT_EQUAL_UINT32(n, (uint32_t)strlen(bounded));
    }
}

/* ================================================================= */
/* B. Read-only NVS adapter, driven through the committed B3 store    */
/* ================================================================= */

/*
 * A synthetic in-memory NVS model. It wraps the REAL preflight ops table's
 * contract but records every call, so "zero writes" is measured rather than
 * asserted. The real ESP-IDF adapter is exercised separately by the firmware
 * build; nothing here touches a physical partition.
 */
#define PF_SLOTS 3
#define PF_MAX   1200

typedef struct {
    bool    present;
    size_t  len;
    uint8_t bytes[PF_MAX];
} PfBlob;

typedef struct {
    bool   namespace_present;
    bool   opened;
    bool   io_fail;
    bool   size_query_fail;
    bool   open_fail;   /* nvs_open fails for a reason OTHER than NOT_FOUND */
    PfBlob v[PF_SLOTS];
    int    opens, reads, writes, commits, closes;
} PfFakeNvs;

static PfFakeNvs g_nvs;

static int pf_idx(const char *key)
{
    if (key == NULL) return -1;
    if (strcmp(key, POOL_STORE_KEY_SLOT_A) == 0) return 0;
    if (strcmp(key, POOL_STORE_KEY_SLOT_B) == 0) return 1;
    if (strcmp(key, POOL_STORE_KEY_ACTIVE) == 0) return 2;
    return -1;
}

static int fake_open(void *ctx)
{
    NxTpsPreflightNvsBackend *b = (NxTpsPreflightNvsBackend *)ctx;
    g_nvs.opens++;
    /* Models nvs_open(..., NVS_READONLY, ...): a missing namespace is
     * reported, never created — and a failure that is NOT "missing" is
     * reported as an open failure, never as absence. */
    if (g_nvs.open_fail) {
        b->open_failed       = true;
        b->namespace_present = false;
        b->open              = false;
        return POOL_STORE_BACKEND_IO;
    }
    if (!g_nvs.namespace_present) {
        b->namespace_present = false;
        b->open              = false;
        return POOL_STORE_BACKEND_OK;
    }
    g_nvs.opened         = true;
    b->namespace_present = true;
    b->open              = true;
    return POOL_STORE_BACKEND_OK;
}

static int fake_read(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *out_len)
{
    NxTpsPreflightNvsBackend *b = (NxTpsPreflightNvsBackend *)ctx;
    int idx = pf_idx(key);

    g_nvs.reads++;
    if (b->read_attempts < UINT32_MAX) b->read_attempts++;
    if (out_len == NULL) return POOL_STORE_BACKEND_IO;
    *out_len = 0;
    if (!b->namespace_present) return POOL_STORE_BACKEND_NOT_FOUND;
    if (!g_nvs.opened || idx < 0) return POOL_STORE_BACKEND_IO;
    if (g_nvs.io_fail) return POOL_STORE_BACKEND_IO;
    if (!g_nvs.v[idx].present) return POOL_STORE_BACKEND_NOT_FOUND;
    if (g_nvs.size_query_fail) return POOL_STORE_BACKEND_IO;
    *out_len = g_nvs.v[idx].len;
    if (buf == NULL || g_nvs.v[idx].len > cap) return POOL_STORE_BACKEND_OK;
    memcpy(buf, g_nvs.v[idx].bytes, g_nvs.v[idx].len);
    return POOL_STORE_BACKEND_OK;
}

/* The refusing stubs, mirroring the production adapter exactly. */
static int fake_write(void *ctx, const char *key, const uint8_t *buf, size_t len)
{
    NxTpsPreflightNvsBackend *b = (NxTpsPreflightNvsBackend *)ctx;
    (void)key; (void)buf; (void)len;
    g_nvs.writes++;
    if (b->write_attempts < UINT32_MAX) b->write_attempts++;
    return POOL_STORE_BACKEND_IO;
}

static int fake_commit(void *ctx)
{
    NxTpsPreflightNvsBackend *b = (NxTpsPreflightNvsBackend *)ctx;
    g_nvs.commits++;
    if (b->commit_attempts < UINT32_MAX) b->commit_attempts++;
    return POOL_STORE_BACKEND_IO;
}

static int fake_close(void *ctx)
{
    NxTpsPreflightNvsBackend *b = (NxTpsPreflightNvsBackend *)ctx;
    g_nvs.closes++;
    g_nvs.opened = false;
    b->open = false;
    return POOL_STORE_BACKEND_OK;
}

static const PoolStoreBackendOps g_fake_ops = {
    .open = fake_open, .read_blob = fake_read, .write_blob = fake_write,
    .commit = fake_commit, .close = fake_close,
};

static PoolSessionStore         g_store;
static NxTpsPreflightNvsBackend g_backend;
static PoolSessionRecord        g_rec;

static void pf_fresh(bool namespace_present)
{
    memset(&g_nvs, 0, sizeof(g_nvs));
    g_nvs.namespace_present = namespace_present;
    nx_tps_preflight_nvs_init(&g_backend);
    g_backend.namespace_present = true;
    memset(&g_store, 0, sizeof(g_store));
}

/* Seed a committed record through the REAL store, using a writable fake, so
 * the fixture is produced by the same encoder production uses. */
static PfFakeNvs g_seed_nvs;

static int seed_open(void *ctx) { (void)ctx; g_seed_nvs.opened = true; return POOL_STORE_BACKEND_OK; }
static int seed_read(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *out_len)
{
    int idx = pf_idx(key);
    (void)ctx;
    if (out_len == NULL || idx < 0) return POOL_STORE_BACKEND_IO;
    *out_len = 0;
    if (!g_seed_nvs.v[idx].present) return POOL_STORE_BACKEND_NOT_FOUND;
    *out_len = g_seed_nvs.v[idx].len;
    if (buf == NULL || g_seed_nvs.v[idx].len > cap) return POOL_STORE_BACKEND_OK;
    memcpy(buf, g_seed_nvs.v[idx].bytes, g_seed_nvs.v[idx].len);
    return POOL_STORE_BACKEND_OK;
}
static int seed_write(void *ctx, const char *key, const uint8_t *buf, size_t len)
{
    int idx = pf_idx(key);
    (void)ctx;
    if (idx < 0 || buf == NULL || len == 0 || len > PF_MAX) return POOL_STORE_BACKEND_IO;
    g_seed_nvs.v[idx].present = true;
    g_seed_nvs.v[idx].len = len;
    memcpy(g_seed_nvs.v[idx].bytes, buf, len);
    return POOL_STORE_BACKEND_OK;
}
static int seed_commit(void *ctx) { (void)ctx; return POOL_STORE_BACKEND_OK; }
static int seed_close(void *ctx) { (void)ctx; g_seed_nvs.opened = false; return POOL_STORE_BACKEND_OK; }

static const PoolStoreBackendOps g_seed_ops = {
    .open = seed_open, .read_blob = seed_read, .write_blob = seed_write,
    .commit = seed_commit, .close = seed_close,
};

#define PF_EPOCH_S 1750000000ull

static void pf_fill_identity(PoolConfigIdentity *c, PoolChainType chain,
                             const char *host, uint16_t port)
{
    memset(c, 0, sizeof(*c));
    c->chain = chain;
    strncpy(c->primary.host, host, sizeof(c->primary.host) - 1);
    c->primary.port = port;
    strncpy(c->primary.user, "acct.worker", sizeof(c->primary.user) - 1);
    c->primary.protocol = POOL_PROTO_STRATUM_V1;
}

/*
 * Produce a real committed record through the REAL encoder, then copy the
 * resulting blobs into the read-only fixture. The field set mirrors the
 * committed Gate B10 runtime fixture, which the B1 validator already accepts —
 * the point here is the read path, not re-deriving record validity rules.
 */
static void pf_seed_committed_session(PoolSessionState st, bool restore_required)
{
    PoolSessionStore seed;
    int i;

    memset(&g_seed_nvs, 0, sizeof(g_seed_nvs));
    memset(&g_rec, 0, sizeof(g_rec));
    pool_session_record_init(&g_rec);
    g_rec.session_id       = 991u;
    g_rec.b1_model_version = POOL_SESSION_MODEL_VERSION;
    g_rec.state            = st;
    pf_fill_identity(&g_rec.source, POOL_CHAIN_BITCOIN, "src.example", 3333);
    pf_fill_identity(&g_rec.target, POOL_CHAIN_BITCOIN_CASH, "tgt.example", 3334);
    g_rec.password_policy = POOL_SESSION_PW_KEEP_CURRENT;
    /* The B1 obligation is monotonic: once the target has been applied the
     * device owes the source pool a restoration, and the committed validator
     * rejects a record that claims otherwise. */
    g_rec.restore_required = restore_required;
    g_rec.target_verify.connection_observed = true;
    g_rec.target_verify.mining_observed     = true;
    g_rec.target_verify.identity_verified   = true;
    g_rec.duration_s               = 3600u;
    g_rec.verified_start_valid     = true;
    g_rec.verified_start_epoch_s   = PF_EPOCH_S;
    g_rec.deadline_valid           = true;
    g_rec.deadline_epoch_s         = PF_EPOCH_S + 3600u;
    g_rec.deadline_sync_generation = 1u;
    g_rec.latest_trusted_valid     = true;
    g_rec.latest_trusted_epoch_s   = PF_EPOCH_S + 100u;
    if (st == POOL_STATE_COMPLETE) {
        g_rec.restore_verify.connection_observed = true;
        g_rec.restore_verify.mining_observed     = true;
        g_rec.restore_verify.identity_verified   = true;
    }
    TEST_ASSERT_EQUAL(RECORD_OK, pool_session_record_validate(&g_rec));

    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_init(&seed, &g_seed_ops, NULL));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_commit_record(&seed, &g_rec));
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_deinit(&seed));

    for (i = 0; i < PF_SLOTS; i++) {
        g_nvs.v[i] = g_seed_nvs.v[i];
    }
}

TEST_CASE("b102 nvs: an absent namespace is reported, never created",
          "[pool_preflight]")
{
    pf_fresh(false);
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_init(&g_store, &g_fake_ops, &g_backend));

    TEST_ASSERT_FALSE(g_backend.namespace_present);
    TEST_ASSERT_TRUE(nx_tps_preflight_nvs_namespace_absent(&g_backend));
    TEST_ASSERT_EQUAL_INT(0, g_nvs.writes);
    TEST_ASSERT_EQUAL_INT(0, g_nvs.commits);
    TEST_ASSERT_EQUAL_UINT32(0u, g_backend.write_attempts);
    TEST_ASSERT_EQUAL_UINT32(0u, g_backend.commit_attempts);
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("b102 nvs: a failed open is reported as unreadable, not as absent",
          "[pool_preflight]")
{
    NxTpsPreflightInput in;

    pf_fresh(true);
    g_nvs.open_fail = true; /* e.g. ESP_ERR_NO_MEM or a flash fault */

    /* The store init fails, and CRUCIALLY the backend distinguishes the two. */
    TEST_ASSERT_EQUAL(STORE_IO_ERROR,
                      pool_session_store_init(&g_store, &g_fake_ops, &g_backend));
    TEST_ASSERT_TRUE(g_backend.open_failed);
    TEST_ASSERT_FALSE(g_backend.namespace_present);

    /* End to end through the same wiring the boot module uses: open failure is
     * consulted BEFORE absence, so this can never come out as EMPTY. */
    memset(&in, 0, sizeof(in));
    in.model_version         = NX_TPS_PREFLIGHT_MODEL_VERSION;
    in.namespace_present     = g_backend.namespace_present;
    in.namespace_open_failed = g_backend.open_failed;
    in.store_loaded          = true;
    in.store_result          = (uint8_t)STORE_IO_ERROR;
    nx_tps_preflight_classify(&in, &g_res);

    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_IO_ERROR, g_res.outcome);
    TEST_ASSERT_FALSE(g_res.permits_pilot);
    TEST_ASSERT_EQUAL_INT(0, g_nvs.writes);
    TEST_ASSERT_EQUAL_INT(0, g_nvs.commits);
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("b102 nvs: an empty namespace loads EMPTY with zero writes",
          "[pool_preflight]")
{
    PoolStoreResult sr;

    pf_fresh(true);
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_init(&g_store, &g_fake_ops, &g_backend));
    sr = pool_session_store_load(&g_store, NULL, NULL);

    TEST_ASSERT_EQUAL(STORE_EMPTY, sr);
    TEST_ASSERT_TRUE(g_nvs.reads > 0);
    TEST_ASSERT_EQUAL_INT(0, g_nvs.writes);
    TEST_ASSERT_EQUAL_INT(0, g_nvs.commits);
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_deinit(&g_store));
    TEST_ASSERT_EQUAL_INT(1, g_nvs.closes); /* handle closed on every path */
}

TEST_CASE("b102 nvs: a committed session record loads without any write",
          "[pool_preflight]")
{
    PoolStoreResult sr;

    pf_fresh(true);
    pf_seed_committed_session(POOL_STATE_TARGET_ACTIVE, true);
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_init(&g_store, &g_fake_ops, &g_backend));

    memset(&g_rec, 0, sizeof(g_rec));
    sr = pool_session_store_load(&g_store, &g_rec, NULL);
    TEST_ASSERT_EQUAL(STORE_OK, sr);
    TEST_ASSERT_EQUAL(POOL_STATE_TARGET_ACTIVE, g_rec.state);

    /* The whole point: reading a real record mutated nothing. */
    TEST_ASSERT_EQUAL_INT(0, g_nvs.writes);
    TEST_ASSERT_EQUAL_INT(0, g_nvs.commits);
    TEST_ASSERT_EQUAL_UINT32(0u, g_backend.write_attempts);
    TEST_ASSERT_EQUAL_UINT32(0u, g_backend.commit_attempts);
    TEST_ASSERT_EQUAL_UINT32(0u, g_backend.erase_attempts);

    /* And the end-to-end classification blocks. */
    memset(&g_in, 0, sizeof(g_in));
    g_in.model_version     = NX_TPS_PREFLIGHT_MODEL_VERSION;
    g_in.namespace_present = true;
    g_in.store_loaded      = true;
    g_in.store_result      = (uint8_t)sr;
    g_in.record_kind       = g_rec.kind;
    g_in.session_state     = (uint8_t)g_rec.state;
    g_in.restore_required  = g_rec.restore_required;
    g_in.record_valid      = (pool_session_record_validate(&g_rec) == RECORD_OK);
    g_in.write_attempts    = g_backend.write_attempts;
    g_in.commit_attempts   = g_backend.commit_attempts;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_RECORD_PRESENT, g_res.outcome);
    TEST_ASSERT_FALSE(g_res.permits_pilot);

    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("b102 nvs: a corrupt payload never becomes EMPTY", "[pool_preflight]")
{
    PoolStoreResult sr;

    pf_fresh(true);
    pf_seed_committed_session(POOL_STATE_TARGET_ACTIVE, true);
    /* Flip bytes inside the pointed record so decoding/CRC fails. */
    g_nvs.v[0].bytes[8] ^= 0xFFu;
    g_nvs.v[1].bytes[8] ^= 0xFFu;

    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_init(&g_store, &g_fake_ops, &g_backend));
    sr = pool_session_store_load(&g_store, NULL, NULL);
    TEST_ASSERT_NOT_EQUAL(STORE_EMPTY, sr);
    TEST_ASSERT_NOT_EQUAL(STORE_CLEARED, sr);

    memset(&g_in, 0, sizeof(g_in));
    g_in.model_version     = NX_TPS_PREFLIGHT_MODEL_VERSION;
    g_in.namespace_present = true;
    g_in.store_loaded      = true;
    g_in.store_result      = (uint8_t)sr;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_FALSE(g_res.permits_pilot);

    TEST_ASSERT_EQUAL_INT(0, g_nvs.writes);
    TEST_ASSERT_EQUAL_INT(0, g_nvs.commits);
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("b102 nvs: a truncated value never becomes EMPTY", "[pool_preflight]")
{
    PoolStoreResult sr;

    pf_fresh(true);
    pf_seed_committed_session(POOL_STATE_TARGET_ACTIVE, true);
    g_nvs.v[0].len = 3; /* truncated below any decodable record */
    g_nvs.v[1].len = 3;

    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_init(&g_store, &g_fake_ops, &g_backend));
    sr = pool_session_store_load(&g_store, NULL, NULL);
    TEST_ASSERT_NOT_EQUAL(STORE_EMPTY, sr);
    TEST_ASSERT_NOT_EQUAL(STORE_CLEARED, sr);
    TEST_ASSERT_EQUAL_INT(0, g_nvs.writes);
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("b102 nvs: an I/O failure never becomes EMPTY and writes nothing",
          "[pool_preflight]")
{
    PoolStoreResult sr;

    pf_fresh(true);
    g_nvs.io_fail = true;
    TEST_ASSERT_EQUAL(STORE_OK, pool_session_store_init(&g_store, &g_fake_ops, &g_backend));
    sr = pool_session_store_load(&g_store, NULL, NULL);

    TEST_ASSERT_EQUAL(STORE_IO_ERROR, sr);
    memset(&g_in, 0, sizeof(g_in));
    g_in.model_version     = NX_TPS_PREFLIGHT_MODEL_VERSION;
    g_in.namespace_present = true;
    g_in.store_loaded      = true;
    g_in.store_result      = (uint8_t)sr;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_IO_ERROR, g_res.outcome);
    TEST_ASSERT_FALSE(g_res.permits_pilot);

    TEST_ASSERT_EQUAL_INT(0, g_nvs.writes);
    TEST_ASSERT_EQUAL_INT(0, g_nvs.commits);
    (void)pool_session_store_deinit(&g_store);
}

TEST_CASE("b102 nvs: the refusing stubs count and reject every mutation",
          "[pool_preflight]")
{
    const PoolStoreBackendOps *ops = nx_tps_preflight_nvs_ops();
    NxTpsPreflightNvsBackend   b;
    static const uint8_t       payload[4] = { 1, 2, 3, 4 };

    nx_tps_preflight_nvs_init(&b);
    TEST_ASSERT_NOT_NULL(ops->write_blob);
    TEST_ASSERT_NOT_NULL(ops->commit);

    /* The PRODUCTION ops table refuses unconditionally and counts. */
    TEST_ASSERT_EQUAL_INT(POOL_STORE_BACKEND_IO,
                          ops->write_blob(&b, POOL_STORE_KEY_SLOT_A, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_INT(POOL_STORE_BACKEND_IO, ops->commit(&b));
    TEST_ASSERT_EQUAL_UINT32(1u, b.write_attempts);
    TEST_ASSERT_EQUAL_UINT32(1u, b.commit_attempts);
    TEST_ASSERT_EQUAL_UINT32(0u, b.erase_attempts);

    /* And a verdict built from those counters fails closed. */
    pf_input_empty(&g_in);
    g_in.write_attempts  = b.write_attempts;
    g_in.commit_attempts = b.commit_attempts;
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_INTERNAL_ERROR, g_res.outcome);
    TEST_ASSERT_FALSE(g_res.permits_pilot);
}

TEST_CASE("b102 nvs: a fresh backend context starts closed and unobserved",
          "[pool_preflight]")
{
    NxTpsPreflightNvsBackend b;

    memset(&b, 0xAA, sizeof(b));
    nx_tps_preflight_nvs_init(&b);
    TEST_ASSERT_FALSE(b.open);
    TEST_ASSERT_EQUAL_UINT32(0u, b.read_attempts);
    TEST_ASSERT_EQUAL_UINT32(0u, b.write_attempts);
    TEST_ASSERT_EQUAL_UINT32(0u, b.erase_attempts);
    TEST_ASSERT_EQUAL_UINT32(0u, b.commit_attempts);
    nx_tps_preflight_nvs_init(NULL); /* must not crash */
    TEST_ASSERT_FALSE(nx_tps_preflight_nvs_namespace_absent(NULL));
}

/* ================================================================= */
/* C. Boot posture                                                    */
/* ================================================================= */

TEST_CASE("b102 boot: the flag default is off and the posture is inert",
          "[pool_preflight]")
{
#ifdef CONFIG_NX_TIMED_SESSIONS_STORE_PREFLIGHT
    TEST_ASSERT_TRUE(nx_tps_preflight_enabled());
#else
    /* The shipped default: no preflight exists at all. */
    TEST_ASSERT_FALSE(nx_tps_preflight_enabled());
    TEST_ASSERT_EQUAL_UINT32(0u, nx_tps_preflight_run_count());

    /* Calling it is a safe no-op that yields the fail-closed verdict and
     * performs no NVS access whatsoever. */
    memset(&g_res, 0xAA, sizeof(g_res));
    nx_tps_preflight_run_once(&g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_INTERNAL_ERROR, g_res.outcome);
    TEST_ASSERT_FALSE(g_res.permits_pilot);
    TEST_ASSERT_EQUAL_UINT32(0u, nx_tps_preflight_run_count());

    nx_tps_preflight_run_once(NULL); /* must not crash */
#endif
}

TEST_CASE("b102 boot: repeated calls never re-read (no polling loop)",
          "[pool_preflight]")
{
    uint32_t first;
    int      i;

    nx_tps_preflight_run_once(NULL);
    first = nx_tps_preflight_run_count();
    for (i = 0; i < 20; i++) {
        nx_tps_preflight_run_once(&g_res);
    }
    TEST_ASSERT_EQUAL_UINT32(first, nx_tps_preflight_run_count());
    TEST_ASSERT_TRUE(nx_tps_preflight_run_count() <= 1u);
}

TEST_CASE("b102 boot: a preflight result never authorizes anything",
          "[pool_preflight]")
{
    /* The verdict has no field through which it could authorize: it carries
     * an outcome, a permission-to-PROCEED-WITH-A-DECISION flag, and audit
     * counters. Even the permitting outcomes grant nothing by themselves. */
    pf_input_empty(&g_in);
    nx_tps_preflight_classify(&g_in, &g_res);
    TEST_ASSERT_EQUAL(NX_TPS_PREFLIGHT_EMPTY, g_res.outcome);
    TEST_ASSERT_TRUE(g_res.permits_pilot);
    TEST_ASSERT_EQUAL_UINT32(0u, g_res.write_attempts);
    TEST_ASSERT_EQUAL_UINT32(0u, g_res.erase_attempts);
    TEST_ASSERT_EQUAL_UINT32(0u, g_res.commit_attempts);
    TEST_ASSERT_EQUAL_UINT32(NX_TPS_PREFLIGHT_MODEL_VERSION, g_res.model_version);
    TEST_ASSERT_EQUAL_UINT32(sizeof(NxTpsPreflightResult), sizeof(g_res));
}
