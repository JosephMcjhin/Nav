# Antigravity UE Coding Skill

This document must be read at the start of every implementation cycle to prevent regression and compilation errors.

## 1. Signature Integrity
- **RULE**: Always check the function signature in the header (`.h`) before editing the implementation (`.cpp`).
- **CONSEQUENCE**: Mismatched signatures lead to `C2597` (illegal reference to non-static member) because the compiler treats the implementation as a static helper rather than a class member.

## 2. Header Policy
- **RULE**: Any use of `FScopeLock` requires `#include "Misc/ScopeLock.h"`.
- **RULE**: Any use of `IWebSocket` requires `#include "WebSocketsModule.h"`.
- **CONSEQUENCE**: Missing headers lead to `C2971` and template instantiation failures.

## 3. Thread Safety & UObjects
- **RULE**: Never call `NewObject`, `ResetAudio`, `Stop`, or `Play` on a non-Game thread.
- **RULE**: Use a `TArray<uint8>` buffer + `FCriticalSection` (or `FScopeLock`) to hand off data from background threads to `TickComponent`.
- **CONSEQUENCE**: Violations cause `0xC0000005` (Access Violation).

## 4. Silence & Timing
- **RULE**: Do not implement interruption logic via hard resets. Use 'Sequential Overwrite' (play current to end, then pick the latest pending).
- **RULE**: `LastTTSTime` must be used for periodic navigation prompts (5.0s interval).

## 5. Type Consistency
- **RULE**: Use `int32` for sizes/indices unless interfacing with low-level C++ APIs that strictly require `SIZE_T`.
- **CONSEQUENCE**: Prevents signed/unsigned mismatch warnings and C-style cast errors.
