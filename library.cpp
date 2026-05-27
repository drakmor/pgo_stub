#define LIBRARY_IMPL 1
#include "library.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PLAYGO_ENABLE_LOGGING
#define PLAYGO_ENABLE_LOGGING 1
#endif

static constexpr uint32_t kDefaultChunkCount = 1000;
static constexpr uint32_t kConfigLineSize = 65536;

struct PlayGoState {
    int initialized;
    uint32_t open_count;
    ScePlayGoHandle handle;
    ScePlayGoInstallSpeed install_speed;
    ScePlayGoLanguageMask language_mask;
    uint32_t chunk_count;
    uint32_t max_chunk_id;
    uint32_t scenario_count;
    int config_loaded_from_file;
    ScePlayGoChunkId chunk_ids[SCE_PLAYGO_CHUNK_INDEX_MAX];
    uint8_t chunk_valid[SCE_PLAYGO_CHUNK_INDEX_MAX];
    uint32_t todo_count;
    ScePlayGoToDo todo[SCE_PLAYGO_CHUNK_INDEX_MAX];
};

static PlayGoState g_playgo;
static char g_config_line[kConfigLineSize];

#if PLAYGO_ENABLE_LOGGING
static void pg_log_reset(void) {
    FILE* fp = fopen("/app0/playlgo.log", "wb");
    if (fp != NULL) {
        fclose(fp);
    }
}

static void pg_logf(const char* fmt, ...) {
    FILE* fp;
    va_list ap;

    fp = fopen("/app0/playlgo.log", "ab");
    if (fp == NULL) {
        return;
    }

    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}
#else
static void pg_log_reset(void) {}
static void pg_logf(const char* fmt, ...) { (void)fmt; }
#endif

extern "C" {
    int module_start(size_t args, const void* argp) {
        pg_log_reset();
        pg_logf("module_start: module loaded args=%zu argp=%p", args, argp);
        return 0;
    }

    int module_stop(size_t args, const void* argp) {
        pg_logf("module_stop: module unloading args=%zu argp=%p", args, argp);
        return 0;
    }
}

/*
 * Small logging helpers.
 *
 * The log should explain what the caller asked for and what the stub returned
 * without requiring a debugger. These helpers keep the per-function log lines
 * short and consistent.
 */
static unsigned long long pg_optional_mask_value(const ScePlayGoOptionalChunk* option) {
    return option ? (unsigned long long)option->bitmask : 0ULL;
}

static unsigned pg_first_chunk_id(const ScePlayGoChunkId* chunkIds, uint32_t numberOfEntries) {
    return (chunkIds != NULL && numberOfEntries > 0) ? (unsigned)chunkIds[0] : 0u;
}

static unsigned pg_last_chunk_id(const ScePlayGoChunkId* chunkIds, uint32_t numberOfEntries) {
    return (chunkIds != NULL && numberOfEntries > 0) ? (unsigned)chunkIds[numberOfEntries - 1] : 0u;
}

static unsigned pg_log_bad_chunk_id(int32_t rc, ScePlayGoChunkId badChunkId) {
    return (rc == SCE_PLAYGO_ERROR_BAD_CHUNK_ID || rc == SCE_PLAYGO_ERROR_PROHIBIT_CHUNK_ID || rc == SCE_PLAYGO_ERROR_BAD_SIZE) ? (unsigned)badChunkId : 0u;
}

static unsigned pg_log_bad_chunk_index(int32_t rc, uint32_t badIndex) {
    return (rc == SCE_PLAYGO_ERROR_BAD_CHUNK_ID || rc == SCE_PLAYGO_ERROR_PROHIBIT_CHUNK_ID || rc == SCE_PLAYGO_ERROR_BAD_SIZE) ? badIndex : 0u;
}

static const char* pg_snapshot_filename_or_default(const char* filename) {
    return filename != NULL ? filename : "/app0/playgo_snapshot.txt";
}

/*
 * Add one valid chunk id to the configured package.
 */
static int pg_add_chunk_id(ScePlayGoChunkId id) {
    if ((uint32_t)id >= SCE_PLAYGO_CHUNK_INDEX_MAX) {
        return 0;
    }
    if (g_playgo.chunk_valid[id]) {
        return 1;
    }
    if (g_playgo.chunk_count >= SCE_PLAYGO_CHUNK_INDEX_MAX) {
        return 0;
    }
    g_playgo.chunk_valid[id] = 1;
    g_playgo.chunk_ids[g_playgo.chunk_count++] = id;
    if ((uint32_t)id > g_playgo.max_chunk_id) {
        g_playgo.max_chunk_id = id;
    }
    return 1;
}

/*
 * Reset the chunk id cache to a linear sequence [0, chunk_count).
 *
 * The stub treats every chunk as already installed and reachable, so there is
 * no need for a complex chunk database. A simple deterministic id list is good
 * enough for titles that query the available chunks and then ask for their
 * locus/progress.
 */
static void pg_reset_chunk_ids(uint32_t chunkCount) {
    uint32_t i;
    memset(g_playgo.chunk_ids, 0, sizeof(g_playgo.chunk_ids));
    memset(g_playgo.chunk_valid, 0, sizeof(g_playgo.chunk_valid));
    g_playgo.chunk_count = 0;
    g_playgo.max_chunk_id = 0;
    for (i = 0; i < chunkCount && i < SCE_PLAYGO_CHUNK_INDEX_MAX; ++i) {
        (void)pg_add_chunk_id((ScePlayGoChunkId)i);
    }
}

