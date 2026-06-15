# TABTSR — DOS-TSR für TAB-Dateinamen-Completion

Resident-Programm (TSR) für **MS-DOS 6.22 auf echtem 386**. Hookt
`INT 21h / AH=0Ah` und ersetzt COMMAND.COMs Zeileneingabe durch einen
eigenen Editor mit TAB-Completion (4DOS-artiges Zykeln durch Treffer).

## Build & Test

- Compiler: **Open Watcom 16-bit, Real Mode** — NICHT `wcl386`. Ein TSR
  läuft im Real Mode.
- Vor dem Kompilieren **muss** `%WATCOM%\setvars.bat` aufgerufen werden
  (sonst findet `wcl` keine Tools/Headers). `build.bat` erledigt das automatisch.
- Bauen: `build.bat` (ruft setvars.bat, dann `wcl`). Ausgabe: `tabtsr.exe`.
- **Compile-Loop ist closed-loop** über Claude Code: Agent baut via
  `build.bat`, liest `wcl`-Fehler, fixt, baut neu.
- **Laufzeit-/Verhaltenstest ist IMMER manuell**: erst DOSBox-X
  (`build.bat test`), dann echter 386. Nie ungetestet auf Hardware —
  ein fehlerhafter INT-21h-Hook kann den Rechner hängen lassen.

## Architektur-Kern (Invarianten, nicht kaputt machen)

- **InDOS == 0 beim Eintritt in den 0Ah-Hook.** Wir fangen INT 21h *vor*
  dem DOS-Handler ab, deshalb dürfen wir selbst DOS-Funktionen (02h,
  4Eh/4Fh, 1Ah/2Fh) aufrufen. Diese Invariante ist die Grundlage von allem.
- **Resident-Code: keine non-reentrant C-Runtime.** Kein `printf`/`malloc`
  im Hook. Nur `int86`/`intdos` (Register per Pointer ⇒ reentrant) und
  direkte INT-Aufrufe.
- **DTA:** vor `FindFirst` eigene DTA setzen, danach COMMAND.COMs DTA
  restaurieren.
- **new21:** Funktion 0Ah selbst behandeln (`return` ⇒ IRET, nicht chainen),
  alle anderen Funktionen via `_chain_intr`.
- **Init-Code (`main`, Banner) ist unkritisch** — läuft nur einmal vor
  `_dos_keep`. Größe/Reinheit zählt nur im residenten Teil.

## Bekannte Risikostellen

1. `_dos_keep`-Paragraphenrechnung in `main()` — bei Instabilität zuerst
   hier prüfen.
2. `INTPACK`-Member (`r.w.ds` / `r.w.dx`) und `_chain_intr` —
   versionsabhängig in Open Watcom.
3. ENTER gibt CR+LF aus (bei Doppel-Leerzeile das `0x0A` entfernen).

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
