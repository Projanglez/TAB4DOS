# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# TAB4DOS — DOS TSR for TAB filename completion

Resident program (TSR) for **MS-DOS 6.22 on a real 386**. Hooks
`INT 21h / AH=0Ah` and replaces COMMAND.COM's line input with its own
editor providing TAB completion (4DOS-style cycling through matches).

## Build & Test

- Compiler: **Open Watcom 16-bit, Real Mode** — NOT `wcl386`. A TSR
  runs in real mode.
- **`-s` is MANDATORY** (set in `build.bat`): it disables the
  stack-overflow checks (the `__STK` call at every function entry). The
  resident code runs on COMMAND.COM's/DOS' stack; `__STK` compares SP against
  *our* runtime stack limits, falsely reports an overflow there and hangs
  the machine. This was the cause of several "input dead + beep" hangs
  (v0.1–v0.3). Check with `wdis -a tab4dos.obj` → there must be NO `call __STK`
  in the resident code.
- Open Watcom lives under `C:\WATCOM`, binaries in `binnt64\`. `build.bat`
  calls `%WATCOM%\owsetenv.bat` and sets the PATH automatically.
- Build: invoke `build.bat` from CMD (not by double-clicking in Explorer —
  then the argument is missing). Output: `tab4dos.exe`.
- **NO DOSBox testing.** DOSBox has its own TAB completion that overlays
  ours — the results there are not meaningful. The user
  tests **exclusively on real 386 hardware**.
- **The compile loop is closed-loop** via Claude Code: the agent builds via
  `build.bat`, reads `wcl` errors, fixes, rebuilds.
- **Runtime/behavior testing is ALWAYS manual on a real 386** (by the
  user). Never report "done" untested — a faulty
  INT 21h hook can hang the machine.

## Architecture core (invariants — do not break)

- **InDOS == 0 on entry to the 0Ah hook.** We intercept INT 21h *before*
  the DOS handler, so we may call DOS functions ourselves (02h,
  4Eh/4Fh, 1Ah/2Fh). This invariant is the foundation of everything.
  It includes **handle-based file I/O** (3Dh/3Fh/40h/3Eh/42h/3Ch): the command
  cache (`idx_path`) and history (`hist_path`) live in `%TEMP%` files, not
  resident. Even **writing per command** (history on ENTER) is proven in the
  hook on real HW (v0.10). Every file op MUST be fail-safe (on error →
  skip/beep, NEVER hang) — patterns like `cmd_find`/`hist_load`.
- **In the hook, `SS != DS`!** The `__interrupt` prolog sets DS=DGROUP, but
  SS stays the caller's stack (COMMAND.COM). A `(void far*)` cast
  of a **stack** array takes DS, however → wrong segment. Buffers that DOS
  fills (e.g. the DTA for FindFirst) MUST be **global** variables
  (they live in DGROUP, then DS:offset is correct). Otherwise DOS writes to a
  different place than we read (read stack garbage, `dta[30]` was 0x5E, skipping
  `.`/`..` failed). Explicit `MK_FP(seg,off)` with a passed-in segment (e.g.
  COMMAND.COM's buffer from `r.w.ds`/`r.w.dx`) is fine.
- **For the same reason: NO `&stackvar` pointers across function boundaries in the
  hook!** `&local` is a *near* pointer (offset) that the called function
  dereferences via DS — but the variable lives on SS. In the hook (SS!=DS)
  it reads garbage. This was why TAB completion saw `len` as 0xCC instead of 2.
  Fix: pass values by value and return them (or use globals).
- **Resident code: no non-reentrant C runtime.** No `printf`/`malloc`
  in the hook.
- **Do NOT use a Watcom int wrapper in the resident — neither `int86` NOR
  `intdos`/`intdosx`!** They all go through the same int-dispatch wrapper, which
  fails in the resident interrupt context (confirmed on HW):
  - `int86`/INT 16h → key never consumed, buffer full, "input dead + beep".
  - `intdos`/INT 21h AH=02h (echo) → wrong character at DOS (0xDB block).
  - `intdosx`/INT 21h FindFirst (scan) → finds nothing, TAB only beeps.
  Instead, for EVERY resident DOS/BIOS call: **a direct `INT` opcode
  via `#pragma aux`** (e.g. `int 0x16`/`int 0x10`/`int 0x21`). Recover the DOS
  carry via `sbb ax,ax`; far pointer in DX:AX. `intdosx` is OK ONLY in the
  init code of `main()` (before `_dos_keep`, hook not yet active).
  Verify via `wdis` that only literal `int` opcodes appear in the resident.
- **DTA:** set our own DTA before `FindFirst`, then restore COMMAND.COM's DTA
  afterwards.
