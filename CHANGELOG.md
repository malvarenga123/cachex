# CacheExplorer Changelog

## 0.15-LLM — changes since 0.14

---

### Bug Fixes

**Discarded return values in timing tests (runtime measurement corruption)**
`TestPlextorFUACommandWorks` and `TestRCDBitWorks` both called `ReadCommand.pFunc()`
and discarded the return value, then read `Delay` or `Delay2` from the previous
call's result. This made the post-flush timing measurement silently wrong in every
test iteration. Both are now correctly assigned: `result = ReadCommand.pFunc(...)`.

**`TestPlextorFUAInvalidationSize` stale result**
Same class of bug: `ReadCommand.pFunc()` return value was discarded and `Delay`
was read from the previous `result`. Fixed.

**`SetMaxDriveSpeed` (`-s`) only executed under `-d`**
`SetDriveSpeed()` was nested inside an `if (DEBUG)` block, so `-s` had no effect
unless `-d` was also passed. The call is now unconditional; the debug message
remains gated on `-d`.

**`ScsiStatus` field name shadowed the `ScsiStatus` namespace**
`CommandResult` had a field named `ScsiStatus`, creating an ambiguous name
lookup in `operator bool()`. Renamed to `ScsiStatusCode` throughout.

**`TestCacheLineSizePrefetch` used old field name (compile error)**
Two references to `result.ScsiStatus` in `TestCacheLineSizePrefetch` were not
updated with the rename. Fixed to `result.ScsiStatusCode`.

**`CommandResult` default `ScsiStatusCode` was `CHECK_CONDITION`**
A previous fix had changed the sentinel from `0xff` to `ScsiStatus::CHECK_CONDITION`,
making a freshly constructed (not-yet-executed) result look like a real SCSI failure.
Reverted to `0xff`, which cannot be confused with any valid SCSI status code.

**`TestCacheLineSize_Stat` returned array index instead of sector count**
The function returned `MostFrequentDeltaIndex` (an index into `DeltaArray`) instead
of `DeltaArray[MostFrequentDeltaIndex].delta * BurstSize`. All callers received a
meaningless small integer.

**`TestCacheLineSize_Stat` could overflow `DeltaArray` and `PeakMeasuresIndexes`**
Both were fixed-size arrays (`std::array<sDelta, 50>` and `std::array<int, 100>`).
With erratic drive timing, more unique deltas than 50 would silently be dropped
and the loop would write past the end of `PeakMeasuresIndexes`. Both are now
`std::vector` that grow dynamically; the delta lookup was rewritten to use
`push_back` for new entries.

**`GetSupportedCommand` asserted instead of giving a useful error**
If called with no supported command (e.g. no `-i` and no `-r`), the function hit
`assert()` in debug builds and was undefined behaviour in release. Replaced with
a graceful error message and clean exit.

**`i = NbTests` before `break` was dead code in `TestRCDBitWorks`**
The loop-counter assignment before `break` had no effect. Removed.

**`NB_IGNORE_MEASURES` was 5, causing cache line size tests to produce no result
when test count was 5 or fewer**
With `NB_IGNORE_MEASURES = 5`, all 5 warm-up measurements were discarded and
`MaxCacheLineSize` was never updated, returning 0. Reduced to 2, which is
sufficient for drive warm-up while leaving useful data at small test counts.

**`Nbtests` bled between test blocks**
`Nbtests` was shared across all blocks in `main`. When `-p` ran first it set
`Nbtests = 5`, then every subsequent block (e.g. `-c`) saw a non-zero value and
skipped its own default (10 for cache line tests), resulting in too few
measurements. Each block now resets `Nbtests = 0` after completing, so each gets
its own independent default.

**`-r` + `-i` FUA state corruption**
Running `-i` before `-r` could clear `FUAbitSupported` on the user-selected
command if the `-i` probe rejected a FUA read. The `-r` override then set
`Supported = true` but left `FUAbitSupported = false`, so `-z` reported
"requires FUA support" even for commands declared FUA-capable. Fixed by adding
`DeclaredFUA` (immutable table value) to `sReadCommand`; `-r` now restores
`FUAbitSupported = DeclaredFUA`.

**`TestCacheSpeedImpact` (`-z`) used FUA reads instead of a validated flush**
`-z` sent the read command with `FUAbit=1` to force a cache bypass. This only
works on drives where a FUA-flagged data read bypasses the cache — most don't
honour it. The function now uses the best validated flush available: Plextor FUA
flush if `-p` confirmed it works, SYNCHRONIZE CACHE if `-q` confirmed it works,
FUA read as a last resort only if explicitly probe-validated.

