#ifndef LIBRARY_H
#define LIBRARY_H

#include <stddef.h>
#include <stdint.h>

#ifdef LIBRARY_IMPL
#  if defined(__ORBIS__) || defined(__PROSPERO__)
#    define PRX_INTERFACE __declspec(dllexport)
#  else
#    define PRX_INTERFACE __attribute__((visibility("default")))
#  endif
#else
#  if defined(__ORBIS__) || defined(__PROSPERO__)
#    define PRX_INTERFACE __declspec(dllimport)
#  else
#    define PRX_INTERFACE
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_OK (0)

#define SCE_PLAYGO_ERROR_UNKNOWN                    (-2135818239)
#define SCE_PLAYGO_ERROR_FATAL                      (-2135818238)
#define SCE_PLAYGO_ERROR_NO_MEMORY                  (-2135818237)
#define SCE_PLAYGO_ERROR_INVALID_ARGUMENT           (-2135818236)
#define SCE_PLAYGO_ERROR_NOT_INITIALIZED            (-2135818235)
#define SCE_PLAYGO_ERROR_ALREADY_INITIALIZED        (-2135818234)
#define SCE_PLAYGO_ERROR_ALREADY_STARTED            (-2135818233)
#define SCE_PLAYGO_ERROR_NOT_STARTED                (-2135818232)
#define SCE_PLAYGO_ERROR_BAD_HANDLE                 (-2135818231)
#define SCE_PLAYGO_ERROR_BAD_POINTER                (-2135818230)
#define SCE_PLAYGO_ERROR_BAD_SIZE                   (-2135818229)
#define SCE_PLAYGO_ERROR_BAD_CHUNK_ID               (-2135818228)
#define SCE_PLAYGO_ERROR_BAD_SPEED                  (-2135818227)
#define SCE_PLAYGO_ERROR_NOT_SUPPORT_PLAYGO         (-2135818226)
#define SCE_PLAYGO_ERROR_EPERM                      (-2135818225)
#define SCE_PLAYGO_ERROR_BAD_LOCUS                  (-2135818224)
#define SCE_PLAYGO_ERROR_NEED_DATA_DISC             (-2135818223)
#define SCE_PLAYGO_ERROR_BAD_LANG_CONFIGURATION     (-2135818207)
#define SCE_PLAYGO_ERROR_BAD_CHUNK_CONFIGURATION    (-2135818206)
#define SCE_PLAYGO_ERROR_PROHIBIT_CHUNK_ID          (-2135818205)
#define SCE_PLAYGO_ERROR_BAD_OPTIONAL_TYPE          (-2135818204)
#define SCE_PLAYGO_ERROR_MISSING_OPTIONAL_CHUNK     (-2135818203)
#define SCE_PLAYGO_ERROR_BAD_SCENARIO_CONFIGURATION (-2135818202)
#define SCE_PLAYGO_ERROR_DEVICE_NOSPC               (-2135818201)

