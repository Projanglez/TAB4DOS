# Changelog

All notable changes to TAB4DOS are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.11.0] - 2026-06-22

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

[0.11.0]: https://github.com/Projanglez/TAB4DOS/releases/tag/v0.11.0