static int pg_parse_chunk_count_line(const char* line, uint32_t* outChunkCount) {
    char* end;
    unsigned long value;

    errno = 0;
    value = strtoul(line, &end, 10);
    if (errno != 0 || end == line || value == 0) {
        return 0;
    }
    if (value > SCE_PLAYGO_CHUNK_INDEX_MAX) {
        value = SCE_PLAYGO_CHUNK_INDEX_MAX;
    }
    *outChunkCount = (uint32_t)value;
    return 1;
}

static int pg_parse_chunk_id_list_line(char* line) {
    char* cursor = line;
    char* end;
    uint32_t added = 0;

    memset(g_playgo.chunk_ids, 0, sizeof(g_playgo.chunk_ids));
    memset(g_playgo.chunk_valid, 0, sizeof(g_playgo.chunk_valid));
    g_playgo.chunk_count = 0;
    g_playgo.max_chunk_id = 0;

    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n' || *cursor == ',') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        errno = 0;
        unsigned long value = strtoul(cursor, &end, 10);
        if (errno != 0 || end == cursor || value >= SCE_PLAYGO_CHUNK_INDEX_MAX) {
            return 0;
        }
        if (!pg_add_chunk_id((ScePlayGoChunkId)value)) {
            return 0;
        }
        ++added;

        cursor = end;
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
            ++cursor;
        }
        if (*cursor != '\0' && *cursor != ',') {
            return 0;
        }
    }

    return added > 0;
}

/*
 * Reset the whole global stub state to defaults.
 *
 * Defaults are intentionally optimistic: the library behaves as if PlayGo is
 * fully initialized, all languages are available, all scenarios are available,
 * and installation is already complete. The title can continue running even when the real PlayGo service is missing.
 */
static void pg_set_defaults(void) {
    memset(&g_playgo, 0, sizeof(g_playgo));
    g_playgo.handle = 1;
    g_playgo.install_speed = SCE_PLAYGO_INSTALL_SPEED_TRICKLE;
    g_playgo.language_mask = SCE_PLAYGO_LANGUAGE_MASK_ALL;
    g_playgo.scenario_count = 0;
    g_playgo.config_loaded_from_file = 0;
    pg_reset_chunk_ids(kDefaultChunkCount);
}

/*
 * Load /app0/playgo_stub.dat.
 *
 * File format:
 *   line 1: chunk count, or comma-separated valid chunk id list
 *   line 2: scenario count (optional)
 *
 * If the file is missing or malformed, the stub falls back to 1000 chunks.
 * Chunk ids are valid in the half-open range [0, chunk_count), so a 1000-chunk
 * package exposes ids 0..999 and reports BAD_CHUNK_ID for 1000. If line 1
 * contains commas, it is treated as the exact valid chunk id list instead.
 */
static void pg_load_config(void) {
    FILE* fp;
    char* line = g_config_line;
    unsigned long value;
    uint32_t chunkCount;
    int parsedChunkConfig = 0;

    pg_reset_chunk_ids(kDefaultChunkCount);
    g_playgo.scenario_count = 0;
    g_playgo.config_loaded_from_file = 0;

    fp = fopen("/app0/playgo_stub.dat", "rb");
    if (fp == NULL) {
        pg_logf("config: using defaults chunk_count=%u scenario_count=%u",
            g_playgo.chunk_count,
            g_playgo.scenario_count);
        return;
    }

    if (fgets(line, sizeof(line), fp) != NULL) {
        if (strchr(line, ',') != NULL) {
            parsedChunkConfig = pg_parse_chunk_id_list_line(line);
        } else if (pg_parse_chunk_count_line(line, &chunkCount)) {
            pg_reset_chunk_ids(chunkCount);
            parsedChunkConfig = 1;
        }
    }
    if (!parsedChunkConfig) {
        pg_reset_chunk_ids(kDefaultChunkCount);
    }

    errno = 0;
    if (fgets(line, sizeof(line), fp) != NULL) {
        value = strtoul(line, NULL, 10);
        if (errno == 0) {
            g_playgo.scenario_count = (uint32_t)value;
        }
    }

    fclose(fp);
    g_playgo.config_loaded_from_file = 1;
    pg_logf("config: loaded chunk_count=%u scenario_count=%u",
        g_playgo.chunk_count,
        g_playgo.scenario_count);
}

/*
 * Helper that enforces the original library lifecycle contract.
 *
 * Most exported functions are invalid until scePlayGoInitialize succeeds.
 */
static int32_t pg_require_initialized(void) {
    return g_playgo.initialized ? SCE_OK : SCE_PLAYGO_ERROR_NOT_INITIALIZED;
}

/*
 * Helper that validates the single stub handle.
 *
 * The real library has a more complex internal client object. The stub keeps a
 * single stable handle value (1) and requires at least one successful Open.
 */
static int32_t pg_require_handle(ScePlayGoHandle handle) {
    if (g_playgo.open_count == 0 || handle != g_playgo.handle) {
        return SCE_PLAYGO_ERROR_BAD_HANDLE;
    }
    return SCE_OK;
}

/*
 * Validate count-style API arguments.
 *
 * A large part of PlayGo uses list/count output APIs. The original library caps
 * chunk-oriented requests to the configured table size; the stub mirrors that
 * expectation while allowing larger synthetic packages for titles that need it.
 */
static int pg_valid_count(uint32_t count) {
    return count > 0 && count <= SCE_PLAYGO_CHUNK_INDEX_MAX;
}

