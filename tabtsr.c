/*
 * TABTSR - TAB-Dateinamen-Completion fuer MS-DOS
 * v0.2  -  Open Watcom C (16-bit, Real Mode)
 *
 * Architektur (siehe CLAUDE.md fuer Details)
 * -------------------------------------------
 * v0.1 hookte INT 21h/AH=0Ah und implementierte einen eigenen Zeilen-Editor,
 * der bei TAB FindFirst/FindNext *aus dem laufenden Hook heraus* aufrief.
 * Das haengt auf echter DOS-6.22-Hardware (Re-Entranz-Problem, tiefer Stack)
 * und ist inkompatibel mit DOSKEY (COMMAND.COM umgeht AH=0Ah, wenn DOSKEY
 * geladen ist).
 *
 * v0.2 verwendet zwei Hooks:
 *
 *   new21 (INT 21h, AH=0Ah): scannt das aktuelle Verzeichnis EINMAL (sicher,
 *   da wir hier InDOS=0 haben und der Stack noch flach ist), setzt
 *   readline_active=1 und reicht den Aufruf an old21 weiter (DOSKEY oder
 *   DOS-eigener Editor uebernimmt Pfeiltasten/History wie gewohnt).
 *
 *   new16 (INT 16h, AH=00h): liest Tasten *bevor* DOSKEY/DOS sie sieht.
 *   Implementiert ein eigenes Warten (STI + Poll auf den BIOS-Keyboard-
 *   Puffer), weil _chain_intr ein nicht-rueckkehrender Jump ist und wir das
 *   Ergebnis sonst nicht inspizieren koennten. Bei TAB wird im bereits
 *   gescannten file_cache gesucht (kein DOS-Call!) und das Ergebnis als
 *   Backspace+Dateiname in eine eigene Tastatur-Queue gelegt. DOS/DOSKEY
 *   liest diese Zeichen wie normale Tastendruecke und editiert ihre eigene
 *   Zeile damit (Backspace loescht korrekt, Zeichen werden echoed).
 *
 * Build:
 *   build.bat   (ruft owsetenv.bat, dann wcl -bt=dos -ms -os -zq)
 *
 * Test ZUERST in DOSBox, dann auf echter Hardware.
 */

#include <dos.h>
#include <i86.h>

extern unsigned _psp;

unsigned get_ss( void );
#pragma aux get_ss = "mov ax, ss" value [ax];
unsigned get_sp( void );
#pragma aux get_sp = "mov ax, sp" value [ax];

/* -------- resident: Hook-Pointer ---------------------------------------- */

static void (__interrupt __far *old21)();
static void (__interrupt __far *old16)();

/* InDOS-Flag-Zeiger (INT 21h/34h) — Re-Entranz-Schutz in new21 */
static unsigned char far *indos_ptr = 0;

/* -------- resident: Dateiname-Cache (von scan_directory gefuellt) ------- */

#define MAX_FILES 64
#define NAME_LEN  13                  /* 8.3 + NUL                         */

static char          file_cache[MAX_FILES][NAME_LEN];
static unsigned char file_is_dir[MAX_FILES];
static int            file_count;

/* -------- resident: eigene Tastatur-Queue -------------------------------*/

#define QUEUE_SIZE 32
static unsigned kbd_queue[QUEUE_SIZE];
static int q_head = 0, q_tail = 0;

#define Q_EMPTY()  (q_head == q_tail)
static void q_push( unsigned key )
{
    int next = (q_tail + 1) % QUEUE_SIZE;
    if ( next == q_head ) return;     /* Queue voll: verwerfen             */
    kbd_queue[q_tail] = key;
    q_tail = next;
}
static unsigned q_pop( void )
{
    unsigned key = kbd_queue[q_head];
    q_head = (q_head + 1) % QUEUE_SIZE;
    return key;
}

/* -------- resident: Completion-Zustand ----------------------------------*/

static char comp_base[NAME_LEN];
static int  comp_base_len = 0;
static int  comp_active   = 0;
static int  comp_index    = 0;
static int  shown_len     = 0;
static int  readline_active = 0;

/* -------- BIOS-Keyboard-Puffer (Segment 0x0040) -------------------------*/

#define BIOS_SEG     0x0040
#define KBD_HEAD_OFF 0x001A
#define KBD_TAIL_OFF 0x001C
#define KBD_BUF_OFF  0x001E
#define KBD_BUF_END  0x003E

/* volatile ist hier ZWINGEND: Head/Tail werden vom Keyboard-IRQ (INT 09h)
   asynchron geaendert. Ohne volatile zieht der Optimierer (-os) die Reads aus
   der Warteschleife heraus -> Endlos-Hang, Puffer laeuft ueber (Test-1-Bug). */
