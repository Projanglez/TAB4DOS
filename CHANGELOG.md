# Changelog

All notable changes to TAB4DOS are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.1] - 2026-07-10

### Added
- Completion now cycles executables (`.EXE`/`.COM`/`.BAT`) first, then the
  remaining entries (directories count as non-executable), alphabetical
  within each group. A short two-note chirp (PC speaker) marks the switch
  from the executable to the non-executable group while cycling.

### Fixed
- TAB completion could miss files in directories with more than 64 entries:
  the scan used a bare `*.*` pattern and capped the result cache at 64, so
  files whose directory entry lay beyond position 64 were silently dropped.
  The typed stem is now part of the FindFirst pattern (e.g. `DARK*.*`), so
  DOS filters before the cache fills.
- Command completion could miss PATH executables: the command index was
  capped at 128 entries (DOS internals + a stocked `C:\DOS` already exhaust
  it), so commands from later PATH directories were silently never indexed.
  The cap is now 512 (the index lives on disk, so this costs no resident
  memory), and the installer warns if it is ever reached.
- The completion sort compared entries through a near pointer to a stack
  buffer — undefined inside the hook, where SS != DS. The scratch entry now
  lives in DGROUP (same bug class as the v0.3 DTA fix).

## [1.0.0] - 2026-06-26

### Added
- `/usetemp` switch — store the index/history files in `%TEMP%` / `%TMP%`
  instead of the program directory.

### Changed
- Default location for `TAB4DOS.IDX` / `TAB4DOS.HST` is now the program's own
  directory (next to the EXE), not `%TEMP%`.
- Reworked wording in banner/help output.

### Fixed
- `CD` / `DIR` backslash handling in path completion.

## [0.10.0] - 2026-06-22

### Changed
- Command history is now offloaded to a file (`TAB4DOS.HST`) instead of being
  kept in resident memory, further shrinking the resident footprint.

## [0.9.0] - 2026-06-22

### Added
- **Shift+TAB** — cycle completion matches backwards.

### Changed
- Command cache offloaded to a file, reducing resident memory use.

### Fixed
- `/u` uninstall now works reliably under a full DOS configuration
  (EMM386/UMB, DOS=HIGH, loaded TSRs).

## [0.8.0] - 2026-06-18

### Changed
- Renamed the project (internal) and cut resident size from ~15.9 KB to ~12 KB.

### Fixed
- `/u` uninstall path and environment-block handling.

## [0.7.0] - 2026-06-16

### Added
- Mid-line editing: Left/Right, Home/End, Del, Ins (insert/overwrite),
  Ctrl+Left / Ctrl+Right (jump word).
- Path completion and context-aware completion.

## [0.4.0] - 2026-06-16

### Added
- Command history (Up/Down).

## [0.3.0] - 2026-06-16

### Changed
- Standalone line editor; the INT 16h hook was removed entirely.
- All resident DOS/BIOS calls use direct `INT` opcodes (key read via INT 16h,
  echo via BIOS INT 10h, directory scan via INT 21h FindFirst/Next).

### Fixed
- Disabled Watcom stack checks (`-s`) — the root cause of the early hardware
  hangs (a TSR runs on a foreign stack).
- DTA buffer made global (SS != DS inside the hook) — the directory-scan fix.
- Completion length passed by value instead of via a stack pointer — the
  TAB-completion fix.

## [0.2.0] - 2026-06-16

### Changed
- Replaced the early dual-hook approach with a dedicated line editor.

## [0.1.0] - 2026-06-16

### Added
- Initial TSR with TAB filename completion for MS-DOS, hooking
  `INT 21h / AH=0Ah`.

[1.0.1]: https://github.com/Projanglez/TAB4DOS/releases/tag/v1.0.1
[1.0.0]: https://github.com/Projanglez/TAB4DOS/releases/tag/v1.0.0
