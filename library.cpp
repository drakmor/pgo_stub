#define LIBRARY_IMPL 1
#include "library.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ORBIS__) || defined(__PROSPERO__)
#include <sys/_pthreadtypes.h>
/* Minimal libkernel imports, using the SDK's opaque mutex type. */
extern "C" {
    int scePthreadMutexLock(pthread_mutex_t* mutex);
    int scePthreadMutexUnlock(pthread_mutex_t* mutex);
    int scePthreadMutexDestroy(pthread_mutex_t* mutex);
}
static pthread_mutex_t g_playgo_mutex = NULL;
static int pg_lock(void) { return scePthreadMutexLock(&g_playgo_mutex); }
static void pg_unlock(void) { (void)scePthreadMutexUnlock(&g_playgo_mutex); }
static int pg_destroy_mutex(void) { return scePthreadMutexDestroy(&g_playgo_mutex); }
#else
#include <pthread.h>
static pthread_mutex_t g_playgo_mutex = PTHREAD_MUTEX_INITIALIZER;
static int pg_lock(void) { return pthread_mutex_lock(&g_playgo_mutex); }
static void pg_unlock(void) { (void)pthread_mutex_unlock(&g_playgo_mutex); }
static int pg_destroy_mutex(void) { return pthread_mutex_destroy(&g_playgo_mutex); }
#endif

/* The mutex lives outside state cleared by Initialize/Terminate. */
class PlayGoGuard {
public:
    PlayGoGuard() : status_(pg_lock()) {}
    ~PlayGoGuard() { if (status_ == 0) pg_unlock(); }
    bool locked() const { return status_ == 0; }
    PlayGoGuard(const PlayGoGuard&) = delete;
    PlayGoGuard& operator=(const PlayGoGuard&) = delete;
private:
    int status_;
};

#ifndef PLAYGO_ENABLE_LOGGING
#define PLAYGO_ENABLE_LOGGING 0
#endif

static constexpr uint32_t kDefaultChunkCount = 1000;
static constexpr uint32_t kConfigLineSize = 65536;
static constexpr const char* kPlayGoStubVersion = "0.6";

struct PlayGoState {
    int initialized;
    uint32_t open_count;
    ScePlayGoHandle handle;
    ScePlayGoInstallSpeed install_speed;
    ScePlayGoLanguageMask language_mask;
    ScePlayGoLanguageMask available_language_mask;
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
/* Scratch bitset used only while holding the state mutex. */
static uint8_t g_chunk_seen[(SCE_PLAYGO_CHUNK_INDEX_MAX + 7u) / 8u];
static const char kConfigWhitespace[] = " \t\r\n\v\f";

static const char* const kLanguageNames[] = {
    "japanese",
    "english_us",
    "french",
    "spanish",
    "german",
    "italian",
    "dutch",
    "portuguese_pt",
    "russian",
    "korean",
    "chinese_t",
    "chinese_s",
    "finnish",
    "swedish",
    "danish",
    "norwegian",
    "polish",
    "portuguese_br",
    "english_gb",
    "turkish",
    "spanish_la",
    "arabic",
    "french_ca",
    "czech",
    "hungarian",
    "greek",
    "romanian",
    "thai",
    "vietnamese",
    "indonesian",
};

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
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
        pg_log_reset();
        pg_logf("module_start: module loaded version=%s args=%zu argp=%p", kPlayGoStubVersion, args, argp);
        return 0;
    }

    int module_stop(size_t args, const void* argp) {
        {
            PlayGoGuard guard;
            if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
            pg_logf("module_stop: module unloading args=%zu argp=%p", args, argp);
        }
        return pg_destroy_mutex() == 0 ? SCE_OK : SCE_PLAYGO_ERROR_FATAL;
    }
}

/*
 * Small logging helpers.
 *
 * The log should explain what the caller asked for and what the stub returned
 * without requiring a debugger. These helpers keep the per-function log lines
 * short and consistent.
 */
static unsigned long long pg_optional_mask_value(int32_t rc, const ScePlayGoOptionalChunk* option) {
    return (rc == SCE_OK && option != NULL) ? (unsigned long long)option->bitmask : 0ULL;
}