/*
 * Validate optional chunk selector type.
 */
static int pg_valid_optional_type(ScePlayGoOptionalChunkType type) {
    return type >= SCE_PLAYGO_OPTIONAL_CHUNK_TYPE_LANGUAGE &&
        type <= SCE_PLAYGO_OPTIONAL_CHUNK_TYPE_OBSERVED_RESERVED;
}

/*
 * Validate that every requested chunk id belongs to the configured package.
 *
 * The stub exposes a linear synthetic package with chunk ids [0, chunk_count).
 * Requests outside that range should not silently succeed, otherwise titles may
 * believe that non-existent chunks are installed and keep polling forever.
 *
 * Chunk ids beyond the configured package extent are invalid ids. Holes inside
 * an explicit chunk-id list are prohibited chunks: titles commonly keep
 * enumerating after PROHIBIT_CHUNK_ID and stop only at BAD_CHUNK_ID.
 */
static int32_t pg_validate_chunk_list(const ScePlayGoChunkId* chunkIds, uint32_t numberOfEntries, uint32_t* outBadIndex, ScePlayGoChunkId* outBadChunkId) {
    uint32_t i;

    if (outBadIndex != NULL) {
        *outBadIndex = 0;
    }
    if (outBadChunkId != NULL) {
        *outBadChunkId = 0;
    }

    if (chunkIds == NULL) {
        return SCE_PLAYGO_ERROR_BAD_POINTER;
    }
    if (!pg_valid_count(numberOfEntries)) {
        return SCE_PLAYGO_ERROR_BAD_SIZE;
    }

    for (i = 0; i < numberOfEntries; ++i) {
        ScePlayGoChunkId id = chunkIds[i];
        if ((uint32_t)id >= SCE_PLAYGO_CHUNK_INDEX_MAX) {
            if (outBadIndex != NULL) {
                *outBadIndex = i;
            }
            if (outBadChunkId != NULL) {
                *outBadChunkId = id;
            }
            return SCE_PLAYGO_ERROR_BAD_CHUNK_ID;
        }
        if ((uint32_t)id > g_playgo.max_chunk_id) {
            if (outBadIndex != NULL) {
                *outBadIndex = i;
            }
            if (outBadChunkId != NULL) {
                *outBadChunkId = id;
            }
            return SCE_PLAYGO_ERROR_BAD_CHUNK_ID;
        }
        if (!g_playgo.chunk_valid[id]) {
            if (outBadIndex != NULL) {
                *outBadIndex = i;
            }
            if (outBadChunkId != NULL) {
                *outBadChunkId = id;
            }
            return SCE_PLAYGO_ERROR_PROHIBIT_CHUNK_ID;
        }
    }

    return SCE_OK;
}

/*
 * Return a scenario bitmask with all configured scenarios enabled.
 *
 * Scenario masks are 64-bit wide in the ABI. When the configuration says there
 * are more than 64 scenarios, the stub reports all 64 bits set because that is
 * the maximum representable value in the public API.
 */
static uint64_t pg_scenario_mask_all(void) {
    if (g_playgo.scenario_count >= 64) {
        return ~0ULL;
    }
    if (g_playgo.scenario_count == 0) {
        return 0ULL;
    }
    return (1ULL << g_playgo.scenario_count) - 1ULL;
}

/*
 * Shared implementation for GetChunkId/GetInstallChunkId style APIs.
 *
 * The helper follows the usual PlayGo list-query pattern:
 *
 * - when outChunkIdList is NULL, the caller is asking only for the total count,
 *   so the function returns the configured chunk count through outEntries;
 * - when outChunkIdList is non-NULL, the caller provided storage for a page of
 *   results, so the function returns as many configured chunk ids as fit in the
 *   provided array and reports how many entries were written.
 *
 * The configured set may be either a synthetic 0..N-1 range or the exact list
 * supplied in /app0/playgo_stub.dat.
 */
static int32_t pg_copy_chunk_ids(ScePlayGoChunkId* outChunkIdList, uint32_t numberOfEntries, uint32_t* outEntries) {
    uint32_t copied;
    uint32_t i;

    if (outEntries == NULL) {
        return SCE_PLAYGO_ERROR_BAD_POINTER;
    }
    if (outChunkIdList == NULL) {
        *outEntries = (g_playgo.chunk_count < SCE_PLAYGO_CHUNK_INDEX_MAX) ? g_playgo.chunk_count : SCE_PLAYGO_CHUNK_INDEX_MAX;
        return SCE_OK;
    }
    if (!pg_valid_count(numberOfEntries)) {
        return SCE_PLAYGO_ERROR_BAD_SIZE;
    }

    copied = (g_playgo.chunk_count < SCE_PLAYGO_CHUNK_INDEX_MAX) ? g_playgo.chunk_count : SCE_PLAYGO_CHUNK_INDEX_MAX;
    if (copied > numberOfEntries) {
        copied = numberOfEntries;
    }
    for (i = 0; i < copied; ++i) {
        outChunkIdList[i] = g_playgo.chunk_ids[i];
    }
    *outEntries = copied;
    return SCE_OK;
}

