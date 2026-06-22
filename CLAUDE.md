# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# DOSTAB — DOS-TSR für TAB-Dateinamen-Completion

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
  (v0.1–v0.3). Prüfen mit `wdis -a dostab.obj` → es darf KEIN `call __STK`
  im residenten Code stehen.
- Open Watcom liegt unter `C:\WATCOM`, Binaries in `binnt64\`. `build.bat`
  ruft `%WATCOM%\owsetenv.bat` auf und setzt den PATH automatisch.
- Bauen: aus CMD `build.bat` aufrufen (nicht per Doppelklick im Explorer —
  dann fehlt das Argument). Ausgabe: `dostab.exe`.
- **KEIN DOSBox-Test.** DOSBox hat eine eigene TAB-Completion, die die unsere
  überlagert — die Ergebnisse sind dort nicht aussagekräftig. Der Anwender
  testet **ausschließlich direkt auf echter 386-Hardware**.
- **Compile-Loop ist closed-loop** über Claude Code: Agent baut via
  `build.bat`, liest `wcl`-Fehler, fixt, baut neu.
- **Laufzeit-/Verhaltenstest ist IMMER manuell auf echtem 386** (durch den
  Anwender). Nie ungetestet als „fertig" melden — ein fehlerhafter
  INT-21h-Hook kann den Rechner hängen lassen.

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
- **Aus demselben Grund: KEINE `&stackvar`-Pointer über Funktionsgrenzen im
  Hook!** `&local` ist ein *near*-Pointer (Offset), den die gerufene Funktion
  über DS dereferenziert — die Variable liegt aber auf SS. Im Hook (SS!=DS)
  liest sie Müll. War der Grund, warum TAB-Completion `len` als 0xCC statt 2
  sah. Lösung: Werte per Wert übergeben und zurückgeben (oder Globals nutzen).
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
5. **Environment-Block beim INSTALL freigeben, NICHT beim Uninstall** (v0.8).
   Nach `scan_path_env()` (PATH gelesen) den eigenen Env-Block freigeben
   (`AH=49h` auf PSP:0x2C) und `PSP:0x2C = 0` setzen — im normalen Prozess-
   kontext, daher sicher. Spart ~Env-Größe resident. `do_uninstall` gibt dann
   NUR den Programmblock (`psp_seg`) frei. Sackgassen, die das verursachten:
   den Env-Block IM Uninstall freizugeben hing den Rechner; ihn liegenzulassen
   erzeugte einen verwaisten MCB-Block, der den ZWEITEN install/uninstall-
   Zyklus zum Hängen brachte (Beep pro Taste = toter 0Ah-Hook).
6. **`#pragma aux` modify-Listen bei INT 21h/10h/16h** — MÜSSEN den vollen
   flüchtigen Satz `[bx cx dx si di es]` (plus benutzte) angeben. Der
   DOS-Dispatch und gechainte Handler dürfen diese Register zerstören. Eine
   zu enge Liste lässt den Compiler einen lebenden Wert über den `int`-Aufruf
   in einem dieser Register halten → Korruption (latenter Bug in v0.8 behoben).
   **Gilt AUCH für die residenten BIOS-Primitive `con_out` (INT 10h) und
   `get_key` (INT 16h), nicht nur für die INT-21h-Helfer!** Bei VOLLER Config
   hooken Maus-/Tastatur-/Display-TSRs (CTMOUSE, KEYB, DISPLAY/ANSI) diese
   BIOS-Interrupts und zerstören Register, die das nackte BIOS bewahrt. Eine
   enge Liste (`con_out` hatte nur `[ah bx]`, `get_key` gar keine) ist bei
   F5-Boot zufällig korrekt, **hängt aber bei vollem Boot**: z.B. wurde `old_vec`
   in `do_uninstall` über den `msg()`-Aufruf in DX gehalten, der gehookte INT 10h
   zerstörte DX → falscher INT-21h-Vektor restauriert → Hänger. Voller Satz
   `[ah bx cx dx si di es]` zwingt den Compiler, lebende Werte über den Aufruf
   auf den Stack zu spillen (per `wdis` verifiziert: `msg_` `push dx … pop dx`).
   Solche Bugs sind auf F5-Boot UNSICHTBAR — immer mit voller Config testen.
7. **Transient-Code-Split (INIT_TEXT/INIT_CODE):** Init/Uninstall-Funktionen
   liegen via `#pragma code_seg` in `INIT_TEXT`, per `dostab.lnk` ORDER über
   den Stack gelegt und durch `_dos_keep` freigegeben. Keep-Größe NICHT aus
   einem Code-Offset rechnen (INIT_TEXT bekommt einen eigenen Frame, Offset
   wird 0!) — die bewährte `(get_ss()-_psp)+(get_sp()/16)+16`-Formel nutzen.
   Resident darf NIE eine INIT_TEXT-Funktion aufrufen (per `wdis` prüfen).
8. **`/u` MUSS via direktem `INT 21h AH=4Ch` (`dos_exit()`) terminieren, NICHT
   über den Watcom-C-Runtime-Exit.** Nach erfolgreichem Unhook (`AH=25h`) +
   Free (`AH=49h`) — beide auf HW als OK verifiziert (Marker `1`/`2`/`3`) —
   **hängt der C-Runtime-Exit** (FiniRtns/atexit/Null-Pointer-Check) bei VOLLER
   Config (EMM386/UMB, DOS=HIGH, STACKS, geladene TSRs); bei F5 läuft er durch.
   `do_uninstall` ruft daher am Ende `dos_exit()` und kehrt nie zur C-Runtime
   zurück. Standard-TSR-Teardown: **Unhook → Free → AH=4Ch.** `return 0` danach
   ist unerreichbar (nur für den Compiler).
9. **Diagnose-Marker im `/u`-Teardown: NUR inline `con_out`, KEINE Helfer-
   Funktion.** Ein Hex-Dump-Helfer (`con_hex16`) bekam vom `-os`-Optimierer
   einen mit `do_uninstall` GETEILTEN Epilog (Cross-Function-Tail-Merge via
   gemeinsames `L$xxx`); das verfälschte den Rücksprung und ließ `/u` mitten im
   Dump sauber terminieren (statt zu hängen) → falsche Spur. Inline-`con_out`
   (per `#pragma aux` ge-inlined, kein Call, kein geteilter Epilog) ist als
   Marker zuverlässig.

## Arbeitsweise

- Vor jedem neuen Feature `/plan`, danach in **kleinen, testbaren
  Inkrementen** arbeiten und nach jedem Schritt einchecken.
- Nach jedem grünen Build + `wdis`-Prüfung: der Anwender macht den
  Smoke-Test auf echtem 386, dann erst weiter. (Kein DOSBox.)
