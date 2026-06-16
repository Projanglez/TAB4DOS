# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# TABTSR — DOS-TSR für TAB-Dateinamen-Completion

Resident-Programm (TSR) für **MS-DOS 6.22 auf echtem 386**. Hookt
`INT 21h / AH=0Ah` und ersetzt COMMAND.COMs Zeileneingabe durch einen
eigenen Editor mit TAB-Completion (4DOS-artiges Zykeln durch Treffer).

## Build & Test

- Compiler: **Open Watcom 16-bit, Real Mode** — NICHT `wcl386`. Ein TSR
  läuft im Real Mode.
- **`-s` ist ZWINGEND** (in `build.bat` gesetzt): schaltet die
  Stack-Overflow-Checks (`__STK`-Aufruf an jedem Funktionseingang) ab. Der
  residente Code läuft auf COMMAND.COMs/DOS' Stack; `__STK` vergleicht SP mit
  *unseren* Runtime-Stack-Grenzen, meldet dort fälschlich Overflow und hängt
  den Rechner auf. War die Ursache mehrerer „Eingabe tot + Beep"-Hänger
  (v0.1–v0.3). Prüfen mit `wdis -a tabtsr.obj` → es darf KEIN `call __STK`
  im residenten Code stehen.
- Open Watcom liegt unter `C:\WATCOM`, Binaries in `binnt64\`. `build.bat`
  ruft `%WATCOM%\owsetenv.bat` auf und setzt den PATH automatisch.
- Bauen: aus CMD `build.bat` aufrufen (nicht per Doppelklick im Explorer —
  dann fehlt das Argument). Ausgabe: `tabtsr.exe`.
- Test in DOSBox: `build.bat test` — startet `C:\dosgames\DOSBox.exe`,
  mounted das Projektverzeichnis als `C:`, lädt `tabtsr`, wartet mit `pause`.
- **Compile-Loop ist closed-loop** über Claude Code: Agent baut via
  `build.bat`, liest `wcl`-Fehler, fixt, baut neu.
- **Laufzeit-/Verhaltenstest ist IMMER manuell**: erst DOSBox (`build.bat test`),
  dann echter 386. Nie ungetestet auf Hardware —
  ein fehlerhafter INT-21h-Hook kann den Rechner hängen lassen.

## Architektur-Kern (Invarianten, nicht kaputt machen)

- **InDOS == 0 beim Eintritt in den 0Ah-Hook.** Wir fangen INT 21h *vor*
  dem DOS-Handler ab, deshalb dürfen wir selbst DOS-Funktionen (02h,
  4Eh/4Fh, 1Ah/2Fh) aufrufen. Diese Invariante ist die Grundlage von allem.
- **Im Hook ist `SS != DS`!** Der `__interrupt`-Prolog setzt DS=DGROUP, aber
  SS bleibt der Stack des Aufrufers (COMMAND.COM). Ein `(void far*)`-Cast
  eines **Stack**-Arrays nimmt aber DS → falsches Segment. Puffer, die DOS
  füllt (z.B. die DTA für FindFirst), MÜSSEN **globale** Variablen sein
  (liegen in DGROUP, dann stimmt DS:offset). Sonst schreibt DOS woanders hin
  als wir lesen (las Stack-Müll, `dta[30]` war 0x5E, Skip von `.`/`..` ging
  nicht). Explizite `MK_FP(seg,off)` mit übergebenem Segment (z.B. COMMAND.COMs
  Puffer aus `r.w.ds`/`r.w.dx`) sind ok.
- **Resident-Code: keine non-reentrant C-Runtime.** Kein `printf`/`malloc`
  im Hook.
- **KEIN Watcom-Int-Wrapper im Resident benutzen — weder `int86` NOCH
  `intdos`/`intdosx`!** Alle laufen über denselben Int-Dispatch-Wrapper, der
  im residenten Interrupt-Kontext versagt (auf HW bestätigt):
  - `int86`/INT 16h → Taste nie konsumiert, Puffer voll, „Eingabe tot + Beep".
  - `intdos`/INT 21h AH=02h (Echo) → falsches Zeichen bei DOS (0xDB-Block).
  - `intdosx`/INT 21h FindFirst (Scan) → findet nichts, TAB beept nur.
  Stattdessen für JEDEN residenten DOS/BIOS-Aufruf: **direkter `INT`-Opcode
  per `#pragma aux`** (z.B. `int 0x16`/`int 0x10`/`int 0x21`). DOS-Carry per
  `sbb ax,ax` zurückholen; Far-Pointer in DX:AX. `intdosx` ist NUR im
  Init-Code von `main()` (vor `_dos_keep`, Hook noch nicht aktiv) ok.
  Per `wdis` verifizieren, dass im Resident nur literale `int`-Opcodes stehen.
- **DTA:** vor `FindFirst` eigene DTA setzen, danach COMMAND.COMs DTA
  restaurieren.
- **new21:** Funktion 0Ah selbst behandeln (`return` ⇒ IRET, nicht chainen),
  alle anderen Funktionen via `_chain_intr`.
- **Init-Code (`main`, Banner) ist unkritisch** — läuft nur einmal vor
  `_dos_keep`. Größe/Reinheit zählt nur im residenten Teil.

## Bekannte Risikostellen

1. **Stack-Checks (`__STK`)** — bei jedem „Eingabe tot/Hänger" ZUERST prüfen,
   ob `-s` aktiv ist und kein `__STK` im residenten Code steht (siehe Build).
2. `_dos_keep`-Paragraphenrechnung in `main()` — bei Instabilität als Nächstes
   hier prüfen.
3. `INTPACK`-Member (`r.w.ds` / `r.w.dx`) und `_chain_intr` —
   versionsabhängig in Open Watcom. (DS wird im `__interrupt`-Prolog korrekt
   auf DGROUP gesetzt — per `wdis` verifiziert.)
4. ENTER gibt CR+LF aus (bei Doppel-Leerzeile das `0x0A` entfernen).

## v0.1-Grenzen & Roadmap

Aktuell: append-only Eingabe (keine Cursor-Tasten), nur Basename im
aktuellen Verzeichnis, kein Pfad-/PATH-Completion, 8.3 only.

Nächste Stufen (nach Aufwand):
1. **Pfad-Completion** — Verzeichnis vor dem letzten `\` als Suchpfad nutzen.
2. **Doppel-TAB = Trefferliste** anzeigen statt zykeln.
3. **Mid-Line-Editing** mit Cursor-Tasten (größter Umbau der Editor-Logik).

## Arbeitsweise

- Vor jedem neuen Feature `/plan`, danach in **kleinen, testbaren
  Inkrementen** arbeiten und nach jedem Schritt einchecken.
- Nach jedem grünen Build: manueller DOSBox-X-Smoke-Test, dann erst weiter.