**`PlextorFlushValidated` was gated on effectiveness test result**
When `-r 28h_12` was used, the Plextor flush effectiveness test returned `0/5`
(because the read and flush commands are the same, interfering with each other).
This incorrectly prevented `-z` from using the Plextor flush. The flag is now set
when the flush command is *accepted* by the drive, not on the effectiveness result.

**Duplicate `[+] Testing cache speed impact` header**
The header was printed once in `main` before calling `TestCacheSpeedImpact`, and
again inside the function itself. The redundant print in `main` was removed.

**`NetBSD` duration calculation integer overflow**
`(ts.tv_sec * 100000) + (ts.tv_nsec / 10000)` performed integer arithmetic before
casting to `double`, overflowing for uptimes over ~5.9 hours on 32-bit `time_t`.
Fixed to `(double)ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6`.

**`argv[++i]` without bounds check**
All option arguments (`-l`, `-b`, `-r`, `-s`, `-m`, `-n`, `-x`, `-t`, `-y`)
incremented `i` before checking `argc`, causing an out-of-bounds read if the
option was the last argument. Each now checks `++i >= argc` and prints a clear
error message.

**`atoi` used for all numeric arguments**
`atoi` returns 0 silently on invalid input. All numeric arguments now use
`std::stoi`, wrapped in a `try`/`catch` block that reports `invalid numeric
argument` or `numeric argument out of range`.

**Uninitialised variables in `TestCacheLineSize_Straight` and `_Wrap`**
`int TargetSectorOffset, CacheLineSize` and `double PreviousDelay, InitialDelay`
were declared without initializers. Split to one declaration per variable, each
initialised to zero.

**`CacheLineSizeSectors` declared but never read**
The variable was assigned but its value was never used after the test. It is now
printed as `[+] Cache line size result: N sectors (M kB)` after each method-1
and method-2 run.

**`CACHE_TEST_BLOCK` defined with `#define` inside a function**
The macro leaked into the rest of the translation unit. Replaced with
`constexpr int CACHE_TEST_BLOCK = 20`.

**`ModeSelect` `data.size()` silently narrowed to `uint16_t`**
`data.size()` is `size_t`; `Command::ModeSelect` takes `std::uint16_t`. Added
`static_cast<std::uint16_t>`.

**`SetCacheRCDBit` implicit `bool`-to-`uint8_t` in bitwise OR**
`RCDBitValue` (a `bool`) was used directly in a bitwise OR expression. Made
explicit with `static_cast<std::uint8_t>(RCDBitValue)`.

**`DeltaArray` not explicitly value-initialised**
`std::array<sDelta, 50> DeltaArray` relied on `sDelta`'s in-class initialisers
but had no `{}`. Changed to `std::array<sDelta, 50> DeltaArray{}` to make intent
explicit and guard against future struct changes.

---

### New Features

**`-q`: SYNCHRONIZE CACHE (0x35) flush test**
Tests whether the standard SBC-3 `SYNCHRONIZE CACHE` command (opcode `0x35`) is
accepted and whether it actually invalidates the drive's read cache. Results are
stored and used by `-z` if the Plextor flush is unavailable. Complements `-p` for
non-Plextor drives.

**Sense data logging (`LogSenseData`)**
All previous `RequestSense()` calls that discarded their result have been replaced
with `LogSenseData()`, which parses the 18-byte fixed-format sense response and
reports the sense key, ASC, and ASCQ. Genuine fault conditions (NOT READY, MEDIUM
ERROR, HARDWARE ERROR, UNIT ATTENTION, etc.) are always printed. ILLEGAL REQUEST
(`0x5`) is shown only under `-d`, since it is expected during capability probing
and does not indicate a real problem.

**`[+] Cache line size result` summary line**
After each cache line size test (methods 1 and 2), the final measured result is
printed as `[+] Cache line size result: N sectors (M kB)`.

**`-z` redesigned to use best available flush**
`-z` now measures cached vs. post-flush read time using whatever flush was
validated on this drive during the session, rather than requiring the selected
read command to support FUA. Priority order: Plextor FUA flush (if `-p` passed),
SYNCHRONIZE CACHE (if `-q` passed), FUA read (if probe-validated). The output
labels which flush was used and classifies the result as confirmed, unclear, or
unavailable.

**Explanatory note when `-r 28h_12 -p` produces unreliable results**
When the selected read command is `28h_12` (the same command as the Plextor flush),
the effectiveness test result is unreliable. A note is printed explaining the
collision and suggesting a different read command for a cleaner test.

---

### Platform Fixes

**Windows: custom `SCSI_PASS_THROUGH_DIRECT` struct replaced with `<ntddscsi.h>`**
The hand-rolled struct definition bypassed the WOW64 thunk layer, causing incorrect
pointer-width handling when a 32-bit build ran on a 64-bit OS. Using the
system-provided definition from `<ntddscsi.h>` ensures the kernel's thunking
activates correctly and `sptd.Length = sizeof(sptd)` produces the right value
for the current build architecture.

