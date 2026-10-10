# libScePlayGo Stub

`libScePlayGo` is a lightweight replacement module for the PlayGo runtime library on PlayStation 5 targets.

Current version: **0.5**.

The module provides the common PlayGo entry points expected by titles that query installation state, chunk availability, language masks, optional chunks, and progress. It is designed for environments where the real PlayGo service is not available or not useful, while still letting software continue through its normal PlayGo checks.

## What It Does

- Exports a `libScePlayGo` PRX-compatible interface.
- Reports PlayGo chunks as available and installed.
- Tracks simple open, close, initialization, install speed, language mask, and todo-list state.
- Provides deterministic responses for chunk IDs, progress, ETA, optional chunks, and required disc checks.
- Supports an optional runtime config file to adjust the visible chunk layout and available languages.
- Can optionally write call logs for debugging PlayGo-related behavior.

By default, all languages and the five SDK scenarios are available, and installation is complete. The title must still initialize PlayGo and open a handle.

## Optional Config

At runtime, the module looks for:

```text
/app0/playgo_stub.dat
```

The file is optional. If it is missing, the module falls back to a default package layout with 1000 chunks and enables all languages. Invalid language configuration falls back to all languages without changing the parsed chunk or scenario settings.

Supported format:

```text
1000
5
english_us,russian,english_gb
```

The first line is either a chunk count or a comma-separated list of valid chunk IDs. Counts above 65000 are clamped to 65000; zero, negative, overflowing, or malformed counts fall back to 1000 chunks. Physical lines longer than 65535 bytes or containing NUL bytes are discarded as invalid without shifting subsequent fields.

The second line is an optional scenario count from 0 through 5. A count of N enables scenario IDs 0 through N-1; an explicit 0 disables all scenarios. Missing or invalid scenario configuration enables all five scenarios. Optional-chunk queries report this mask, and requests for unavailable scenarios return `SCE_PLAYGO_ERROR_BAD_SCENARIO_CONFIGURATION`.

The third line is an optional language configuration, expressed as either:

- A 64-bit hexadecimal mask with a required `0x` prefix, for example `0x4080200000000000`.
- A comma-separated list of language names or numeric system-language IDs, for example `english_us,8,english_gb`.

Names are case-insensitive, whitespace around entries is ignored, and names and IDs can be mixed. Numeric IDs from 0 through 47 are accepted and converted with the PlayGo language-bit mapping. A single decimal value is therefore a language ID, while an empty language set must be written as `0x0`.

Supported language names are:

| ID | Name | ID | Name |
|---:|---|---:|---|
| 0 | `japanese` | 15 | `norwegian` |
| 1 | `english_us` | 16 | `polish` |
| 2 | `french` | 17 | `portuguese_br` |
| 3 | `spanish` | 18 | `english_gb` |
| 4 | `german` | 19 | `turkish` |
| 5 | `italian` | 20 | `spanish_la` |
| 6 | `dutch` | 21 | `arabic` |
| 7 | `portuguese_pt` | 22 | `french_ca` |
| 8 | `russian` | 23 | `czech` |
| 9 | `korean` | 24 | `hungarian` |
| 10 | `chinese_t` | 25 | `greek` |
| 11 | `chinese_s` | 26 | `romanian` |
| 12 | `finnish` | 27 | `thai` |
| 13 | `swedish` | 28 | `vietnamese` |
| 14 | `danish` | 29 | `indonesian` |

Examples:

```text
250
```

```text
0,1,2,10,20,30
3
0x4080200000000000
```

The same language selection can be written as a mixed list:

```text
0,1,2,10,20,30
3
english_us,8,english_gb
```

## Logging

Logging is disabled by default. When built with logging enabled, the module writes a simple call log to:

```text
/app0/playlgo.log
```

The log is intended to help identify which PlayGo calls a title makes and what the stub returned.

## Build Variants

Two optimized Prospero configurations are available:

- `ReleaseLog` builds with `PLAYGO_ENABLE_LOGGING=1` and writes artifacts to `out/log/`.
- `ReleaseNoLog` builds with `PLAYGO_ENABLE_LOGGING=0` and writes artifacts to `out/nolog/`.

Each configuration produces both `libScePlayGo.prx` and a fake-signed `libScePlayGo.sprx`. The SPRX is generated automatically by `tools/make_fself.py`.

```powershell
msbuild libScePlayGo.sln /p:Configuration=ReleaseLog /p:Platform=Prospero
msbuild libScePlayGo.sln /p:Configuration=ReleaseNoLog /p:Platform=Prospero
```

## Scope

This project is a compatibility stub, not a full PlayGo implementation. It does not download, install, verify, or stream real package data. Its purpose is to satisfy PlayGo API usage with predictable responses so titles can continue running in test, research, or compatibility environments.

### Empty Lists

After successful initialization and opening a valid handle, the stub accepts `numberOfEntries == 0` in these APIs:

- `scePlayGoGetLocus`: returns `SCE_OK` without accessing either array; both arrays may be `NULL`.
- `scePlayGoSetToDoList`: clears the stub's pending list and returns `SCE_OK`; the input array may be `NULL`.
- `scePlayGoGetToDoList`: writes `0` to `outEntries` and returns `SCE_OK`; the output array may be `NULL`. `outEntries` remains required.

This is a compatibility extension, not a claim about original SDK behavior. The local original PS5 PlayGo reference checks pointers before the count and rejects counts outside 1..1000 in these APIs.

Nonempty lists retain pointer, size, and chunk validation. Empty lists still require an initialized library and a valid open handle.