static unsigned bios_peek_head( void )
{
    return *(volatile unsigned far *)MK_FP( BIOS_SEG, KBD_HEAD_OFF );
}
static unsigned bios_peek_tail( void )
{
    return *(volatile unsigned far *)MK_FP( BIOS_SEG, KBD_TAIL_OFF );
}
static unsigned bios_take_key( void )
{
    unsigned head = bios_peek_head();
    unsigned key  = *(volatile unsigned far *)MK_FP( BIOS_SEG, head );
    head += 2;
    if ( head >= KBD_BUF_END ) head = KBD_BUF_OFF;
    *(volatile unsigned far *)MK_FP( BIOS_SEG, KBD_HEAD_OFF ) = head;
    return key;
}

/* -------- Verzeichnis-Scan (nur aus new21 heraus aufgerufen) ------------ */

static void scan_directory( void )
{
    union REGS r; struct SREGS s;
    unsigned save_dta_seg, save_dta_off;
    unsigned char dta[64];
    char far *fdta = (char far *)dta;

    file_count = 0;

    /* alte DTA sichern */
    segread( &s );
    r.h.ah = 0x2F;
    intdosx( &r, &r, &s );
    save_dta_seg = s.es; save_dta_off = r.x.bx;

    /* eigene DTA setzen */
    segread( &s );
    r.h.ah = 0x1A;
    s.ds   = FP_SEG(fdta);
    r.x.dx = FP_OFF(fdta);
    intdosx( &r, &r, &s );

    /* FindFirst "*.*" inkl. Verzeichnisse */
    segread( &s );
    r.h.ah = 0x4E;
    r.x.cx = 0x10;
    s.ds   = FP_SEG("*.*");
    r.x.dx = FP_OFF("*.*");
    intdosx( &r, &r, &s );

    while ( r.x.cflag == 0 && file_count < MAX_FILES ) {
        if ( dta[30] != '.' ) {                    /* "." / ".." ueberspringen */
            int i;
            for ( i = 0; i < NAME_LEN - 1 && dta[30+i]; i++ )
                file_cache[file_count][i] = dta[30+i];
            file_cache[file_count][i] = 0;
            file_is_dir[file_count] = (dta[21] & 0x10) ? 1 : 0;
            file_count++;
        }
        r.h.ah = 0x4F;
        intdos( &r, &r );
    }

    /* DTA zurueck */
    segread( &s );
    r.h.ah = 0x1A;
    s.ds   = save_dta_seg;
    r.x.dx = save_dta_off;
    intdosx( &r, &r, &s );
}

/* -------- kleine lokale String-Helfer (kein <string.h> im Hook) --------- */

static int strlen_local( const char *s )
{
    int n = 0;
    while ( s[n] ) n++;
    return n;
}
/* Case-insensitiver Vergleich: file_cache haelt DTA-Namen GROSS, comp_base
   die roh getippten (meist kleinen) Tasten. FAT16-Case-Insensitivity gilt nur
   fuer DOS' eigenen Vergleich — wir matchen hier selbst, also upcasen. */
static int memcmp_local( const char *a, const char *b, int n )
{
    int i;
    char ca, cb;
    for ( i = 0; i < n; i++ ) {
        ca = a[i]; cb = b[i];
        if ( ca >= 'a' && ca <= 'z' ) ca -= 0x20;
        if ( cb >= 'a' && cb <= 'z' ) cb -= 0x20;
        if ( ca != cb ) return 1;
    }
    return 0;
}

/* -------- TAB-Completion: sucht nur im Cache, kein DOS-Call ------------- */

static void do_complete_queue( void )
{
    int i, target, count, matched, nl, namelen, erase;

    target  = comp_index;
    matched = -1;
    count   = 0;

    for ( i = 0; i < file_count; i++ ) {
        if ( comp_base_len == 0 ||
             ( strlen_local(file_cache[i]) >= comp_base_len &&
               memcmp_local(file_cache[i], comp_base, comp_base_len) == 0 ) ) {
            if ( count == target ) { matched = i; break; }
            count++;
        }
    }

    if ( matched < 0 && target > 0 ) {             /* wrap: von vorn        */
        comp_index = 0;
        for ( i = 0; i < file_count; i++ ) {
            if ( comp_base_len == 0 ||
                 ( strlen_local(file_cache[i]) >= comp_base_len &&
                   memcmp_local(file_cache[i], comp_base, comp_base_len) == 0 ) ) {
                matched = i;
                break;
            }
        }
    }

    if ( matched < 0 ) { comp_active = 0; return; }   /* kein Treffer       */

    /* Erste TAB: getipptes Praefix (comp_base_len) loeschen.
       Folge-TABs (Zykeln): vorigen Treffer (shown_len) loeschen. */
    erase = comp_active ? shown_len : comp_base_len;
    for ( i = 0; i < erase; i++ ) q_push( 0x0E08 );   /* Backspace          */

    namelen = strlen_local( file_cache[matched] );
    nl = 0;
    for ( i = 0; i < namelen; i++ ) {
        q_push( (unsigned char)file_cache[matched][i] );
        nl++;
    }
    if ( file_is_dir[matched] ) { q_push( '\\' ); nl++; }

    shown_len   = nl;
    comp_active = 1;
    comp_index++;
}