static unsigned pg_first_chunk_id(int32_t rc, const ScePlayGoChunkId* chunkIds, uint32_t numberOfEntries) {
    return (rc == SCE_OK && chunkIds != NULL && numberOfEntries > 0) ? (unsigned)chunkIds[0] : 0u;
}

static unsigned pg_last_chunk_id(int32_t rc, const ScePlayGoChunkId* chunkIds, uint32_t numberOfEntries) {
    return (rc == SCE_OK && chunkIds != NULL && numberOfEntries > 0) ? (unsigned)chunkIds[numberOfEntries - 1] : 0u;
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

static int pg_parse_unsigned_decimal(const char* text, char** end, unsigned long* outValue) {
    text += strspn(text, kConfigWhitespace);
    if (*text < '0' || *text > '9') {
        return 0;
    }
    int savedErrno = errno;
    errno = 0;
    unsigned long value = strtoul(text, end, 10);
    int parseErrno = errno;
    errno = savedErrno;
    if (*end == text || parseErrno == ERANGE) {
        return 0;
    }
    *outValue = value;
    return 1;
}

static int pg_parse_chunk_count_line(const char* line, uint32_t* outChunkCount) {
    char* end;
    unsigned long value;

    if (!pg_parse_unsigned_decimal(line, &end, &value) || value == 0) {
        return 0;
    }
    end += strspn(end, kConfigWhitespace);
    if (*end != '\0') {
        return 0;
    }
    if (value > SCE_PLAYGO_CHUNK_INDEX_MAX) {
        value = SCE_PLAYGO_CHUNK_INDEX_MAX;
    }
    *outChunkCount = (uint32_t)value;
    return 1;
}

static int pg_parse_scenario_count_line(const char* line, uint32_t* outCount) {
    char* end;
    unsigned long value;
    if (!pg_parse_unsigned_decimal(line, &end, &value) || value > SCE_PLAYGO_MAX_SCENARIO) {
        return 0;
    }
    end += strspn(end, kConfigWhitespace);
    if (*end != '\0') {
        return 0;
    }
    *outCount = (uint32_t)value;
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
        cursor += strspn(cursor, " \t\r\n\v\f,");
        if (*cursor == '\0') {
            break;
        }

        unsigned long value;
        if (!pg_parse_unsigned_decimal(cursor, &end, &value) || value >= SCE_PLAYGO_CHUNK_INDEX_MAX) {
            return 0;
        }
        if (!pg_add_chunk_id((ScePlayGoChunkId)value)) {
            return 0;
        }
        ++added;

        cursor = end;
        cursor += strspn(cursor, kConfigWhitespace);
        if (*cursor != '\0' && *cursor != ',') {
            return 0;
        }
    }

    return added > 0;
}

static char* pg_trim_config_token(char* text) {
    char* end;

    text += strspn(text, kConfigWhitespace);
    end = text + strlen(text);
    while (end > text && strchr(kConfigWhitespace, end[-1]) != NULL) {
        --end;
    }
    *end = '\0';
    return text;
}

static int pg_parse_language_id(char* token, int32_t* outSystemLanguage) {
    char* end;
    unsigned long value;
    size_t i;

    if (token[0] >= '0' && token[0] <= '9') {
        if (!pg_parse_unsigned_decimal(token, &end, &value) || *end != '\0' || value >= 48u) {
            return 0;
        }
        *outSystemLanguage = (int32_t)value;
        return 1;
    }

    for (i = 0; i < sizeof(kLanguageNames) / sizeof(kLanguageNames[0]); ++i) {
        if (strcasecmp(token, kLanguageNames[i]) == 0) {
            *outSystemLanguage = (int32_t)i;
            return 1;
        }
    }
    return 0;
}

static int pg_parse_language_hex_mask(char* line, ScePlayGoLanguageMask* outMask) {
    char* end;
    unsigned long long value;
    int parseErrno;
    int savedErrno = errno;

    errno = 0;
    value = strtoull(line, &end, 16);
    parseErrno = errno;
    errno = savedErrno;
    if (end <= line + 2 || *end != '\0' || parseErrno == ERANGE) {
        return 0;
    }
    *outMask = (ScePlayGoLanguageMask)value;
    return 1;
}