extern "C" {

    /*
     * Initialize the PlayGo client.
     *
     * The original library validates the init buffer and creates its internal IPMI
     * client. The stub does not talk to any service, but it still validates the
     * public arguments closely enough for local compatibility. On success it resets
     * internal state, loads /app0/playgo_stub.dat, marks itself initialized and
     * returns SCE_OK.
     */
    PRX_INTERFACE int32_t scePlayGoInitialize(const ScePlayGoInitParams* initParam) {
        int32_t rc;
        if (g_playgo.initialized) {
            rc = SCE_PLAYGO_ERROR_ALREADY_INITIALIZED;
            pg_logf("scePlayGoInitialize rc=%d already_initialized=1", rc);
            return rc;
        }
        if (initParam == NULL ||
            initParam->bufAddr == NULL ||
            initParam->reserved != 0 ||
            initParam->bufSize < SCE_PLAYGO_HEAP_SIZE) {
            rc = SCE_PLAYGO_ERROR_INVALID_ARGUMENT;
            pg_logf("scePlayGoInitialize rc=%d invalid_args bufAddr=%p bufSize=%u reserved=%u", rc, initParam ? initParam->bufAddr : NULL, initParam ? initParam->bufSize : 0u, initParam ? initParam->reserved : 0u);
            return rc;
        }

        pg_set_defaults();
        pg_load_config();
        g_playgo.initialized = 1;
        pg_logf("scePlayGoInitialize rc=0 chunk_count=%u scenario_count=%u config_source=%s language_mask=0x%016llx install_speed=%d",
            g_playgo.chunk_count,
            g_playgo.scenario_count,
            g_playgo.config_loaded_from_file ? "file" : "defaults",
            (unsigned long long)g_playgo.language_mask,
            g_playgo.install_speed);
        return SCE_OK;
    }

    /*
     * Terminate the PlayGo client.
     *
     * The stub simply resets its state back to defaults. This mirrors the external
     * lifecycle contract of the original API without involving any real backend.
     */
    PRX_INTERFACE int32_t scePlayGoTerminate(void) {
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) {
            pg_logf("scePlayGoTerminate rc=%d", rc);
            return rc;
        }
        pg_set_defaults();
        pg_logf("scePlayGoTerminate rc=0 state_reset=1");
        return SCE_OK;
    }

    /*
     * Open a PlayGo handle.
     *
     * The stub exposes a single fixed handle value and keeps a simple open_count so
     * Close can reject invalid use. The param argument is ignored, matching common
     * sample code that passes NULL.
     */
    PRX_INTERFACE int32_t scePlayGoOpen(ScePlayGoHandle* outHandle, const void* param) {
        int32_t rc = pg_require_initialized();
        (void)param;
        if (rc != SCE_OK) {
            pg_logf("scePlayGoOpen rc=%d", rc);
            return rc;
        }
        if (outHandle == NULL) {
            rc = SCE_PLAYGO_ERROR_BAD_POINTER;
            pg_logf("scePlayGoOpen rc=%d bad_pointer", rc);
            return rc;
        }
        g_playgo.open_count++;
        *outHandle = g_playgo.handle;
        pg_logf("scePlayGoOpen rc=0 handle=%d open_count=%u chunk_count=%u scenario_count=%u", g_playgo.handle, g_playgo.open_count, g_playgo.chunk_count, g_playgo.scenario_count);
        return SCE_OK;
    }

    /*
     * Close a PlayGo handle.
     *
     * The stub accepts only the single handle returned by Open and decreases the
     * open reference count. No external resources are released because there is no
     * backend service involved.
     */
    PRX_INTERFACE int32_t scePlayGoClose(ScePlayGoHandle handle) {
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) {
            pg_logf("scePlayGoClose rc=%d handle=%d", rc, handle);
            return rc;
        }
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) {
            pg_logf("scePlayGoClose rc=%d handle=%d", rc, handle);
            return rc;
        }
        if (g_playgo.open_count > 0) {
            g_playgo.open_count--;
        }
        pg_logf("scePlayGoClose rc=0 handle=%d open_count=%u", handle, g_playgo.open_count);
        return SCE_OK;
    }

    /*
     * Report the locus of each requested chunk.
     *
     * This follows the observed validation pattern of the original wrapper:
     * the number of requested entries must be a valid API list size, and every
     * chunk id must be in the configured package set. Valid chunks are reported
     * as already available locally at fast speed.
     */
    PRX_INTERFACE int32_t scePlayGoGetLocus(ScePlayGoHandle handle, const ScePlayGoChunkId* chunkIds, uint32_t numberOfEntries, ScePlayGoLocus* outLoci) {
        int32_t rc = pg_require_initialized();
        uint32_t badIndex = 0;
        ScePlayGoChunkId badChunkId = 0;
        const uint32_t chunkCount = g_playgo.chunk_count;
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (chunkIds == NULL || outLoci == NULL) { rc = SCE_PLAYGO_ERROR_BAD_POINTER; goto done; }
        if (numberOfEntries == 0) { rc = SCE_OK; goto done; }
        rc = pg_validate_chunk_list(chunkIds, numberOfEntries, &badIndex, &badChunkId);
        if (rc != SCE_OK) goto done;
        for (uint32_t i = 0; i < numberOfEntries; ++i) {
            outLoci[i] = SCE_PLAYGO_LOCUS_LOCAL_FAST;
        }
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoGetLocus rc=%d handle=%d entries=%u first_chunk=%u last_chunk=%u first_locus=%d bad_chunk=%u bad_index=%u chunk_count=%u",
            rc,
            handle,
            numberOfEntries,
            pg_first_chunk_id(chunkIds, numberOfEntries),
            pg_last_chunk_id(chunkIds, numberOfEntries),
            (rc == SCE_OK && outLoci != NULL && numberOfEntries > 0) ? outLoci[0] : -1,
            pg_log_bad_chunk_id(rc, badChunkId),
            pg_log_bad_chunk_index(rc, badIndex),
            chunkCount);
        return rc;
    }

    /*
     * Accept a caller-provided todo list without creating pending work.
     *
     * Titles may use this API as part of their normal PlayGo bookkeeping. In this
     * stub implementation all content is already available, so the request is only
     * validated and then discarded. Subsequent GetToDoList calls therefore still
     * report an empty list.
     */
    PRX_INTERFACE int32_t scePlayGoSetToDoList(ScePlayGoHandle handle, const ScePlayGoToDo* todoList, uint32_t numberOfEntries) {
        int32_t rc = pg_require_initialized();
        uint32_t badIndex = 0;
        ScePlayGoChunkId badChunkId = 0;
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (todoList == NULL) { rc = SCE_PLAYGO_ERROR_BAD_POINTER; goto done; }
        if (!pg_valid_count(numberOfEntries)) { rc = SCE_PLAYGO_ERROR_BAD_SIZE; goto done; }
        for (uint32_t i = 0; i < numberOfEntries; ++i) {
            ScePlayGoChunkId id = todoList[i].chunkId;
            if (id >= (ScePlayGoChunkId)SCE_PLAYGO_CHUNK_INDEX_MAX) { badIndex = i; badChunkId = id; rc = SCE_PLAYGO_ERROR_BAD_CHUNK_ID; goto done; }
            if ((uint32_t)id > g_playgo.max_chunk_id) { badIndex = i; badChunkId = id; rc = SCE_PLAYGO_ERROR_BAD_CHUNK_ID; goto done; }
            if (!g_playgo.chunk_valid[id]) { badIndex = i; badChunkId = id; rc = SCE_PLAYGO_ERROR_PROHIBIT_CHUNK_ID; goto done; }
        }
        g_playgo.todo_count = 0;
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoSetToDoList rc=%d handle=%d entries=%u first_chunk=%u first_locus=%d stored_entries=%u bad_chunk=%u bad_index=%u",
            rc,
            handle,
            numberOfEntries,
            (todoList != NULL && numberOfEntries > 0) ? (unsigned)todoList[0].chunkId : 0u,
            (todoList != NULL && numberOfEntries > 0) ? todoList[0].locus : -1,
            g_playgo.todo_count,
            pg_log_bad_chunk_id(rc, badChunkId),
            pg_log_bad_chunk_index(rc, badIndex));
        return rc;
    }

    /*
     * Return an empty todo list.
     *
     * In this stub there is never any remaining installation work. The function
     * still validates its output buffers so callers using the official API pattern
     * continue to behave as expected.
     */
    PRX_INTERFACE int32_t scePlayGoGetToDoList(ScePlayGoHandle handle, ScePlayGoToDo* outTodoList, uint32_t numberOfEntries, uint32_t* outEntries) {
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (outTodoList == NULL || outEntries == NULL) { rc = SCE_PLAYGO_ERROR_BAD_POINTER; goto done; }
        if (!pg_valid_count(numberOfEntries)) { rc = SCE_PLAYGO_ERROR_BAD_SIZE; goto done; }
        *outEntries = 0;
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoGetToDoList rc=%d handle=%d request_entries=%u returned_entries=%u first_chunk=%u first_locus=%d", rc, handle, numberOfEntries, (rc == SCE_OK && outEntries != NULL) ? *outEntries : 0u, 0u, -1);
        return rc;
    }

    /*
     * Ask PlayGo to prefetch chunks.
     *
     * The stub accepts the request and returns success. The title can issue these
     * requests freely without triggering background work or additional validation.
     */
    PRX_INTERFACE int32_t scePlayGoPrefetch(ScePlayGoHandle handle, const ScePlayGoChunkId* chunkIds, uint32_t numberOfEntries, ScePlayGoLocus minimumLocus) {
        int32_t rc = pg_require_initialized();
        uint32_t badIndex = 0;
        ScePlayGoChunkId badChunkId = 0;
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (numberOfEntries > 0) {
            rc = pg_validate_chunk_list(chunkIds, numberOfEntries, &badIndex, &badChunkId);
            if (rc != SCE_OK) goto done;
        }
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoPrefetch rc=%d handle=%d entries=%u first_chunk=%u last_chunk=%u minimum_locus=%d bad_chunk=%u bad_index=%u",
            rc,
            handle,
            numberOfEntries,
            pg_first_chunk_id(chunkIds, numberOfEntries),
            pg_last_chunk_id(chunkIds, numberOfEntries),
            minimumLocus,
            pg_log_bad_chunk_id(rc, badChunkId),
            pg_log_bad_chunk_index(rc, badIndex));
        return rc;
    }

    /*
     * Return an ETA for the requested chunks.
     *
     * The stub always reports that nothing remains to be installed, so ETA is 0.
     */
    PRX_INTERFACE int32_t scePlayGoGetEta(ScePlayGoHandle handle, const ScePlayGoChunkId* chunkIds, uint32_t numberOfEntries, ScePlayGoEta* outEta) {
        int32_t rc = pg_require_initialized();
        uint32_t badIndex = 0;
        ScePlayGoChunkId badChunkId = 0;
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (outEta == NULL) { rc = SCE_PLAYGO_ERROR_BAD_POINTER; goto done; }
        if (numberOfEntries > 0) {
            rc = pg_validate_chunk_list(chunkIds, numberOfEntries, &badIndex, &badChunkId);
            if (rc != SCE_OK) goto done;
        }
        *outEta = 0;
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoGetEta rc=%d handle=%d entries=%u first_chunk=%u eta=%lld bad_chunk=%u bad_index=%u",
            rc,
            handle,
            numberOfEntries,
            pg_first_chunk_id(chunkIds, numberOfEntries),
            (long long)((rc == SCE_OK && outEta != NULL) ? *outEta : -1),
            pg_log_bad_chunk_id(rc, badChunkId),
            pg_log_bad_chunk_index(rc, badIndex));
        return rc;
    }

    /*
     * Set the install speed for this handle.
     *
     * The value is stored in memory and later returned by GetInstallSpeed. No real
     * installation scheduling happens.
     */
    PRX_INTERFACE int32_t scePlayGoSetInstallSpeed(ScePlayGoHandle handle, ScePlayGoInstallSpeed speed) {
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (speed < SCE_PLAYGO_INSTALL_SPEED_SUSPENDED || speed > SCE_PLAYGO_INSTALL_SPEED_FULL) { rc = SCE_PLAYGO_ERROR_BAD_SPEED; goto done; }
        g_playgo.install_speed = speed;
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoSetInstallSpeed rc=%d handle=%d speed=%d", rc, handle, speed);
        return rc;
    }

    /*
     * Return the current install speed remembered by the stub.
     */
    PRX_INTERFACE int32_t scePlayGoGetInstallSpeed(ScePlayGoHandle handle, ScePlayGoInstallSpeed* speed) {
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (speed == NULL) { rc = SCE_PLAYGO_ERROR_BAD_POINTER; goto done; }
        *speed = g_playgo.install_speed;
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoGetInstallSpeed rc=%d handle=%d speed=%d", rc, handle, (rc == SCE_OK && speed != NULL) ? *speed : -1);
        return rc;
    }

    /*
     * Override the active language mask.
     *
     * This allows a title to test language-specific optional chunk paths while the
     * rest of the stub continues to report that those chunks are already available.
     */
    PRX_INTERFACE int32_t scePlayGoSetLanguageMask(ScePlayGoHandle handle, ScePlayGoLanguageMask languageMask) {
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        g_playgo.language_mask = languageMask;
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoSetLanguageMask rc=%d handle=%d mask=0x%016llx", rc, handle, (unsigned long long)languageMask);
        return rc;
    }

    /*
     * Return the current language mask.
     */
    PRX_INTERFACE int32_t scePlayGoGetLanguageMask(ScePlayGoHandle handle, ScePlayGoLanguageMask* outLanguageMask) {
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (outLanguageMask == NULL) { rc = SCE_PLAYGO_ERROR_INVALID_ARGUMENT; goto done; }
        *outLanguageMask = g_playgo.language_mask;
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoGetLanguageMask rc=%d handle=%d mask=0x%016llx", rc, handle, (unsigned long long)((rc == SCE_OK && outLanguageMask != NULL) ? *outLanguageMask : 0ULL));
        return rc;
    }

    /*
     * Return chunk progress.
     *
     * The stub reports a fixed fully-complete progress state. Callers that only
     * care whether installation finished will treat 100/100 as complete.
     */
    PRX_INTERFACE int32_t scePlayGoGetProgress(ScePlayGoHandle handle, const ScePlayGoChunkId* chunkIds, uint32_t numberOfEntries, ScePlayGoProgress* outProgress) {
        int32_t rc = pg_require_initialized();
        uint32_t badIndex = 0;
        ScePlayGoChunkId badChunkId = 0;
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (outProgress == NULL) { rc = SCE_PLAYGO_ERROR_BAD_POINTER; goto done; }
        if (numberOfEntries > 0) {
            rc = pg_validate_chunk_list(chunkIds, numberOfEntries, &badIndex, &badChunkId);
            if (rc != SCE_OK) goto done;
        }
        outProgress->progressSize = 100;
        outProgress->totalSize = 100;
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoGetProgress rc=%d handle=%d entries=%u first_chunk=%u progress=%llu total=%llu bad_chunk=%u bad_index=%u",
            rc,
            handle,
            numberOfEntries,
            pg_first_chunk_id(chunkIds, numberOfEntries),
            (unsigned long long)((rc == SCE_OK && outProgress != NULL) ? outProgress->progressSize : 0ULL),
            (unsigned long long)((rc == SCE_OK && outProgress != NULL) ? outProgress->totalSize : 0ULL),
            pg_log_bad_chunk_id(rc, badChunkId),
            pg_log_bad_chunk_index(rc, badIndex));
        return rc;
    }

    /*
     * Return the chunk id list exposed by the stub.
     *
     * This follows the standard two-step query pattern used by list APIs:
     * callers may pass outChunkIdList == NULL to query the total count first,
     * then allocate storage and call again to receive the configured chunk ids.
     */
    PRX_INTERFACE int32_t scePlayGoGetChunkId(ScePlayGoHandle handle, ScePlayGoChunkId* outChunkIdList, uint32_t numberOfEntries, uint32_t* outEntries) {
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) {
            pg_logf("scePlayGoGetChunkId rc=%d handle=%d request_entries=%u returned_entries=%u first_chunk=%u last_chunk=%u", rc, handle, numberOfEntries, (rc == SCE_OK && outEntries != NULL) ? *outEntries : 0u, (rc == SCE_OK && outChunkIdList != NULL && outEntries != NULL && *outEntries > 0) ? (unsigned)outChunkIdList[0] : 0u, (rc == SCE_OK && outChunkIdList != NULL && outEntries != NULL && *outEntries > 0) ? (unsigned)outChunkIdList[*outEntries - 1] : 0u);
            return rc;
        }
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) {
            pg_logf("scePlayGoGetChunkId rc=%d handle=%d request_entries=%u returned_entries=%u first_chunk=%u last_chunk=%u", rc, handle, numberOfEntries, (rc == SCE_OK && outEntries != NULL) ? *outEntries : 0u, (rc == SCE_OK && outChunkIdList != NULL && outEntries != NULL && *outEntries > 0) ? (unsigned)outChunkIdList[0] : 0u, (rc == SCE_OK && outChunkIdList != NULL && outEntries != NULL && *outEntries > 0) ? (unsigned)outChunkIdList[*outEntries - 1] : 0u);
            return rc;
        }
        rc = pg_copy_chunk_ids(outChunkIdList, numberOfEntries, outEntries);
        pg_logf("scePlayGoGetChunkId rc=%d handle=%d request_entries=%u returned_entries=%u first_chunk=%u last_chunk=%u", rc, handle, numberOfEntries, (rc == SCE_OK && outEntries != NULL) ? *outEntries : 0u, (rc == SCE_OK && outChunkIdList != NULL && outEntries != NULL && *outEntries > 0) ? (unsigned)outChunkIdList[0] : 0u, (rc == SCE_OK && outChunkIdList != NULL && outEntries != NULL && *outEntries > 0) ? (unsigned)outChunkIdList[*outEntries - 1] : 0u);
        return rc;
    }

    /*
     * Return the currently available optional chunk set.
     *
     * The stub reports all bits set for every optional chunk category. This is the
     * most permissive answer and lets titles proceed as if all optional content is
     * already present.
     */
    PRX_INTERFACE int32_t scePlayGoGetOptionalChunk(ScePlayGoHandle handle, ScePlayGoOptionalChunkType type, ScePlayGoOptionalChunk* option) {
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (option == NULL) { rc = SCE_PLAYGO_ERROR_BAD_POINTER; goto done; }
        (void)type;
        option->bitmask = ~0ULL;
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoGetOptionalChunk rc=%d handle=%d type=%d value=0x%016llx", rc, handle, type, (unsigned long long)((rc == SCE_OK && option != NULL) ? option->bitmask : 0ULL));
        return rc;
    }

    /*
     * Request prefetch of an optional chunk set.
     *
     * The stub validates inputs and succeeds without scheduling any work because the
     * requested optional chunks are already available.
     */
    PRX_INTERFACE int32_t scePlayGoPrefetchOptionalChunk(ScePlayGoHandle handle, ScePlayGoOptionalChunkType type, const ScePlayGoOptionalChunk* option) {
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (option == NULL) { rc = SCE_PLAYGO_ERROR_BAD_POINTER; goto done; }
        if (!pg_valid_optional_type(type)) { rc = SCE_PLAYGO_ERROR_BAD_OPTIONAL_TYPE; goto done; }
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoPrefetchOptionalChunk rc=%d handle=%d type=%d requested=0x%016llx", rc, handle, type, pg_optional_mask_value(option));
        return rc;
    }

    /*
     * Return the installed chunk list.
     *
     * The installed set equals the full chunk list because the stub treats every
     * configured chunk as already present. The same count-only and bounded-copy
     * pattern as GetChunkId is used here so callers can probe for the size first.
     */
    PRX_INTERFACE int32_t scePlayGoGetInstallChunkId(ScePlayGoHandle handle, ScePlayGoChunkId* outChunkIdList, uint32_t numberOfEntries, uint32_t* outEntries) {
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) {
            pg_logf("scePlayGoGetInstallChunkId rc=%d handle=%d request_entries=%u returned_entries=%u first_chunk=%u last_chunk=%u", rc, handle, numberOfEntries, (rc == SCE_OK && outEntries != NULL) ? *outEntries : 0u, (rc == SCE_OK && outChunkIdList != NULL && outEntries != NULL && *outEntries > 0) ? (unsigned)outChunkIdList[0] : 0u, (rc == SCE_OK && outChunkIdList != NULL && outEntries != NULL && *outEntries > 0) ? (unsigned)outChunkIdList[*outEntries - 1] : 0u);
            return rc;
        }
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) {
            pg_logf("scePlayGoGetInstallChunkId rc=%d handle=%d request_entries=%u returned_entries=%u first_chunk=%u last_chunk=%u", rc, handle, numberOfEntries, (rc == SCE_OK && outEntries != NULL) ? *outEntries : 0u, (rc == SCE_OK && outChunkIdList != NULL && outEntries != NULL && *outEntries > 0) ? (unsigned)outChunkIdList[0] : 0u, (rc == SCE_OK && outChunkIdList != NULL && outEntries != NULL && *outEntries > 0) ? (unsigned)outChunkIdList[*outEntries - 1] : 0u);
            return rc;
        }
        rc = pg_copy_chunk_ids(outChunkIdList, numberOfEntries, outEntries);
        pg_logf("scePlayGoGetInstallChunkId rc=%d handle=%d request_entries=%u returned_entries=%u first_chunk=%u last_chunk=%u", rc, handle, numberOfEntries, (rc == SCE_OK && outEntries != NULL) ? *outEntries : 0u, (rc == SCE_OK && outChunkIdList != NULL && outEntries != NULL && *outEntries > 0) ? (unsigned)outChunkIdList[0] : 0u, (rc == SCE_OK && outChunkIdList != NULL && outEntries != NULL && *outEntries > 0) ? (unsigned)outChunkIdList[*outEntries - 1] : 0u);
        return rc;
    }

    /*
     * Return which optional chunks are supported by the title.
     *
     * The stub mirrors GetOptionalChunk and exposes all bits as supported.
     */
    PRX_INTERFACE int32_t scePlayGoGetSupportedOptionalChunk(ScePlayGoHandle handle, ScePlayGoOptionalChunkType type, ScePlayGoOptionalChunk* option) {
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (option == NULL) { rc = SCE_PLAYGO_ERROR_BAD_POINTER; goto done; }
        (void)type;
        option->bitmask = ~0ULL;
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoGetSupportedOptionalChunk rc=%d handle=%d type=%d value=0x%016llx", rc, handle, type, (unsigned long long)((rc == SCE_OK && option != NULL) ? option->bitmask : 0ULL));
        return rc;
    }

    /*
     * Request an optional chunk.
     *
     * This shares the same no-op success behavior as PrefetchOptionalChunk.
     */
    PRX_INTERFACE int32_t scePlayGoRequestOptionalChunk(ScePlayGoHandle handle, ScePlayGoOptionalChunkType type, const ScePlayGoOptionalChunk* option) {
        int32_t rc = scePlayGoPrefetchOptionalChunk(handle, type, option);
        pg_logf("scePlayGoRequestOptionalChunk rc=%d handle=%d type=%d requested=0x%016llx", rc, handle, type, pg_optional_mask_value(option));
        return rc;
    }

    /*
     * Ask the service to advance to the next chunk.
     *
     * The stub only validates that reserved is NULL and then returns success.
     */
    PRX_INTERFACE int32_t scePlayGoRequestNextChunk(ScePlayGoHandle handle, const void* reserved) {
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (reserved != NULL) { rc = SCE_PLAYGO_ERROR_INVALID_ARGUMENT; goto done; }
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoRequestNextChunk rc=%d handle=%d reserved=%p", rc, handle, reserved);
        return rc;
    }

    /*
     * Write a tiny snapshot file.
     *
     * The original library accepts a NULL filename and falls back to an internal
     * default path. The stub mirrors that behavior by resolving NULL to a stable
     * file in /app0 and then writing a short human-readable summary there.
     */
    PRX_INTERFACE int32_t scePlayGoSnapshot(ScePlayGoHandle handle, const char* filename) {
        int32_t rc = pg_require_initialized();
        const char* resolvedFilename = pg_snapshot_filename_or_default(filename);
        FILE* fp;
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        fp = fopen(resolvedFilename, "wb");
        if (fp == NULL) {
            rc = SCE_PLAYGO_ERROR_EPERM;
            goto done;
        }
        fprintf(fp,
            "PlayGo PRX stub\n"
            "status=ok\n"
            "chunk_count=%u\n"
            "scenario_count=%u\n",
            g_playgo.chunk_count,
            g_playgo.scenario_count);
        fclose(fp);
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoSnapshot rc=%d handle=%d filename=%s chunk_count=%u scenario_count=%u", rc, handle, resolvedFilename, g_playgo.chunk_count, g_playgo.scenario_count);
        return rc;
    }

    /*
     * Return which disc would be required for the requested optional chunk.
     *
     * This implementation always reports 0, meaning no extra disc is needed.
     */
    PRX_INTERFACE int32_t scePlayGoGetRequiredDisc(ScePlayGoHandle handle, ScePlayGoOptionalChunkType type, const ScePlayGoOptionalChunk* option, uint32_t* outRequiredDisc) {
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (option == NULL || outRequiredDisc == NULL) { rc = SCE_PLAYGO_ERROR_BAD_POINTER; goto done; }
        if (!pg_valid_optional_type(type)) { rc = SCE_PLAYGO_ERROR_BAD_OPTIONAL_TYPE; goto done; }
        *outRequiredDisc = 0;
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoGetRequiredDisc rc=%d handle=%d type=%d requested=0x%016llx required_disc=%u", rc, handle, type, pg_optional_mask_value(option), (rc == SCE_OK && outRequiredDisc != NULL) ? *outRequiredDisc : 0u);
        return rc;
    }

    /*
     * Requires the first argument to be zero and rejects SUSPENDED with EPERM. 
     * The stub remembers the accepted speed globally so GetInstallSpeed returns the same value later.
     */
    PRX_INTERFACE int32_t scePlayGoSetInstallSpeed2(int32_t reservedMustBeZero, uint32_t speed) {
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) goto done;
        if (reservedMustBeZero != 0) { rc = SCE_PLAYGO_ERROR_INVALID_ARGUMENT; goto done; }
        if (speed > SCE_PLAYGO_INSTALL_SPEED_FULL) { rc = SCE_PLAYGO_ERROR_BAD_SPEED; goto done; }
        if (speed == SCE_PLAYGO_INSTALL_SPEED_SUSPENDED) { rc = SCE_PLAYGO_ERROR_EPERM; goto done; }
        g_playgo.install_speed = (ScePlayGoInstallSpeed)speed;
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoSetInstallSpeed2 rc=%d reserved=%d speed=%u current_speed=%d", rc, reservedMustBeZero, speed, g_playgo.install_speed);
        return rc;
    }

} /* extern "C" */