/* -------- INT 16h Hook: faengt Tasten ab, bevor DOSKEY/DOS sie sehen ---- */

void __interrupt __far new16( union INTPACK r )
{
    unsigned char ah = r.h.ah;
    unsigned key;

    /* AH=00h (Standard) UND AH=10h (Enhanced) sind blockierendes Lesen.
       Beide muessen wir abfangen: DOS/DOSKEY nutzen auf 386ern mit
       Enhanced-Keyboard-BIOS oft 10h/11h statt 00h/01h. */
    if ( ah == 0x00 || ah == 0x10 ) {
        if ( !Q_EMPTY() ) { r.w.ax = q_pop(); return; }

        /* Ausserhalb der Zeileneingabe NICHT eingreifen: voll an BIOS/DOSKEY
           durchreichen. So uebernimmt unser Busy-Wait nicht systemweit jeden
           Tastatur-Read (Sicherheit fuer Editoren, Spiele usw.). */
        if ( !readline_active ) { _chain_intr( old16 ); return; }

        /* eigenes blockierendes Warten: STI, bis BIOS-Puffer eine Taste hat */
        _enable();
        while ( bios_peek_head() == bios_peek_tail() ) { /* warten */ }
        key = bios_take_key();

        if ( (key & 0xFF) == 0x09 && readline_active ) {   /* TAB           */
            do_complete_queue();
            if ( !Q_EMPTY() ) { r.w.ax = q_pop(); return; }
            r.w.ax = key;
            return;
        }
        else if ( (key & 0xFF) == 0x0D ) {                 /* ENTER         */
            readline_active = 0;
            comp_active = 0; comp_base_len = 0; shown_len = 0;
        }
        else if ( (key & 0xFF) == 0x08 ) {                 /* Backspace     */
            comp_active = 0;
            if ( comp_base_len > 0 ) comp_base_len--;
        }
        else if ( (key & 0xFF) >= 0x20 && (key & 0xFF) < 0x7F ) { /* Printbl */
            comp_active = 0;
            if ( comp_base_len < NAME_LEN - 1 )
                comp_base[comp_base_len++] = (char)(key & 0xFF);
        }
        else {
            comp_active = 0;                               /* Sondertasten  */
        }

        r.w.ax = key;
        return;
    }

    /* AH=01h (Status) UND AH=11h (Enhanced-Status): wenn unsere Queue noch
       Completion-Zeichen enthaelt, MUESSEN wir "Taste verfuegbar" melden —
       sonst pollt DOS/DOSKEY den BIOS-Puffer (leer) und holt den Rest des
       Dateinamens nie ab (halb eingefuegter Name / scheinbarer Haenger). */
    if ( ah == 0x01 || ah == 0x11 ) {
        if ( !Q_EMPTY() ) {
            r.w.ax     = kbd_queue[q_head];   /* Peek, nicht poppen         */
            r.w.flags &= ~0x0040;             /* ZF=0 -> Taste verfuegbar   */
            return;
        }
        _chain_intr( old16 );                 /* sonst BIOS fragen          */
        return;
    }

    _chain_intr( old16 );                     /* alle anderen Funktionen    */
}

/* -------- INT 21h Hook: scannt Verzeichnis, reicht Zeileneingabe weiter - */

void __interrupt __far new21( union INTPACK r )
{
    /* Nur scannen, wenn DOS NICHT re-entrant ist (InDOS==0). Sonst wuerden
       unsere DOS-Calls in scan_directory den DOS-Zustand korrumpieren. */
    if ( r.h.ah == 0x0A && *indos_ptr == 0 ) {
        scan_directory();
        readline_active = 1;
        comp_active = 0; comp_base_len = 0; shown_len = 0; comp_index = 0;
    }
    _chain_intr( old21 );
}

/* -------- Installation --------------------------------------------------*/

static void con_out( char c )
{
    union REGS r;
    r.h.ah = 0x02;
    r.h.dl = (unsigned char)c;
    intdos( &r, &r );
}
static void msg( char *s ) { while ( *s ) con_out( *s++ ); }

int main( void )
{
    union REGS r; struct SREGS s;
    unsigned para;

    msg( "TABTSR v0.2.2 - TAB-Dateinamen-Completion fuer DOS\r\n" );
    msg( "INT 21h/0Ah + INT 16h (00h/10h/01h/11h) gehookt, jetzt resident.\r\n" );

    /* InDOS-Flag-Zeiger holen (ES:BX) fuer Re-Entranz-Schutz in new21 */
    segread( &s );
    r.h.ah = 0x34;
    intdosx( &r, &r, &s );
    indos_ptr = (unsigned char far *)MK_FP( s.es, r.x.bx );

    old21 = _dos_getvect( 0x21 );
    old16 = _dos_getvect( 0x16 );
    _dos_setvect( 0x21, new21 );
    _dos_setvect( 0x16, new16 );

    para = (get_ss() - _psp) + (get_sp() / 16) + 16;

    _dos_keep( 0, para );
    return 0;
}
