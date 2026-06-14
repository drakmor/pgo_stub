# PS5 libScePlayGo Stub

`libScePlayGo` is a lightweight replacement module for the PlayGo runtime library on PlayStation 5 targets.

The module provides the common PlayGo entry points expected by titles that query installation state, chunk availability, language masks, optional chunks, and progress. It is designed for environments where the real PlayGo service is not available or not useful, while still letting software continue through its normal PlayGo checks.

## What It Does

- Exports a `libScePlayGo` PRX-compatible interface.
- Reports PlayGo chunks as available and installed.
- Tracks simple open, close, initialization, install speed, language mask, and todo-list state.
- Provides deterministic responses for chunk IDs, progress, ETA, optional chunks, and required disc checks.
- Supports an optional runtime config file to adjust the visible chunk layout.
- Can optionally write call logs for debugging PlayGo-related behavior.

By default, the stub behaves optimistically: PlayGo is treated as initialized, all languages are available, all scenarios are supported, and installation is complete.

## Optional Config

At runtime, the module looks for:

```text
/app0/playgo_stub.dat
```

The file is optional. If it is missing or invalid, the module falls back to a default package layout with 1000 chunks.

Supported format:

```text
1000
5
```

The first line is either a chunk count or a comma-separated list of valid chunk IDs. The second line is an optional scenario count.

Examples:

```text
250
```

```text
0,1,2,10,20,30
3
```

## Logging

Logging is disabled by default. When built with logging enabled, the module writes a simple call log to:

```text
/app0/playlgo.log
```

The log is intended to help identify which PlayGo calls a title makes and what the stub returned.

## Scope

This project is a compatibility stub, not a full PlayGo implementation. It does not download, install, verify, or stream real package data. Its purpose is to satisfy PlayGo API usage with predictable responses so titles can continue running in test, research, or compatibility environments.

Based on https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/playgo/playgo.cpp