static int pg_parse_language_list(char* line, ScePlayGoLanguageMask* outMask) {
    char* context = NULL;
    char* token;
    size_t lineLength = strlen(line);
    ScePlayGoLanguageMask mask = 0;

    if (lineLength == 0 || line[0] == ',' || line[lineLength - 1] == ',' || strstr(line, ",,") != NULL) {
        return 0;
    }

    for (token = strtok_r(line, ",", &context); token != NULL; token = strtok_r(NULL, ",", &context)) {
        int32_t systemLanguage;

        token = pg_trim_config_token(token);
        if (*token == '\0' || !pg_parse_language_id(token, &systemLanguage)) {
            return 0;
        }
        mask |= scePlayGoConvertLanguage(systemLanguage);
    }

    *outMask = mask;
    return 1;
}

static int pg_parse_language_config_line(char* line, ScePlayGoLanguageMask* outMask) {
    char* value = pg_trim_config_token(line);

    if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        return pg_parse_language_hex_mask(value, outMask);
    }
    return pg_parse_language_list(value, outMask);
}

/*
 * Reset the whole global stub state to defaults.
 *
 * Defaults make all languages and scenarios available and installation complete.
 * The caller must still initialize the library and open a handle.
 */
static void pg_set_defaults(void) {
    memset(&g_playgo, 0, sizeof(g_playgo));
    g_playgo.handle = 1;
    g_playgo.install_speed = SCE_PLAYGO_INSTALL_SPEED_TRICKLE;
    g_playgo.language_mask = SCE_PLAYGO_LANGUAGE_MASK_ALL;
    g_playgo.available_language_mask = SCE_PLAYGO_LANGUAGE_MASK_ALL;
    g_playgo.scenario_count = SCE_PLAYGO_MAX_SCENARIO;
    g_playgo.config_loaded_from_file = 0;
    pg_reset_chunk_ids(kDefaultChunkCount);
}

/*
 * Load /app0/playgo_stub.dat.
 *
 * File format:
 *   line 1: chunk count, or comma-separated valid chunk id list
 *   line 2: scenario count (optional)
 *   line 3: language mask (0x...) or comma-separated language id/name list (optional)
 *
 * If the file is missing or the chunk line is malformed, the stub falls back to
 * 1000 chunks. An invalid language line falls back to all languages independently.
 * Chunk ids are valid in the half-open range [0, chunk_count), so a 1000-chunk
 * package exposes ids 0..999 and reports BAD_CHUNK_ID for 1000. If line 1
 * contains commas, it is treated as the exact valid chunk id list instead.
 */
/* Read one physical line; discard oversized or NUL-containing lines. */
static int pg_read_config_line(FILE* fp) {
    size_t length = 0;
    int invalid = 0;
    int ch;
    while ((ch = fgetc(fp)) != EOF) {
        if (ch == '\0') {
            invalid = 1;
        }
        if (length < sizeof(g_config_line) - 1) {
            g_config_line[length++] = (char)ch;
        } else {
            invalid = 1;
        }
        if (ch == '\n') {
            break;
        }
    }
    g_config_line[length] = '\0';
    if (ferror(fp)) {
        return -1;
    }
    if (length == 0 && ch == EOF) {
        return 0;
    }
    return invalid ? -1 : 1;
}