**Windows: `SetThreadAffinityMask` failure not handled**
If `SetThreadAffinityMask` returned 0 (failure), restoring the old mask with value
0 would forbid the thread from running on any core. The return value is now
checked before restoring.

**Windows: `GetTickCount` replaced with `GetTickCount64`**
`GetTickCount` wraps every ~49.7 days. `GetTickCount64` eliminates the overflow.

**Windows: QPC + `SetThreadAffinityMask` timing replaced with `std::chrono`**
The old approach locked the thread to Core 0 for each timestamp, but the drive I/O
happened between the two timestamps while the thread was unlocked and could migrate
to a different core. Both timestamps are now taken with
`std::chrono::steady_clock::now()`, which is monotonic and core-independent.

**Windows: `open_volume` falls back to read-only if write access is denied**
Some drives or OS configurations reject `GENERIC_WRITE` on optical media. The
function now retries with `GENERIC_READ` only and prints a warning, rather than
failing to open the drive entirely.

**Windows: `printf` replaced with `std::cerr` in `open_volume` error paths**
Consistent output stream throughout.

**Linux/NetBSD: `O_EXCL` removed from `open_volume`**
Using `O_EXCL` without `O_CREAT` on a character device is undefined on POSIX.
Removed.

**Linux: actual transfer length now used to resize `Data`**
After `ioctl(SG_IO)`, `rv.Data` is resized to `dxfer_len - resid` (actual bytes
received) rather than remaining at the pre-allocated maximum.

**NetBSD: actual transfer length now used to resize `Data`**
After `ioctl(SCIOCCOMMAND)`, `rv.Data` is resized to `io.datalen_used`.

**Windows: post-transfer resize clamped to capacity**
`rv.Data.resize(sptd.DataTransferLength)` could theoretically grow the vector if
a misbehaving driver reported a larger-than-requested transfer count, invalidating
the pre-allocated DMA buffer pointer. The resize is now clamped to
`rv.Data.capacity()`.

**Linux/NetBSD: `monotonic_clock` switched to `std::chrono::steady_clock`**
Replaces the direct `clock_gettime(CLOCK_MONOTONIC)` call with the standard
C++11 `std::chrono::steady_clock`, consistent with Windows.

---

### Code Quality

**`DriveContext` struct consolidates all mutable per-drive state**
`hVolume`, `NbBurstReadSectors`, `ThresholdRatioMethod2`, `CachedNonCachedSpeedFactor`,
and `MaxCacheSectors` were bare globals. They are now fields of a `DriveContext`
struct (`g_ctx`), with inline accessor functions returning references. All call
sites are unchanged. Moving to multi-drive support requires only replacing `g_ctx`
with a local variable and threading it through function parameters.

**`DeviceHandle<Platform>` RAII wrapper**
The drive handle is now managed by a `DeviceHandle<Platform>` object in `main`.
The destructor calls `Platform::close_handle` automatically on every exit path,
including early returns and future exceptions. Each platform struct exposes
`static device_handle invalid_handle()` as the sentinel value for a moved-from or
unopened handle.

**Timing globals (`Delay`, `Delay2`, `InitDelay`) eliminated**
These were global variables that happened to be written and read within the same
function body. Each function that used them now declares its own local `double`
at the narrowest correct scope.

**All `#define` constants replaced with `constexpr`**
`MAX_CACHE_LINES`, `NB_IGNORE_MEASURES`, `DESCRIPTOR_BLOCK_1`,
`CACHING_MODE_PAGE`, `CD_DVD_CAPABILITIES_PAGE`, `RCD_BIT`,
`RCD_READ_CACHE_ENABLED`, `RCD_READ_CACHE_DISABLED`, and `CACHE_TEST_BLOCK`
are all now typed `constexpr` values.

**`exit(-1)` in `main` replaced with `return -1`**
`exit()` bypasses C++ stack unwinding. All early-exit paths in `main` now use
`return`.

**All string output uses ASCII only**
Em dashes in user-visible `std::cerr` strings have been replaced with `--` to
avoid mojibake on Windows consoles using non-UTF-8 code pages (e.g. cp1252).

**`sReadCommand` gains `DeclaredFUA` and `FUAValidated` fields**
`DeclaredFUA` stores the original table-declared FUA capability, preserved across
`-i` probing which may clear `FUAbitSupported`. `FUAValidated` is set only when
the `-i` probe actually confirms FUA works on the connected drive, preventing
unreliable measurements when `-r` forces a command that was not probe-validated.

**`DriveContext` gains `PlextorFlushValidated` and `SyncCacheValidated` flags**
Set after `-p` and `-q` confirm their respective flush mechanisms work, allowing
`-z` to select the best available flush without repeating probe commands.