#define SCE_PLAYGO_HEAP_SIZE         (2u * 1024u * 1024u)
#define SCE_PLAYGO_CHUNK_INDEX_MAX   (65000u)
#define SCE_PLAYGO_MAX_SCENARIO      (5u)
#define SCE_PLAYGO_MAX_LANGUAGE      (64u)
#define SCE_PLAYGO_MAX_ETA_VALUE     (9223372036854775807LL)
#define SCE_PLAYGO_LANGUAGE_MASK_ALL (0xFFFFFFFFFFFFFFFFULL)

    typedef enum ScePlayGoLocusValue {
        SCE_PLAYGO_LOCUS_NOT_DOWNLOADED = 0,
        SCE_PLAYGO_LOCUS_LOCAL_SLOW = 2,
        SCE_PLAYGO_LOCUS_LOCAL_FAST = 3
    } ScePlayGoLocusValue;

    typedef enum ScePlayGoInstallSpeedValue {
        SCE_PLAYGO_INSTALL_SPEED_SUSPENDED = 0,
        SCE_PLAYGO_INSTALL_SPEED_TRICKLE = 1,
        SCE_PLAYGO_INSTALL_SPEED_FULL = 2
    } ScePlayGoInstallSpeedValue;

    typedef enum ScePlayGoOptionalChunkTypeValue {
        SCE_PLAYGO_OPTIONAL_CHUNK_TYPE_LANGUAGE = 0,
        SCE_PLAYGO_OPTIONAL_CHUNK_TYPE_SCENARIO = 1,
        SCE_PLAYGO_OPTIONAL_CHUNK_TYPE_OBSERVED_RESERVED = 2
    } ScePlayGoOptionalChunkTypeValue;

    typedef int32_t ScePlayGoHandle;
    typedef uint16_t ScePlayGoChunkId;
    typedef int8_t ScePlayGoLocus;
    typedef int32_t ScePlayGoInstallSpeed;
    typedef int64_t ScePlayGoEta;
    typedef uint64_t ScePlayGoLanguageMask;
    typedef uint64_t ScePlayGoScenarioMask;
    typedef int32_t ScePlayGoOptionalChunkType;

    typedef struct ScePlayGoInitParams {
        const void* bufAddr;
        uint32_t bufSize;
        uint32_t reserved;
    } ScePlayGoInitParams;

    typedef struct ScePlayGoToDo {
        ScePlayGoChunkId chunkId;
        ScePlayGoLocus locus;
        int8_t reserved;
    } ScePlayGoToDo;

    typedef struct ScePlayGoProgress {
        uint64_t progressSize;
        uint64_t totalSize;
    } ScePlayGoProgress;

    typedef union ScePlayGoOptionalChunk {
        uint64_t bitmask;
        ScePlayGoLanguageMask languages;
        ScePlayGoScenarioMask scenarios;
    } ScePlayGoOptionalChunk;

    PRX_INTERFACE int32_t scePlayGoInitialize(const ScePlayGoInitParams* initParam);
    PRX_INTERFACE int32_t scePlayGoTerminate(void);
    PRX_INTERFACE int32_t scePlayGoOpen(ScePlayGoHandle* outHandle, const void* param);
    PRX_INTERFACE int32_t scePlayGoClose(ScePlayGoHandle handle);
    PRX_INTERFACE int32_t scePlayGoGetLocus(ScePlayGoHandle handle, const ScePlayGoChunkId* chunkIds, uint32_t numberOfEntries, ScePlayGoLocus* outLoci);
    PRX_INTERFACE int32_t scePlayGoSetToDoList(ScePlayGoHandle handle, const ScePlayGoToDo* todoList, uint32_t numberOfEntries);
    PRX_INTERFACE int32_t scePlayGoGetToDoList(ScePlayGoHandle handle, ScePlayGoToDo* outTodoList, uint32_t numberOfEntries, uint32_t* outEntries);
    PRX_INTERFACE int32_t scePlayGoPrefetch(ScePlayGoHandle handle, const ScePlayGoChunkId* chunkIds, uint32_t numberOfEntries, ScePlayGoLocus minimumLocus);
    PRX_INTERFACE int32_t scePlayGoGetEta(ScePlayGoHandle handle, const ScePlayGoChunkId* chunkIds, uint32_t numberOfEntries, ScePlayGoEta* outEta);
    PRX_INTERFACE int32_t scePlayGoSetInstallSpeed(ScePlayGoHandle handle, ScePlayGoInstallSpeed speed);
    PRX_INTERFACE int32_t scePlayGoGetInstallSpeed(ScePlayGoHandle handle, ScePlayGoInstallSpeed* speed);
    PRX_INTERFACE int32_t scePlayGoSetLanguageMask(ScePlayGoHandle handle, ScePlayGoLanguageMask languageMask);
    PRX_INTERFACE int32_t scePlayGoGetLanguageMask(ScePlayGoHandle handle, ScePlayGoLanguageMask* outLanguageMask);
    PRX_INTERFACE int32_t scePlayGoGetProgress(ScePlayGoHandle handle, const ScePlayGoChunkId* chunkIds, uint32_t numberOfEntries, ScePlayGoProgress* outProgress);
    PRX_INTERFACE int32_t scePlayGoGetChunkId(ScePlayGoHandle handle, ScePlayGoChunkId* outChunkIdList, uint32_t numberOfEntries, uint32_t* outEntries);
    PRX_INTERFACE int32_t scePlayGoGetOptionalChunk(ScePlayGoHandle handle, ScePlayGoOptionalChunkType type, ScePlayGoOptionalChunk* option);
    PRX_INTERFACE int32_t scePlayGoPrefetchOptionalChunk(ScePlayGoHandle handle, ScePlayGoOptionalChunkType type, const ScePlayGoOptionalChunk* option);
    PRX_INTERFACE int32_t scePlayGoGetInstallChunkId(ScePlayGoHandle handle, ScePlayGoChunkId* outChunkIdList, uint32_t numberOfEntries, uint32_t* outEntries);
    PRX_INTERFACE int32_t scePlayGoGetSupportedOptionalChunk(ScePlayGoHandle handle, ScePlayGoOptionalChunkType type, ScePlayGoOptionalChunk* option);
    PRX_INTERFACE int32_t scePlayGoRequestOptionalChunk(ScePlayGoHandle handle, ScePlayGoOptionalChunkType type, const ScePlayGoOptionalChunk* option);
    PRX_INTERFACE int32_t scePlayGoRequestNextChunk(ScePlayGoHandle handle, const void* reserved);
    PRX_INTERFACE int32_t scePlayGoSnapshot(ScePlayGoHandle handle, const char* filename);
    PRX_INTERFACE int32_t scePlayGoGetRequiredDisc(ScePlayGoHandle handle, ScePlayGoOptionalChunkType type, const ScePlayGoOptionalChunk* option, uint32_t* outRequiredDisc);
    PRX_INTERFACE int32_t scePlayGoSetInstallSpeed2(int32_t reservedMustBeZero, uint32_t speed);

    static inline ScePlayGoLanguageMask scePlayGoConvertLanguage(int32_t systemLang) {
        return (systemLang >= 0 && systemLang < 48) ? (1ULL << (64 - systemLang - 1)) : 0ULL;
    }

    static inline ScePlayGoScenarioMask scePlayGoConvertScenario(uint32_t scenarioId) {
        return (scenarioId < SCE_PLAYGO_MAX_SCENARIO) ? (1ULL << scenarioId) : 0ULL;
    }

#ifdef __cplusplus
}
#endif

#endif