static void pg_load_config(void) {
    FILE* fp;
    char* line = g_config_line;
    uint32_t chunkCount;
    int parsedChunkConfig = 0;
    ScePlayGoLanguageMask languageMask;

    pg_reset_chunk_ids(kDefaultChunkCount);
    g_playgo.scenario_count = SCE_PLAYGO_MAX_SCENARIO;
    g_playgo.language_mask = SCE_PLAYGO_LANGUAGE_MASK_ALL;
    g_playgo.available_language_mask = SCE_PLAYGO_LANGUAGE_MASK_ALL;
    g_playgo.config_loaded_from_file = 0;

    fp = fopen("/app0/playgo_stub.dat", "rb");
    if (fp == NULL) {
        pg_logf("config: using defaults chunk_count=%u scenario_count=%u available_language_mask=0x%016llx current_language_mask=0x%016llx",
            g_playgo.chunk_count,
            g_playgo.scenario_count,
            (unsigned long long)g_playgo.available_language_mask,
            (unsigned long long)g_playgo.language_mask);
        return;
    }

    if (pg_read_config_line(fp) == 1) {
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

    if (pg_read_config_line(fp) == 1) {
        (void)pg_parse_scenario_count_line(line, &g_playgo.scenario_count);
    }

    if (pg_read_config_line(fp) == 1) {
        if (pg_parse_language_config_line(line, &languageMask)) {
            g_playgo.available_language_mask = languageMask;
            g_playgo.language_mask = languageMask;
        } else {
            pg_logf("config: invalid language configuration, using all languages");
        }
    }

    fclose(fp);
    g_playgo.config_loaded_from_file = 1;
    pg_logf("config: loaded chunk_count=%u scenario_count=%u available_language_mask=0x%016llx current_language_mask=0x%016llx",
        g_playgo.chunk_count,
        g_playgo.scenario_count,
        (unsigned long long)g_playgo.available_language_mask,
        (unsigned long long)g_playgo.language_mask);
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

static int pg_valid_locus(ScePlayGoLocus locus) {
    return locus == SCE_PLAYGO_LOCUS_NOT_DOWNLOADED ||
        locus == SCE_PLAYGO_LOCUS_LOCAL_SLOW || locus == SCE_PLAYGO_LOCUS_LOCAL_FAST;
}

/* Call only after validating the id's range. */
static int pg_mark_chunk_seen(ScePlayGoChunkId id) {
    uint8_t mask = (uint8_t)(1u << (id & 7u));
    uint8_t* bits = &g_chunk_seen[id >> 3];
    if ((*bits & mask) != 0) return 0;
    *bits |= mask;
    return 1;
}

/*
 * Validate optional chunk selector type.
 */
static int pg_valid_optional_type(ScePlayGoOptionalChunkType type) {
    return type >= SCE_PLAYGO_OPTIONAL_CHUNK_TYPE_LANGUAGE &&
        type <= SCE_PLAYGO_OPTIONAL_CHUNK_TYPE_OBSERVED_RESERVED;
}

static int pg_language_mask_is_available(ScePlayGoLanguageMask languageMask) {
    return (languageMask & ~g_playgo.available_language_mask) == 0;
}

static ScePlayGoScenarioMask pg_available_scenario_mask(void) {
    return (1ULL << g_playgo.scenario_count) - 1ULL;
}

static int32_t pg_validate_optional_mask(
    ScePlayGoOptionalChunkType type,
    const ScePlayGoOptionalChunk* option) {
    if (type == SCE_PLAYGO_OPTIONAL_CHUNK_TYPE_LANGUAGE &&
        !pg_language_mask_is_available(option->languages)) {
        return SCE_PLAYGO_ERROR_BAD_LANG_CONFIGURATION;
    }
    if (type == SCE_PLAYGO_OPTIONAL_CHUNK_TYPE_SCENARIO &&
        (option->scenarios & ~pg_available_scenario_mask()) != 0) {
        return SCE_PLAYGO_ERROR_BAD_SCENARIO_CONFIGURATION;
    }
    return SCE_OK;
}

/* Called with the state mutex held by either optional-request export. */
static int32_t pg_request_optional_chunk(ScePlayGoHandle handle,
    ScePlayGoOptionalChunkType type, const ScePlayGoOptionalChunk* option) {
    int32_t rc = pg_require_initialized();
    if (rc != SCE_OK) return rc;
    rc = pg_require_handle(handle);
    if (rc != SCE_OK) return rc;
    if (option == NULL) return SCE_PLAYGO_ERROR_BAD_POINTER;
    if (!pg_valid_optional_type(type)) return SCE_PLAYGO_ERROR_BAD_OPTIONAL_TYPE;
    return pg_validate_optional_mask(type, option);
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

    memset(g_chunk_seen, 0, sizeof(g_chunk_seen));
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
        if (!pg_mark_chunk_seen(id)) {
            if (outBadIndex != NULL) *outBadIndex = i;
            if (outBadChunkId != NULL) *outBadChunkId = id;
            return SCE_PLAYGO_ERROR_BAD_CHUNK_ID;
        }
    }

    return SCE_OK;
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
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
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
        pg_logf("scePlayGoInitialize rc=0 version=%s chunk_count=%u scenario_count=%u config_source=%s available_language_mask=0x%016llx current_language_mask=0x%016llx install_speed=%d",
            kPlayGoStubVersion,
            g_playgo.chunk_count,
            g_playgo.scenario_count,
            g_playgo.config_loaded_from_file ? "file" : "defaults",
            (unsigned long long)g_playgo.available_language_mask,
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
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
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
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
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
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
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
     * Empty lists are accepted as a compatibility extension to the original
     * wrapper, which requires non-NULL arrays and a positive count. Nonempty
     * lists still require valid buffers and chunk ids in the configured package
     * set. Valid chunks are reported as already available locally at fast speed.
     */
    PRX_INTERFACE int32_t scePlayGoGetLocus(ScePlayGoHandle handle, const ScePlayGoChunkId* chunkIds, uint32_t numberOfEntries, ScePlayGoLocus* outLoci) {
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
        int32_t rc = pg_require_initialized();
        uint32_t badIndex = 0;
        ScePlayGoChunkId badChunkId = 0;
        const uint32_t chunkCount = g_playgo.chunk_count;
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (numberOfEntries == 0) { rc = SCE_OK; goto done; }
        if (chunkIds == NULL || outLoci == NULL) { rc = SCE_PLAYGO_ERROR_BAD_POINTER; goto done; }
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
            pg_first_chunk_id(rc, chunkIds, numberOfEntries),
            pg_last_chunk_id(rc, chunkIds, numberOfEntries),
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
     * report an empty list. An empty input list also clears pending work and may
     * be NULL, as a compatibility extension to the original wrapper.
     */
    PRX_INTERFACE int32_t scePlayGoSetToDoList(ScePlayGoHandle handle, const ScePlayGoToDo* todoList, uint32_t numberOfEntries) {
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
        int32_t rc = pg_require_initialized();
        uint32_t badIndex = 0;
        ScePlayGoChunkId badChunkId = 0;
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (numberOfEntries == 0) {
            g_playgo.todo_count = 0;
            rc = SCE_OK;
            goto done;
        }
        if (todoList == NULL) { rc = SCE_PLAYGO_ERROR_BAD_POINTER; goto done; }
        if (!pg_valid_count(numberOfEntries)) { rc = SCE_PLAYGO_ERROR_BAD_SIZE; goto done; }
        memset(g_chunk_seen, 0, sizeof(g_chunk_seen));
        for (uint32_t i = 0; i < numberOfEntries; ++i) {
            ScePlayGoChunkId id = todoList[i].chunkId;
            if (id >= (ScePlayGoChunkId)SCE_PLAYGO_CHUNK_INDEX_MAX) { badIndex = i; badChunkId = id; rc = SCE_PLAYGO_ERROR_BAD_CHUNK_ID; goto done; }
            if ((uint32_t)id > g_playgo.max_chunk_id) { badIndex = i; badChunkId = id; rc = SCE_PLAYGO_ERROR_BAD_CHUNK_ID; goto done; }
            if (!g_playgo.chunk_valid[id]) { badIndex = i; badChunkId = id; rc = SCE_PLAYGO_ERROR_PROHIBIT_CHUNK_ID; goto done; }
            if (!pg_mark_chunk_seen(id)) { badIndex = i; badChunkId = id; rc = SCE_PLAYGO_ERROR_BAD_CHUNK_ID; goto done; }
            if (!pg_valid_locus(todoList[i].locus)) { rc = SCE_PLAYGO_ERROR_BAD_LOCUS; goto done; }
            if (todoList[i].reserved != 0) { rc = SCE_PLAYGO_ERROR_INVALID_ARGUMENT; goto done; }
        }
        g_playgo.todo_count = 0;
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoSetToDoList rc=%d handle=%d entries=%u first_chunk=%u first_locus=%d stored_entries=%u bad_chunk=%u bad_index=%u",
            rc,
            handle,
            numberOfEntries,
            (rc == SCE_OK && todoList != NULL && numberOfEntries > 0) ? (unsigned)todoList[0].chunkId : 0u,
            (rc == SCE_OK && todoList != NULL && numberOfEntries > 0) ? todoList[0].locus : -1,
            g_playgo.todo_count,
            pg_log_bad_chunk_id(rc, badChunkId),
            pg_log_bad_chunk_index(rc, badIndex));
        return rc;
    }

    /*
     * Return an empty todo list.
     *
     * In this stub there is never any remaining installation work. The function
     * accepts a NULL array when its capacity is zero as a compatibility extension.
     * The scalar outEntries output remains mandatory, including for empty lists.
     */
    PRX_INTERFACE int32_t scePlayGoGetToDoList(ScePlayGoHandle handle, ScePlayGoToDo* outTodoList, uint32_t numberOfEntries, uint32_t* outEntries) {
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (outEntries == NULL) { rc = SCE_PLAYGO_ERROR_BAD_POINTER; goto done; }
        if (numberOfEntries == 0) { *outEntries = 0; rc = SCE_OK; goto done; }
        if (outTodoList == NULL) { rc = SCE_PLAYGO_ERROR_BAD_POINTER; goto done; }
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
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
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
        if (!pg_valid_locus(minimumLocus)) { rc = SCE_PLAYGO_ERROR_BAD_LOCUS; goto done; }
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoPrefetch rc=%d handle=%d entries=%u first_chunk=%u last_chunk=%u minimum_locus=%d bad_chunk=%u bad_index=%u",
            rc,
            handle,
            numberOfEntries,
            pg_first_chunk_id(rc, chunkIds, numberOfEntries),
            pg_last_chunk_id(rc, chunkIds, numberOfEntries),
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
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
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
            pg_first_chunk_id(rc, chunkIds, numberOfEntries),
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
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
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
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
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
     * The requested set must be a subset of the languages exposed by the runtime
     * configuration.
     */
    PRX_INTERFACE int32_t scePlayGoSetLanguageMask(ScePlayGoHandle handle, ScePlayGoLanguageMask languageMask) {
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (!pg_language_mask_is_available(languageMask)) {
            rc = SCE_PLAYGO_ERROR_BAD_LANG_CONFIGURATION;
            goto done;
        }
        g_playgo.language_mask = languageMask;
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoSetLanguageMask rc=%d handle=%d requested=0x%016llx available=0x%016llx current=0x%016llx", rc, handle, (unsigned long long)languageMask, (unsigned long long)g_playgo.available_language_mask, (unsigned long long)g_playgo.language_mask);
        return rc;
    }

    /*
     * Return the current language mask.
     */
    PRX_INTERFACE int32_t scePlayGoGetLanguageMask(ScePlayGoHandle handle, ScePlayGoLanguageMask* outLanguageMask) {
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
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
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
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
            pg_first_chunk_id(rc, chunkIds, numberOfEntries),
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
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
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
     * Language queries return the active language selection. Scenario queries use the configured scenario mask;
     * the observed reserved category retains the all-bits-set behavior.
     */
    PRX_INTERFACE int32_t scePlayGoGetOptionalChunk(ScePlayGoHandle handle, ScePlayGoOptionalChunkType type, ScePlayGoOptionalChunk* option) {
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (option == NULL) { rc = SCE_PLAYGO_ERROR_BAD_POINTER; goto done; }
        if (!pg_valid_optional_type(type)) { rc = SCE_PLAYGO_ERROR_BAD_OPTIONAL_TYPE; goto done; }
        option->bitmask = (type == SCE_PLAYGO_OPTIONAL_CHUNK_TYPE_LANGUAGE)
            ? g_playgo.language_mask
            : (type == SCE_PLAYGO_OPTIONAL_CHUNK_TYPE_SCENARIO ? pg_available_scenario_mask() : ~0ULL);
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
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
        int32_t rc = pg_request_optional_chunk(handle, type, option);
        pg_logf("scePlayGoPrefetchOptionalChunk rc=%d handle=%d type=%d requested=0x%016llx", rc, handle, type, pg_optional_mask_value(rc, option));
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
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
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
     * Language queries return the configured availability boundary. Scenario queries use the configured
     * scenario mask; the observed reserved category retains all bits set.
     */
    PRX_INTERFACE int32_t scePlayGoGetSupportedOptionalChunk(ScePlayGoHandle handle, ScePlayGoOptionalChunkType type, ScePlayGoOptionalChunk* option) {
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (option == NULL) { rc = SCE_PLAYGO_ERROR_BAD_POINTER; goto done; }
        if (!pg_valid_optional_type(type)) { rc = SCE_PLAYGO_ERROR_BAD_OPTIONAL_TYPE; goto done; }
        option->bitmask = (type == SCE_PLAYGO_OPTIONAL_CHUNK_TYPE_LANGUAGE)
            ? g_playgo.available_language_mask
            : (type == SCE_PLAYGO_OPTIONAL_CHUNK_TYPE_SCENARIO ? pg_available_scenario_mask() : ~0ULL);
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
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
        int32_t rc = pg_request_optional_chunk(handle, type, option);
        pg_logf("scePlayGoRequestOptionalChunk rc=%d handle=%d type=%d requested=0x%016llx", rc, handle, type, pg_optional_mask_value(rc, option));
        return rc;
    }

    /*
     * Ask the service to advance to the next chunk.
     *
     * The stub only validates that reserved is NULL and then returns success.
     */
    PRX_INTERFACE int32_t scePlayGoRequestNextChunk(ScePlayGoHandle handle, const void* reserved) {
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
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
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
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
        {
            int writeResult = fprintf(fp,
                "PlayGo PRX stub\n"
                "version=%s\n"
                "status=ok\n"
                "chunk_count=%u\n"
                "scenario_count=%u\n",
                kPlayGoStubVersion,
                g_playgo.chunk_count,
                g_playgo.scenario_count);
            int closeResult = fclose(fp);
            rc = (writeResult >= 0 && closeResult == 0) ? SCE_OK : SCE_PLAYGO_ERROR_EPERM;
        }
    done:
        pg_logf("scePlayGoSnapshot rc=%d handle=%d filename=%s chunk_count=%u scenario_count=%u", rc, handle, rc == SCE_OK ? resolvedFilename : "(unavailable)", g_playgo.chunk_count, g_playgo.scenario_count);
        return rc;
    }

    /*
     * Return which disc would be required for the requested optional chunk.
     *
     * This implementation always reports 0, meaning no extra disc is needed.
     */
    PRX_INTERFACE int32_t scePlayGoGetRequiredDisc(ScePlayGoHandle handle, ScePlayGoOptionalChunkType type, const ScePlayGoOptionalChunk* option, uint32_t* outRequiredDisc) {
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (option == NULL || outRequiredDisc == NULL) { rc = SCE_PLAYGO_ERROR_BAD_POINTER; goto done; }
        if (!pg_valid_optional_type(type)) { rc = SCE_PLAYGO_ERROR_BAD_OPTIONAL_TYPE; goto done; }
        rc = pg_validate_optional_mask(type, option);
        if (rc != SCE_OK) goto done;
        *outRequiredDisc = 0;
        rc = SCE_OK;
    done:
        pg_logf("scePlayGoGetRequiredDisc rc=%d handle=%d type=%d requested=0x%016llx required_disc=%u", rc, handle, type, pg_optional_mask_value(rc, option), (rc == SCE_OK && outRequiredDisc != NULL) ? *outRequiredDisc : 0u);
        return rc;
    }

    /*
     * Requires the first argument to be zero and rejects SUSPENDED with EPERM. 
     * The stub remembers the accepted speed globally so GetInstallSpeed returns the same value later.
     */
    PRX_INTERFACE int32_t scePlayGoSetInstallSpeed2(int32_t reservedMustBeZero, uint32_t speed) {
        PlayGoGuard guard;
        if (!guard.locked()) return SCE_PLAYGO_ERROR_FATAL;
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
