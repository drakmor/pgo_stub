# PS5 libScePlayGo Stub

`libScePlayGo` is a lightweight replacement module for the PlayGo runtime library on PlayStation 5 targets.

Current version: **0.5**.

The module provides the common PlayGo entry points expected by titles that query installation state, chunk availability, language masks, optional chunks, and progress. It is designed for environments where the real PlayGo service is not available or not useful, while still letting software continue through its normal PlayGo checks.

## What It Does

- Exports a `libScePlayGo` PRX-compatible interface.
- Reports PlayGo chunks as available and installed.
- Tracks simple open, close, initialization, install speed, language mask, and todo-list state.
- Provides deterministic responses for chunk IDs, progress, ETA, optional chunks, and required disc checks.
- Supports an optional runtime config file to adjust the visible chunk layout and the voice languages a title ships.
- Can optionally report the console system language as supported, so titles that gate their voice fallback on the supported set can take it. Off unless the config file asks for it.
- Stops the title with an on-screen notification when the config file has a line it cannot parse, rather than silently using defaults.
- Can optionally write call logs for debugging PlayGo-related behavior.

By default, the stub behaves optimistically: PlayGo is treated as initialized, all languages are available, all scenarios are supported, and installation is complete.

## Optional Config

At runtime, the module looks for:

```text
/app0/playgo_stub.dat
```

The file is optional. If it is missing, the module falls back to a default package layout with 1000 chunks and reports all languages as installed (so no language fallback is triggered). Every line after the first is optional too.

A line that **is** present and cannot be parsed does not fall back to a default — it stops the title. See [Configuration Errors](#configuration-errors).

Supported format:

```text
1000
5
english_us,russian,english_gb
0
```

The first line is either a chunk count or a comma-separated list of valid chunk IDs. The second line is an optional scenario count. The third line is an optional list of the voice languages the title actually **ships** (the languages it contains voice data for); the stub reports these as the *installed* languages. It is expressed as either:

- A 64-bit hexadecimal mask with a required `0x` prefix, for example `0x4080200000000000`.
- A comma-separated list of language names, for example `english_us,french,german`.

Names are case-insensitive and whitespace around entries is ignored. An empty language set must be written as `0x0`.

The fourth line is an optional console-language fallback flag, `0` (default, off) or `1`. It is only useful for a few titles and is described under [Language Selection and Fallback](#language-selection-and-fallback).

`#` starts a comment, so values can be annotated in place. A blank or comment-only line is skipped rather than counted, so a header comment does not shift what the following lines mean. Line 3 also accepts `all`, which is how to leave the language set at its default while still setting the fourth line.

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

The `ID` column is each language's PlayGo code, shown for reference (for example to build a hex mask as `1 << (63 - ID)`); IDs are not accepted directly in the language list.

Examples:

```text
250
```

```text
0,1,2,10,20,30
3
0x4080200000000000
```

The same language selection can be written as a list of names:

```text
0,1,2,10,20,30
3
english_us,russian,english_gb
```

## Configuration Errors

A typo in `playgo_stub.dat` stops the title rather than quietly becoming a default. The stub shows a notification naming the line and the text that failed — `playgo_stub.dat line 3: "englsh_us" is not a language name` — counts down 12 seconds, and closes the title.

Every value in this file changes what the stub reports to the game, so a silent fallback means a title running on settings nobody chose, surfacing much later as a bug nobody can attribute. A missing file or a missing line is not an error; only a line that is present and wrong.

## Language Selection and Fallback

Some titles ship voice data for only a subset of system languages. On a console whose language has no shipped voice data, such a title can end up trying to load a voice archive that does not exist, which leaves dialogue silent.

By default the stub reports the third config line as both the installed and the supported language set, and nothing depends on the console: every console running the same package gets the same masks, and a title that asks whether its console language is installed is told no and can run its own fallback.

Some engines instead gate their fallback on the *supported* set, and treat a language that is neither installed nor supported as an error rather than a cue to fall back. For those titles only, an optional fourth config line adds the console's current system language to the supported set:

```text
1000
5
english_us,russian,english_gb
1
```

With the flag enabled, the stub distinguishes two things the PlayGo API reports separately:

- **Installed languages** (`scePlayGoGetLanguageMask`) — the voice languages the title ships, i.e. the third config line. The console language is never added here.
- **Supported languages** (`scePlayGoGetSupportedOptionalChunk`) — the installed set **plus the console's current system language**, which the stub reads at runtime.

When the console language is *supported but not installed*, a title's own logic recognizes that its language is unavailable and selects its fallback voice language instead. When the console language is in the shipped list, it is reported as installed and plays normally.

Because the supported set then depends on a runtime value, a package configured this way can behave differently on two consoles set to different languages. That is the point of the flag, but it also makes reports harder to interpret, which is why it is off by default and why the effective setting is written to the log as `console_language_fallback=`. Leave it at `0` unless a specific title needs it.

> **Final language selection is game-engine dependent.** The stub only reports which languages are installed and supported; it does not choose the language a title plays. Whether a title falls back at all, and which language it falls back to — commonly English (US), but this varies by engine — is decided entirely by the title's own code. The stub cannot force a specific language or create a fallback a title does not implement, and titles built on different engines may need a different shipped-language list or may not honor this mechanism. If voice selection does not behave as expected, enable logging and check the reported `current_language_mask` and `available_language_mask`.

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

Based on https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/playgo/playgo.cpp