- **new21:** handle function 0Ah ourselves (`return` ⇒ IRET, do not chain),
  all other functions via `_chain_intr`.
- **Init code (`main`, banner) is uncritical** — it runs only once before
  `_dos_keep`. Size/purity matters only in the resident part.

## Known risk areas

1. **Stack checks (`__STK`)** — on every "input dead/hang", FIRST check
   whether `-s` is active and no `__STK` is in the resident code (see Build).
2. `_dos_keep` paragraph calculation in `main()` — on instability, check
   here next.
3. `INTPACK` members (`r.w.ds` / `r.w.dx`) and `_chain_intr` —
   version-dependent in Open Watcom. (DS is set correctly to DGROUP in the
   `__interrupt` prolog — verified via `wdis`.)
4. ENTER outputs CR+LF (on a double blank line, drop the `0x0A`).
5. **Free the environment block on INSTALL, NOT on uninstall** (v0.8).
   After `scan_path_env()` (PATH read), free our own env block
   (`AH=49h` on PSP:0x2C) and set `PSP:0x2C = 0` — in the normal process
   context, therefore safe. Saves ~the env size resident. `do_uninstall` then
   frees ONLY the program block (`psp_seg`). Dead ends that caused this:
   freeing the env block IN uninstall hung the machine; leaving it in place
   created an orphaned MCB block that hung the SECOND install/uninstall
   cycle (beep per key = dead 0Ah hook).
6. **`#pragma aux` modify lists for INT 21h/10h/16h** — MUST list the full
   volatile set `[bx cx dx si di es]` (plus the ones used). The
   DOS dispatch and chained handlers may destroy these registers. A
   too-narrow list lets the compiler keep a live value across the `int` call
   in one of these registers → corruption (latent bug fixed in v0.8).
   **Applies ALSO to the resident BIOS primitives `con_out` (INT 10h) and
   `get_key` (INT 16h), not only the INT 21h helpers!** Under a FULL config,
   mouse/keyboard/display TSRs (CTMOUSE, KEYB, DISPLAY/ANSI) hook these
   BIOS interrupts and destroy registers that bare BIOS preserves. A
   narrow list (`con_out` had only `[ah bx]`, `get_key` none at all) is
   accidentally correct on an F5 boot, **but hangs on a full boot**: e.g.
   `old_vec` in `do_uninstall` was held in DX across the `msg()` call, the
   hooked INT 10h destroyed DX → a wrong INT 21h vector was restored → hang.
   The full set `[ah bx cx dx si di es]` forces the compiler to spill live
   values across the call onto the stack (verified via `wdis`: `msg_`
   `push dx … pop dx`). Such bugs are INVISIBLE on an F5 boot — always test
   with a full config.
7. **Transient code split (INIT_TEXT/INIT_CODE):** init/uninstall functions
   live via `#pragma code_seg` in `INIT_TEXT`, placed above the stack via
   `tab4dos.lnk` ORDER and freed by `_dos_keep`. Do NOT compute the keep size
   from a code offset (INIT_TEXT gets its own frame, the offset becomes 0!) —
   use the proven `(get_ss()-_psp)+(get_sp()/16)+16` formula.
   The resident must NEVER call an INIT_TEXT function (check via `wdis`).
8. **`/u` MUST terminate via a direct `INT 21h AH=4Ch` (`dos_exit()`), NOT
   via the Watcom C-runtime exit.** After a successful unhook (`AH=25h`) +
   free (`AH=49h`) — both verified OK on HW (markers `1`/`2`/`3`) —
   **the C-runtime exit hangs** (FiniRtns/atexit/null-pointer check) under a
   FULL config (EMM386/UMB, DOS=HIGH, STACKS, loaded TSRs); on F5 it runs
   through. `do_uninstall` therefore calls `dos_exit()` at the end and never
   returns to the C runtime. Standard TSR teardown: **Unhook → Free → AH=4Ch.**
   The `return 0` after it is unreachable (only to satisfy the compiler).
9. **Diagnostic markers in the `/u` teardown: ONLY inline `con_out`, NO helper
   function.** A hex-dump helper (`con_hex16`) was given by the `-os` optimizer
   an epilogue SHARED with `do_uninstall` (cross-function tail merge via a
   common `L$xxx`); this corrupted the return and made `/u` terminate cleanly
   mid-dump (instead of hanging) → a false trail. Inline `con_out` (inlined via
   `#pragma aux`, no call, no shared epilogue) is reliable as a marker.

## Workflow

- Before each new feature `/plan`, then work in **small, testable
  increments** and check in after each step.
- After each green build + `wdis` check: the user runs the
  smoke test on a real 386, and only then continue. (No DOSBox.)
