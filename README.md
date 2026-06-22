# TAB4DOS

A small resident utility (TSR) for MS-DOS that adds **4DOS-style TAB filename
completion**, **command history**, and **command-line editing** to the standard
`COMMAND.COM` prompt.

It hooks `INT 21h / AH=0Ah` (buffered line input) and replaces COMMAND.COM's
line editor with its own, while leaving everything else untouched.

## Features

- **TAB / Shift+TAB** — complete and cycle matches (forward / backward) for
  files, directories, path fragments, DOS internal commands, and executables
  found on `PATH`.
- **Up / Down** — browse command history (64 entries).
- **Line editing** — Left/Right, Home/End, Del, Ins (insert/overwrite),
  Ctrl+Left/Ctrl+Right (jump word), ESC (clear line).
- **Small footprint** — ~8.5 KB resident. The command list and the history ring
  are kept in small files under `%TEMP%` (`TAB4DOS.IDX` / `TAB4DOS.HST`), not in
  resident memory; with SmartDrive these reads/writes are effectively RAM-speed.

## Requirements

- **CPU:** Intel 8086/8088 or later. The binary is pure 16-bit real-mode code
  with no 386- or 286-specific instructions.
- **OS:** MS-DOS 2.0 or later (it uses only DOS 2.0+ services — file handles,
  FindFirst/Next, the environment block, get/set interrupt vector, TSR).
  Developed and tested on MS-DOS 6.22.
- **Optional:** SmartDrive (`SMARTDRV`) makes the per-keystroke index/history
  file access RAM-fast. A `TEMP` or `TMP` variable is recommended; without one,
  the files fall back to `C:\TAB4DOS.IDX` / `C:\TAB4DOS.HST`.

## Usage

```
TAB4DOS        Install the TSR (shows a banner)
TAB4DOS /s     Install silently (no output)
TAB4DOS /u     Uninstall and free the resident memory
TAB4DOS /h     Show help
```

After installing, just type at the DOS prompt and use the keys above.

## Build

Built with **Open Watcom 16-bit (real mode)** — not `wcl386`; a TSR runs in real
mode.

1. Install Open Watcom (the build expects it under `C:\WATCOM`).
2. From a `CMD` prompt, run:

   ```
   build.bat
   ```

   Output: `tab4dos.exe`. The script compiles with `wcc -bt=dos -ms -os -s -zq`
   (`-s` is mandatory — it disables Watcom's `__STK` stack checks, which would
   falsely fire on the foreign stack a TSR runs on) and links with
   `wlink @tab4dos.lnk`.

## Notes

- Test on **real DOS hardware** (or a faithful emulator). DOSBox ships its own
  built-in TAB completion that overrides this tool, so results there are not
  meaningful.

(c) 2026 Projanglez
