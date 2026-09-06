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
static constexpr const char* kPlayGoStubVersion = "0.5";

struct PlayGoState {
    int initialized;
    uint32_t open_count;
    ScePlayGoHandle handle;
    ScePlayGoInstallSpeed install_speed;
    ScePlayGoLanguageMask language_mask;
    ScePlayGoLanguageMask available_language_mask;
    int console_language_fallback;
    int config_error;
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

/* Console system-language query; see README, "Language Selection and Fallback". */
extern "C" int32_t sceSystemServiceParamGetInt(int32_t paramId, int32_t* value);

/*
 * How long the stop toast counts down before the title is closed, matching the
 * hold unilibrary's gate uses so the two feel like the same event to a user.
 */
static constexpr uint32_t kConfigStopSeconds = 12;

/* How much of an offending config line is quoted back in the toast. */
static constexpr size_t kConfigExcerptSize = 64;

/*
 * Stopping the title goes through ShellCore, not through exit().
 *
 * A title cannot reach sceSystemServiceLoadExec from inside a module, so the
 * IPMI request that export makes is reproduced here: connect to SceLncService
 * and invoke its LoadExec command. ShellCore KILLS THE CALLER THE MOMENT IT
 * ACCEPTS, before it tries to launch anything, so the stop does not depend on
 * the replacement process starting - or existing. What LoadExec requires is a
 * non-empty path; it is never compared against any literal.
 *
 * The path named is the config file itself. It is guaranteed to exist, because
 * this path only runs when a line in it was read and rejected, and it can never
 * be spawned as a process, so there is no way for a handover to turn into a
 * relaunch of the title that just stopped.
 *
 * Being killed by ShellCore keeps the stop on a route ShellCore initiated and
 * understands, which exit() does not.
 */
#define PG_LNC_IPMI_SERVICE "SceLncService"
#define PG_LNC_CMD_LOADEXEC 0x3000Du
#define PG_LNC_QUIT_PATH "/app0/playgo_stub.dat"

/* Client storage sized as the firmware sizes its own IPMI clients. Guessing
 * down on a reversed ABI is the one direction that corrupts memory. */
#define PG_IPMI_CLIENT_STORAGE 0xf800u
#define PG_IPMI_REQUEST_BUFFER 0x200u

struct PgIpcBuffer {
    void* data;
    uint64_t size;
};

typedef int32_t (*PgIpmiClientConnect)(void*, uint64_t, uint64_t, int32_t*);
typedef int32_t (*PgIpmiClientInvoke)(void*, uint32_t, const PgIpcBuffer*, uint32_t, int32_t*, PgIpcBuffer*, uint32_t);

/*
 * Bound by mangled name: these are C++ entry points with no C declaration.
 *
 * WEAK ON PURPOSE. Linking the stub archive names libSceIpmi as a dependency
 * either way, so this is not about avoiding that; it is about the module that
 * answers. A title can be running against a substitute libSceIpmi that exports
 * a different set of symbols than the firmware one, and a strong reference
 * would turn that into a module that will not load. Weak leaves the pointers
 * NULL instead, the request is skipped, and the caller falls back to failing
 * scePlayGoInitialize - which still stops the title.
 */
extern "C" void pg_ipmi_client_config_ctor(void*) __asm__("_ZN4IPMI6Client6ConfigC1Ev") __attribute__((weak));
extern "C" int32_t pg_ipmi_client_create(void**, const void*, void*, void*) __asm__("_ZN4IPMI6Client6createEPPS0_PKNS0_6ConfigEPvS6_") __attribute__((weak));

static unsigned char g_ipmi_client_storage[PG_IPMI_CLIENT_STORAGE] __attribute__((aligned(16)));

/*
 * System notification request. The kernel takes a fixed 3120-byte block whose
 * message field starts at offset 45; everything else is zero for a plain text
 * toast.
 */
struct PgNotificationRequest {
    char unused[45];
    char message[3075];
};

extern "C" int sceKernelSendNotificationRequest(int target, PgNotificationRequest* req, unsigned long size, int blocking);
extern "C" int sceKernelUsleep(unsigned int microseconds);

/* Console system-language bit (1<<(63-code)); 0 (and *outCode=-1) if unavailable. */
static ScePlayGoLanguageMask pg_console_language_bit(int32_t* outCode) {
    int32_t lang = -1;
    if (sceSystemServiceParamGetInt(SCE_SYSTEM_SERVICE_PARAM_ID_LANG, &lang) != SCE_OK) {
        if (outCode != NULL) { *outCode = -1; }
        return 0;
    }
    if (outCode != NULL) { *outCode = lang; }
    return scePlayGoConvertLanguage(lang);
}

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

/* Put a line of text on screen. Toasts are the only channel a user sees. */
static void pg_notify(const char* body) {
    PgNotificationRequest req;

    memset(&req, 0, sizeof(req));
    /* snprintf, not strncpy: it is already imported from libSceLibcInternal,
     * and strncpy is the one libc symbol that would pull libc.prx in as a whole
     * extra NEEDED module for a stub that has to load inside any title. */
    snprintf(req.message, sizeof(req.message), "%s", body);
    (void)sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/*
 * Ask ShellCore to replace this title. Returns only if it refused.
 */
static void pg_request_title_stop(void) {
    unsigned char config[0x180] __attribute__((aligned(16)));
    static const char kService[16] = PG_LNC_IPMI_SERVICE;
    /* Buffer 0 points AT the argument length: ShellCore reads four bytes from
     * that address rather than taking the value. */
    uint32_t argsLength = 0;
    PgIpcBuffer bufs[3];
    void* client = NULL;
    int32_t serviceResult = 0;
    void** vtable;
    PgIpmiClientConnect connect;
    PgIpmiClientInvoke invoke;
    int32_t rc;
    size_t i;

    if (pg_ipmi_client_config_ctor == NULL || pg_ipmi_client_create == NULL) {
        pg_logf("stop: libSceIpmi is not loaded in this title, cannot ask ShellCore");
        return;
    }

    memset(config, 0, sizeof(config));
    pg_ipmi_client_config_ctor(config);
    /* Flat 16 bytes; the firmware strlcpys the service name into this field. */
    for (i = 0; i < sizeof(kService); ++i) {
        config[i] = (unsigned char)kService[i];
    }
    *(uint64_t*)(config + 0x10) = 0;
    *(uint64_t*)(config + 0x28) = PG_IPMI_REQUEST_BUFFER;
    *(uint64_t*)(config + 0x30) = PG_IPMI_CLIENT_STORAGE;

    rc = pg_ipmi_client_create(&client, config, NULL, g_ipmi_client_storage);
    if (rc < 0 || client == NULL) {
        pg_logf("stop: ipmi client create failed rc=%d", rc);
        return;
    }

    vtable = *(void***)client;
    connect = (PgIpmiClientConnect)vtable[2];
    rc = connect(client, 0, 0, &serviceResult);
    if (rc < 0 || serviceResult < 0) {
        pg_logf("stop: connect to %s failed rc=%d service_result=%d", PG_LNC_IPMI_SERVICE, rc, serviceResult);
        return;
    }

    bufs[0].data = &argsLength;
    bufs[0].size = sizeof(argsLength);
    bufs[1].data = (void*)PG_LNC_QUIT_PATH;
    bufs[1].size = strlen(PG_LNC_QUIT_PATH) + 1;
    /* No argv: a null pointer with length zero, as the firmware sends. */
    bufs[2].data = NULL;
    bufs[2].size = 0;

    serviceResult = 0;
    invoke = (PgIpmiClientInvoke)vtable[11];

    /*
     * Logged BEFORE the call, because the process is killed inside it: a line
     * written afterwards can never be reached on the path that works. So this
     * line means "the request is going out", and its being the last line in the
     * log is what success looks like.
     */
    pg_logf("stop: requesting title stop through %s path=%s", PG_LNC_IPMI_SERVICE, PG_LNC_QUIT_PATH);

    rc = invoke(client, PG_LNC_CMD_LOADEXEC, bufs, 3, &serviceResult, NULL, 0);
    if (rc == 0) {
        rc = serviceResult;
    }
    pg_logf("stop: ShellCore did not take the request rc=%d", rc);
}

/*
 * Stop the title because playgo_stub.dat has a line that cannot be understood.
 *
 * A typo must never quietly turn into a default. Every silent fallback here
 * produces a title that runs with settings its author did not choose - the
 * wrong chunk count, or every language reported as installed - and the only
 * evidence is a log line in a build that may not have logging compiled in. A
 * toast naming the file, the line and the text that failed is worth more than a
 * title that limps.
 *
 * A MISSING FILE IS NOT AN ERROR and never reaches here: the config is
 * optional, and so is every line after the first. This is only for a line that
 * is present and wrong.
 *
 * Returns only if ShellCore refused to stop the title, in which case
 * config_error is set and scePlayGoInitialize fails.
 */
static void pg_config_fatal(const char* detail) {
    char message[512];
    uint32_t i;

    pg_logf("config: fatal: %s", detail);

    snprintf(message, sizeof(message),
        "libScePlayGo: %s\n\nFix /app0/playgo_stub.dat or delete it to use defaults.", detail);
    pg_notify(message);

    /*
     * Let the notification service take the first request before sending the
     * next one: it hands the message to another process, and going away
     * instantly turns the diagnostic toast back into the silent failure this
     * path exists to prevent.
     */
    (void)sceKernelUsleep(300u * 1000u);

    snprintf(message, sizeof(message),
        "libScePlayGo: closing this title in %u seconds...", kConfigStopSeconds);
    pg_notify(message);

    for (i = 0; i < kConfigStopSeconds; ++i) {
        (void)sceKernelUsleep(1000u * 1000u);
    }

    pg_request_title_stop();

    /*
     * Still running, so ShellCore never accepted the request. The title must not
     * continue on settings its author did not choose, so the failure is recorded
     * and scePlayGoInitialize refuses instead - a title's own error handling is
     * a better place to end up than a PlayGo that quietly reports defaults.
     */
    g_playgo.config_error = 1;
    pg_logf("config: title stop was refused, scePlayGoInitialize will fail");
}

/* Copy a config line into a short, trimmed, quotable excerpt for a toast. */
static char* pg_trim_config_token(char* text);

static void pg_config_excerpt(char* out, size_t outSize, const char* line) {
    char scratch[kConfigExcerptSize];
    size_t i;

    for (i = 0; i + 1 < sizeof(scratch) && line[i] != '\0' && line[i] != '\n'; ++i) {
        scratch[i] = line[i];
    }
    scratch[i] = '\0';
    snprintf(out, outSize, "%s", pg_trim_config_token(scratch));
}

extern "C" {
    int module_start(size_t args, const void* argp) {
        pg_log_reset();
        pg_logf("module_start: module loaded version=%s args=%zu argp=%p", kPlayGoStubVersion, args, argp);
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

static unsigned long pg_strtoul_preserve_errno(const char* text, char** end, int base) {
    int savedErrno = errno;
    unsigned long value = strtoul(text, end, base);
    errno = savedErrno;
    return value;
}

static int pg_parse_chunk_count_line(const char* line, uint32_t* outChunkCount) {
    char* end;
    unsigned long value;

    /*
     * strtoul accepts a leading sign and wraps, so "-5" parses cleanly, passes
     * the trailing check below, and clamps to SCE_PLAYGO_CHUNK_INDEX_MAX -- a
     * wrong line silently reporting the maximum chunk count instead of stopping
     * the title. Returning 0 here hands it to the chunk id list parser, which
     * turns the wrapped value away on its own bound, so it reaches
     * pg_config_fatal like any other bad first line.
     */
    if (*line == '-' || *line == '+') {
        return 0;
    }
    value = pg_strtoul_preserve_errno(line, &end, 10);
    if (end == line || value == 0) {
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

        unsigned long value = pg_strtoul_preserve_errno(cursor, &end, 10);
        if (end == cursor || value >= SCE_PLAYGO_CHUNK_INDEX_MAX) {
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
    size_t i;

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

/*
 * Parse a comma-separated list of language names into an OR of their bits.
 *
 * On failure the token that could not be understood is copied into outBadToken
 * (empty when the list itself is malformed rather than one entry), so the
 * caller can name it in the toast instead of quoting the whole line.
 */
static int pg_parse_language_list(char* line, ScePlayGoLanguageMask* outMask, char* outBadToken, size_t badTokenSize) {
    char* context = NULL;
    char* token;
    size_t lineLength = strlen(line);
    ScePlayGoLanguageMask mask = 0;
    int count = 0;

    outBadToken[0] = '\0';
    if (lineLength == 0 || line[0] == ',' || line[lineLength - 1] == ',' || strstr(line, ",,") != NULL) {
        return 0;
    }

    for (token = strtok_r(line, ",", &context); token != NULL; token = strtok_r(NULL, ",", &context)) {
        int32_t systemLanguage;

        token = pg_trim_config_token(token);
        if (*token == '\0' || !pg_parse_language_id(token, &systemLanguage)) {
            snprintf(outBadToken, badTokenSize, "%s", token);
            return 0;
        }
        mask |= scePlayGoConvertLanguage(systemLanguage);
        ++count;
    }
    if (count == 0) {
        return 0;
    }

    *outMask = mask;
    return 1;
}

static int pg_parse_language_config_line(char* line, ScePlayGoLanguageMask* outMask, char* outBadToken, size_t badTokenSize) {
    char* value = pg_trim_config_token(line);

    outBadToken[0] = '\0';
    /*
     * "all" is the way to say "every language ships" on purpose. Blank lines are
     * skipped rather than counted (see pg_next_config_value), so without a
     * spelling for the default there would be no way to leave line 3 at its
     * default while still setting line 4.
     */
    if (strcasecmp(value, "all") == 0) {
        *outMask = SCE_PLAYGO_LANGUAGE_MASK_ALL;
        return 1;
    }
    if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        return pg_parse_language_hex_mask(value, outMask);
    }
    return pg_parse_language_list(value, outMask, outBadToken, badTokenSize);
}

/*
 * Parse the optional console-language fallback flag (fourth config line).
 *
 * Accepts 1/on/yes/true and 0/off/no/false, case-insensitively; an empty line
 * means disabled. Returns 1 when the line was understood, 0 otherwise.
 *
 * The flag is off by default, which keeps the reported language sets a pure
 * function of the config file: every console running the same package sees the
 * same masks. See pg_load_config and the README for what enabling it changes.
 */
static int pg_parse_console_fallback_line(char* line, int* outEnabled) {
    static const char* const kEnabledNames[] = { "1", "on", "yes", "true" };
    static const char* const kDisabledNames[] = { "0", "off", "no", "false" };
    char* value = pg_trim_config_token(line);
    size_t i;

    if (*value == '\0') {
        *outEnabled = 0;
        return 1;
    }
    for (i = 0; i < sizeof(kEnabledNames) / sizeof(kEnabledNames[0]); ++i) {
        if (strcasecmp(value, kEnabledNames[i]) == 0) {
            *outEnabled = 1;
            return 1;
        }
    }
    for (i = 0; i < sizeof(kDisabledNames) / sizeof(kDisabledNames[0]); ++i) {
        if (strcasecmp(value, kDisabledNames[i]) == 0) {
            *outEnabled = 0;
            return 1;
        }
    }
    return 0;
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
    g_playgo.available_language_mask = SCE_PLAYGO_LANGUAGE_MASK_ALL;
    g_playgo.console_language_fallback = 0;
    g_playgo.scenario_count = 0;
    g_playgo.config_loaded_from_file = 0;
    pg_reset_chunk_ids(kDefaultChunkCount);
}

/*
 * Read the next line that carries a value.
 *
 * Everything from a '#' to the end of the line is a comment, and a line that
 * holds nothing but a comment or whitespace is skipped entirely rather than
 * counted as one of the numbered lines below. That is what lets the file carry
 * a header comment, or a note above a value, without silently shifting what
 * every following line means.
 *
 * -> the trimmed value, or NULL at end of file.
 */
static char* pg_next_config_value(FILE* fp, char* line, uint32_t size) {
    while (fgets(line, (int)size, fp) != NULL) {
        char* comment = strchr(line, '#');
        char* value;

        if (comment != NULL) {
            *comment = '\0';
        }
        value = pg_trim_config_token(line);
        if (*value != '\0') {
            return value;
        }
    }
    return NULL;
}

/*
 * Load /app0/playgo_stub.dat.
 *
 * File format:
 *   line 1: chunk count, or comma-separated valid chunk id list
 *   line 2: scenario count (optional)
 *   line 3: shipped voice languages - comma-separated language name list (e.g.
 *           "english_us,french,german"), or a raw 0x... installed mask (optional)
 *   line 4: console-language fallback flag, 0/1 (optional, default 0)
 *
 * '#' starts a comment, and a blank or comment-only line does not count as one
 * of the numbered lines above. Line 3 takes "all" for every language.
 *
 * The file is optional and so is every line after the first; a missing file
 * means 1000 chunks and all languages. A line that IS present and cannot be
 * parsed stops the title through pg_config_fatal rather than falling back to a
 * default the author did not choose.
 * With line 4 absent or 0 the installed and supported language sets are both the
 * line 3 set, so the stub reports the same thing on every console. With line 4
 * set to 1 the console's current system language is added to the supported set
 * only, for titles that run their own voice fallback when the console language
 * is supported but not installed; the console language is never reported as
 * installed.
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
    ScePlayGoLanguageMask languageMask;
    int parsedLanguageConfig = 0;
    int consoleFallback = 0;
    char* valueLine;
    char excerpt[kConfigExcerptSize];
    char badToken[kConfigExcerptSize];
    char detail[256];

    pg_reset_chunk_ids(kDefaultChunkCount);
    g_playgo.scenario_count = 0;
    g_playgo.language_mask = SCE_PLAYGO_LANGUAGE_MASK_ALL;
    g_playgo.available_language_mask = SCE_PLAYGO_LANGUAGE_MASK_ALL;
    g_playgo.console_language_fallback = 0;
    g_playgo.config_error = 0;
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

    valueLine = pg_next_config_value(fp, line, kConfigLineSize);
    if (valueLine != NULL) {
        pg_config_excerpt(excerpt, sizeof(excerpt), valueLine);
        if (strchr(valueLine, ',') != NULL) {
            parsedChunkConfig = pg_parse_chunk_id_list_line(valueLine);
        } else if (pg_parse_chunk_count_line(valueLine, &chunkCount)) {
            pg_reset_chunk_ids(chunkCount);
            parsedChunkConfig = 1;
        }
        if (!parsedChunkConfig) {
            fclose(fp);
            snprintf(detail, sizeof(detail),
                "playgo_stub.dat line 1: \"%s\" is not a chunk count or a list of chunk ids", excerpt);
            pg_config_fatal(detail);
            return;
        }
    } else {
        pg_reset_chunk_ids(kDefaultChunkCount);
    }

    valueLine = pg_next_config_value(fp, line, kConfigLineSize);
    if (valueLine != NULL) {
        char* end;
        pg_config_excerpt(excerpt, sizeof(excerpt), valueLine);
        /*
         * A typo check, not a range check: pg_scenario_mask_all documents
         * support for counts far above SCE_PLAYGO_MAX_SCENARIO, so the only
         * thing rejected here is a line that is not a plain number.
         */
        value = pg_strtoul_preserve_errno(valueLine, &end, 10);
        if (end == valueLine || *end != '\0' || value > 0xFFFFFFFFul) {
            fclose(fp);
            snprintf(detail, sizeof(detail),
                "playgo_stub.dat line 2: \"%s\" is not a scenario count", excerpt);
            pg_config_fatal(detail);
            return;
        }
        g_playgo.scenario_count = (uint32_t)value;
    }

    valueLine = pg_next_config_value(fp, line, kConfigLineSize);
    if (valueLine != NULL) {
        pg_config_excerpt(excerpt, sizeof(excerpt), valueLine);
        if (pg_parse_language_config_line(valueLine, &languageMask, badToken, sizeof(badToken))) {
            parsedLanguageConfig = 1;
        } else {
            fclose(fp);
            if (badToken[0] != '\0') {
                snprintf(detail, sizeof(detail),
                    "playgo_stub.dat line 3: \"%s\" is not a language name", badToken);
            } else {
                snprintf(detail, sizeof(detail),
                    "playgo_stub.dat line 3: \"%s\" is not a language list, a 0x... mask, or \"all\"", excerpt);
            }
            pg_config_fatal(detail);
            return;
        }
    }

    valueLine = pg_next_config_value(fp, line, kConfigLineSize);
    if (valueLine != NULL) {
        pg_config_excerpt(excerpt, sizeof(excerpt), valueLine);
        if (!pg_parse_console_fallback_line(valueLine, &consoleFallback)) {
            fclose(fp);
            snprintf(detail, sizeof(detail),
                "playgo_stub.dat line 4: \"%s\" is not 0 or 1", excerpt);
            pg_config_fatal(detail);
            return;
        }
    }

    fclose(fp);

    if (parsedLanguageConfig) {
        g_playgo.language_mask = languageMask;
        g_playgo.available_language_mask = languageMask;
    }

    /*
     * Opt-in only: the console system language is queried at all only when the
     * config file asks for it, and it widens the supported set without ever
     * touching the installed set reported by scePlayGoGetLanguageMask.
     */
    g_playgo.console_language_fallback = consoleFallback;
    if (consoleFallback) {
        int32_t consoleLang = -1;
        ScePlayGoLanguageMask consoleBit = pg_console_language_bit(&consoleLang);
        pg_logf("config: console_language=%d console_bit=0x%016llx", consoleLang, (unsigned long long)consoleBit);
        g_playgo.available_language_mask |= consoleBit;
    }

    g_playgo.config_loaded_from_file = 1;
    pg_logf("config: loaded chunk_count=%u scenario_count=%u available_language_mask=0x%016llx current_language_mask=0x%016llx console_language_fallback=%d",
        g_playgo.chunk_count,
        g_playgo.scenario_count,
        (unsigned long long)g_playgo.available_language_mask,
        (unsigned long long)g_playgo.language_mask,
        g_playgo.console_language_fallback);
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

static int pg_language_mask_is_available(ScePlayGoLanguageMask languageMask) {
    return (languageMask & ~g_playgo.available_language_mask) == 0;
}

static int32_t pg_validate_optional_language_mask(
    ScePlayGoOptionalChunkType type,
    const ScePlayGoOptionalChunk* option) {
    if (type == SCE_PLAYGO_OPTIONAL_CHUNK_TYPE_LANGUAGE &&
        !pg_language_mask_is_available(option->languages)) {
        return SCE_PLAYGO_ERROR_BAD_LANG_CONFIGURATION;
    }
    return SCE_OK;
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
        if (g_playgo.config_error) {
            rc = SCE_PLAYGO_ERROR_FATAL;
            pg_logf("scePlayGoInitialize rc=%d config_error=1", rc);
            return rc;
        }
        g_playgo.initialized = 1;
        pg_logf("scePlayGoInitialize rc=0 version=%s chunk_count=%u scenario_count=%u config_source=%s available_language_mask=0x%016llx current_language_mask=0x%016llx console_language_fallback=%d install_speed=%d",
            kPlayGoStubVersion,
            g_playgo.chunk_count,
            g_playgo.scenario_count,
            g_playgo.config_loaded_from_file ? "file" : "defaults",
            (unsigned long long)g_playgo.available_language_mask,
            (unsigned long long)g_playgo.language_mask,
            g_playgo.console_language_fallback,
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
     * The requested set must be a subset of the languages exposed by the runtime
     * configuration.
     */
    PRX_INTERFACE int32_t scePlayGoSetLanguageMask(ScePlayGoHandle handle, ScePlayGoLanguageMask languageMask) {
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
     * Language queries return the active language selection. Other optional chunk
     * categories retain the permissive all-bits-set behavior.
     */
    PRX_INTERFACE int32_t scePlayGoGetOptionalChunk(ScePlayGoHandle handle, ScePlayGoOptionalChunkType type, ScePlayGoOptionalChunk* option) {
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (option == NULL) { rc = SCE_PLAYGO_ERROR_BAD_POINTER; goto done; }
        if (!pg_valid_optional_type(type)) { rc = SCE_PLAYGO_ERROR_BAD_OPTIONAL_TYPE; goto done; }
        option->bitmask = (type == SCE_PLAYGO_OPTIONAL_CHUNK_TYPE_LANGUAGE)
            ? g_playgo.language_mask
            : ~0ULL;
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
        rc = pg_validate_optional_language_mask(type, option);
        if (rc != SCE_OK) goto done;
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
     * Language queries return the configured availability boundary. Other optional
     * chunk categories retain the permissive all-bits-set behavior.
     */
    PRX_INTERFACE int32_t scePlayGoGetSupportedOptionalChunk(ScePlayGoHandle handle, ScePlayGoOptionalChunkType type, ScePlayGoOptionalChunk* option) {
        int32_t rc = pg_require_initialized();
        if (rc != SCE_OK) goto done;
        rc = pg_require_handle(handle);
        if (rc != SCE_OK) goto done;
        if (option == NULL) { rc = SCE_PLAYGO_ERROR_BAD_POINTER; goto done; }
        if (!pg_valid_optional_type(type)) { rc = SCE_PLAYGO_ERROR_BAD_OPTIONAL_TYPE; goto done; }
        option->bitmask = (type == SCE_PLAYGO_OPTIONAL_CHUNK_TYPE_LANGUAGE)
            ? g_playgo.available_language_mask
            : ~0ULL;
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
            "version=%s\n"
            "status=ok\n"
            "chunk_count=%u\n"
            "scenario_count=%u\n",
            kPlayGoStubVersion,
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
        rc = pg_validate_optional_language_mask(type, option);
        if (rc != SCE_OK) goto done;
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
