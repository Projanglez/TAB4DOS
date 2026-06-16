/*
 * TABTSR - TAB-Dateinamen-Completion fuer MS-DOS
 * v0.3  -  Open Watcom C (16-bit, Real Mode)
 *
 * Architektur (siehe CLAUDE.md)
 * -----------------------------
 * Eigenstaendiger Zeileneditor. Wir hooken NUR INT 21h / AH=0Ah und
 * uebernehmen die Zeileneingabe komplett selbst (return == IRET, KEIN chain
 * an DOS). Tasten lesen wir als ganz normaler Aufrufer von INT 16h (BIOS
 * blockiert fuer uns) - wir hooken INT 16h NICHT. Damit entfaellt der ganze
 * v0.2-Schmerz (Busy-Wait, volatile, DOSKEY-Bypass).
 *
 * Warum das traegt (beides auf echter 386er-HW erprobt):
 *   - Tasten als INT-16h-Aufrufer lesen: v0.1 konnte damit normal tippen.
 *   - scan_directory (FindFirst am AH=0Ah-Eingang, InDOS==0): in v0.2 Test 2
 *     lief der Prompt damit sauber.
 * Neu/sicher: TAB sucht nur im bereits gescannten file_cache - KEIN DOS-Call
 * im heissen Pfad (das war v0.1s Absturzursache).
 *
 * v0.3 ist append-only (kein Cursor/Mid-Line-Editing), Basename im aktuellen
 * Verzeichnis. History + Editing folgen als naechste Inkremente (DOSKEY-Ersatz).
 *
 * Build: build.bat   Test: ZUERST DOSBox (nur Lade-Check!), dann echter 386.
 */

#include <dos.h>
#include <i86.h>

#define TABTSR_VERSION "0.3.4"        /* bei jedem Build letzte Stelle +1   */
#define DEBUG_SCAN     1              /* 1 = file_count oben rechts anzeigen */

extern unsigned _psp;

unsigned get_ss( void );
#pragma aux get_ss = "mov ax, ss" value [ax];
unsigned get_sp( void );
#pragma aux get_sp = "mov ax, sp" value [ax];

/* -------- resident: Hook-Pointer + InDOS-Flag --------------------------- */

static void (__interrupt __far *old21)();
static unsigned char far *indos_ptr = 0;   /* INT 21h/34h, Re-Entranz-Schutz */

/* -------- resident: Dateiname-Cache (von scan_directory gefuellt) ------- */

#define MAX_FILES 64
#define NAME_LEN  13                  /* 8.3 + NUL                          */

static char          file_cache[MAX_FILES][NAME_LEN];
static unsigned char file_is_dir[MAX_FILES];
static int           file_count;

/* DTA-Puffer MUSS global sein (DGROUP)! Im Interrupt-Hook ist SS != DS;
   ein Stack-Array + (void far*)-Cast wuerde DS statt SS nehmen -> DOS
   schreibt woanders hin als wir lesen (las Stack-Muell). */
static unsigned char dta_buf[64];

/* -------- resident: Completion-Zustand ----------------------------------*/

static char comp_base[NAME_LEN];      /* getippter Stem (Praefix)           */
static int  comp_base_len = 0;
static int  comp_active    = 0;       /* 1 = wir zykeln gerade durch Treffer */
static int  comp_index     = 0;
static int  shown_len      = 0;       /* aktuell auf dem Schirm: Stem o. Name */

/* -------- kleine lokale String-Helfer (kein <string.h> im Resident) ----- */

static int strlen_local( const char *s )
{
    int n = 0;
    while ( s[n] ) n++;
    return n;
}
/* case-insensitiv: file_cache ist GROSS (DTA), comp_base meist klein.
   FAT16-Case-Insensitivity gilt nur fuer DOS' eigenen Vergleich. */
static int memcmp_local( const char *a, const char *b, int n )
{
    int i; char ca, cb;
    for ( i = 0; i < n; i++ ) {
        ca = a[i]; cb = b[i];
        if ( ca >= 'a' && ca <= 'z' ) ca -= 0x20;
        if ( cb >= 'a' && cb <= 'z' ) cb -= 0x20;
        if ( ca != cb ) return 1;
    }
    return 0;
}

/* -------- Konsolen-Ausgabe via BIOS INT 10h/0Eh (Teletype) --------------
   NICHT DOS AH=02h: das wuerde unseren eigenen INT-21h-Hook erneut betreten
   (nested) und ueber _chain_intr laufen - dabei kam bei DOS ein falsches
   Zeichen an (Echo zeigte 0xDB statt der Taste). BIOS INT 10h umgeht den
   Hook komplett (wie INT 16h beim Tasten-Lesen). 0x07=BEL piept. */
void con_out( char c );
#pragma aux con_out =   \
    "mov ah,0x0E"       \
    "mov bx,0x0007"     \
    "int 0x10"          \
    parm [al]           \
    modify [ah bx];
static void msg( char *s ) { while ( *s ) con_out( *s++ ); }

/* -------- Debug: direkt ins Video-RAM schreiben (kein DOS/BIOS) --------- */
#if DEBUG_SCAN
static void poke_ch( int col, char ch )
{
    *(unsigned char far *)MK_FP( 0xB800, col*2     ) = (unsigned char)ch;
    *(unsigned char far *)MK_FP( 0xB800, col*2 + 1 ) = 0x4F;   /* weiss/rot */
    *(unsigned char far *)MK_FP( 0xB000, col*2     ) = (unsigned char)ch;
    *(unsigned char far *)MK_FP( 0xB000, col*2 + 1 ) = 0x70;
}
static void poke_hex( int col, unsigned char v )
{
    static char hx[] = "0123456789ABCDEF";
    poke_ch( col,   hx[(v >> 4) & 0x0F] );
    poke_ch( col+1, hx[v & 0x0F] );
}
#endif

/* -------- Taste lesen via INT 16h/00h (wir sind Aufrufer, kein Hook) ----
   DIREKTES INT 16h, NICHT int86: int86 muss eine variable Int-Nummer
   ausfuehren (Dispatch/selbstmodifizierend) und versagt im residenten
   Interrupt-Kontext - Taste wurde nicht konsumiert, Puffer lief voll.
   intdos/intdosx (feste Nummer 21h) sind dagegen ok. */
unsigned get_key( void );
#pragma aux get_key =   \
    "mov ah,0"          \
    "int 0x16"          \
    value [ax];         /* AL=ASCII, AH=Scancode */

/* -------- Direkte INT-21h-Helfer (feste Opcodes, NICHT intdos/intdosx) ---
   intdos/intdosx versagen wie int86 im residenten Kontext (Int-Dispatch-
   Wrapper). Hier per #pragma aux mit literalem "int 0x21". DOS-Carry kommt
   ueber sbb ax,ax (CF -> 0/0xFFFF) zurueck. Far-Pointer in DX:AX. */

void far *dos_get_dta( void );              /* AH=2Fh -> DTA (ES:BX)        */
#pragma aux dos_get_dta =   \
    "mov ah,0x2F"           \
    "int 0x21"              \
    "mov ax,bx"             \
    "mov dx,es"             \
    value  [dx ax]          \
    modify [bx cx si di es];

void dos_set_dta( void far *p );            /* AH=1Ah, DS:DX = p            */
#pragma aux dos_set_dta =   \
    "push ds"               \
    "mov ds,dx"             \
    "mov dx,ax"             \
    "mov ah,0x1A"           \
    "int 0x21"              \
    "pop ds"                \
    parm   [dx ax]          \
    modify [ax bx cx si di es];

unsigned dos_findfirst( void far *pat, unsigned attr );  /* AH=4Eh, 0=ok    */
#pragma aux dos_findfirst = \
    "push ds"               \
    "mov ds,dx"             \
    "mov dx,ax"             \
    "mov ah,0x4E"           \
    "int 0x21"              \
    "pop ds"                \
    "sbb ax,ax"             \
    parm   [dx ax] [cx]     \
    value  [ax]             \
    modify [bx cx dx si di es];

unsigned dos_findnext( void );              /* AH=4Fh, 0=ok                 */
#pragma aux dos_findnext =  \
    "mov ah,0x4F"           \
    "int 0x21"              \
    "sbb ax,ax"             \
    value  [ax]             \
    modify [bx cx dx si di es];

/* -------- Verzeichnis-Scan: einmal am Eingang von do_readline ----------- */

static void scan_directory( void )
{
    void far *save_dta;
    unsigned ok;

    file_count = 0;
    save_dta = dos_get_dta();
    dos_set_dta( (void far *)dta_buf );          /* global -> DS=DGROUP ok  */

    ok = dos_findfirst( (void far *)"*.*", 0x10 );   /* inkl. Verzeichnisse */
    while ( ok == 0 && file_count < MAX_FILES ) {
        if ( dta_buf[30] != '.' ) {                  /* "." / ".." weglassen */
            int i;
            for ( i = 0; i < NAME_LEN - 1 && dta_buf[30+i]; i++ )
                file_cache[file_count][i] = dta_buf[30+i];
            file_cache[file_count][i] = 0;
            file_is_dir[file_count] = (dta_buf[21] & 0x10) ? 1 : 0;
            file_count++;
        }
        ok = dos_findnext();
    }

    dos_set_dta( save_dta );
}

/* -------- TAB-Completion: sucht im Cache, schreibt direkt in Puffer+Schirm */

/* len per WERT rein und raus! NICHT als Pointer: &len waere ein near-Pointer
   auf eine Stack-Variable, der ueber DS dereferenziert wird - im Hook ist aber
   SS != DS, also laese do_complete Muell (war L=0xCC statt 2). */
static int do_complete( unsigned bseg, unsigned boff, int len )
{
    char far *buf = (char far *)MK_FP( bseg, boff );
    int maxlen = (unsigned char)buf[0];
    int stem_start, i, target, count, matched, namelen, addlen, avail;

    /* Bei frischer Completion (nicht am Zykeln): Stem = letztes Token
       (zurueck bis Space oder Backslash) aus dem aktuellen Puffer ziehen. */
    if ( !comp_active ) {
        stem_start = len;
        while ( stem_start > 0 &&
                buf[2+stem_start-1] != ' ' && buf[2+stem_start-1] != '\\' )
            stem_start--;
        comp_base_len = len - stem_start;
        if ( comp_base_len > NAME_LEN - 1 ) comp_base_len = NAME_LEN - 1;
        for ( i = 0; i < comp_base_len; i++ )
            comp_base[i] = buf[2+stem_start+i];
        comp_index = 0;
        shown_len  = comp_base_len;   /* der getippte Stem ist gerade sichtbar */
    }

#if DEBUG_SCAN
    /* Zeile 2: "BL=NN xy L=MM" -> comp_base_len, comp_base[0..1], len */
    poke_ch(140,'B'); poke_ch(141,'L'); poke_ch(142,'=');
    poke_hex(143,(unsigned char)comp_base_len);
    poke_ch(146, (char)(comp_base_len > 0 ? comp_base[0] : '-'));
    poke_ch(147, (char)(comp_base_len > 1 ? comp_base[1] : '-'));
    poke_ch(149,'L'); poke_ch(150,'=');
    poke_hex(151,(unsigned char)len);
#endif

    /* Treffer Nr. comp_index suchen (Praefix comp_base, case-insensitiv) */
    target = comp_index; matched = -1; count = 0;
    for ( i = 0; i < file_count; i++ ) {
        if ( comp_base_len == 0 ||
             ( strlen_local(file_cache[i]) >= comp_base_len &&
               memcmp_local(file_cache[i], comp_base, comp_base_len) == 0 ) ) {
            if ( count == target ) { matched = i; break; }
            count++;
        }
    }
    if ( matched < 0 && target > 0 ) {            /* ueber Ende: wrap        */
        comp_index = 0;
        for ( i = 0; i < file_count; i++ ) {
            if ( comp_base_len == 0 ||
                 ( strlen_local(file_cache[i]) >= comp_base_len &&
                   memcmp_local(file_cache[i], comp_base, comp_base_len) == 0 ) ) {
                matched = i; break;
            }
        }
    }
    if ( matched < 0 ) { con_out( 0x07 ); comp_active = 0; return len; } /* Beep */

    namelen = strlen_local( file_cache[matched] );
    addlen  = namelen + ( file_is_dir[matched] ? 1 : 0 );

    /* Passt der Treffer (nach Loeschen des sichtbaren Teils) in den Puffer? */
    avail = (maxlen - 1) - (len - shown_len);
    if ( addlen > avail ) { con_out( 0x07 ); comp_active = 0; return len; }

    for ( i = 0; i < shown_len; i++ )             /* sichtbaren Teil loeschen */
        { con_out(0x08); con_out(' '); con_out(0x08); }
    len -= shown_len;

    for ( i = 0; i < namelen; i++ ) {             /* Treffer schreiben       */
        buf[2+len] = file_cache[matched][i];
        con_out( file_cache[matched][i] );
        len++;
    }
    if ( file_is_dir[matched] ) { buf[2+len] = '\\'; con_out('\\'); len++; }

    shown_len   = addlen;
    comp_active = 1;
    comp_index++;
    return len;
}

/* -------- eigener Zeilen-Editor (haengt im AH=0Ah-Hook) ----------------- */

static void do_readline( unsigned bseg, unsigned boff )
{
    char far *buf = (char far *)MK_FP( bseg, boff );
    int maxlen = (unsigned char)buf[0];
    int len = 0;
    unsigned k; unsigned char ascii;

    scan_directory();                 /* sicher: InDOS==0, flacher Stack    */
    comp_active = 0; comp_base_len = 0; shown_len = 0; comp_index = 0;

#if DEBUG_SCAN
    /* oben rechts: "FC=NN c" -> NN=file_count(hex), c=1.Zeichen 1.Datei */
    poke_ch(60,'F'); poke_ch(61,'C'); poke_ch(62,'=');
    poke_hex(63, (unsigned char)file_count);
    poke_ch(66, (char)(file_count > 0 ? file_cache[0][0] : '-'));
#endif

    for ( ;; ) {
        k = get_key();
        ascii = (unsigned char)( k & 0xFF );

        if ( ascii == 0x0D ) {                    /* ENTER                  */
            buf[1] = (char)len;
            buf[2+len] = 0x0D;
            con_out(0x0D); con_out(0x0A);
            return;
        }
        else if ( ascii == 0x08 ) {               /* Backspace              */
            if ( len > 0 ) {
                len--;
                con_out(0x08); con_out(' '); con_out(0x08);
            }
            comp_active = 0;
        }
        else if ( ascii == 0x09 ) {               /* TAB                    */
            len = do_complete( bseg, boff, len );
        }
        else if ( ascii >= 0x20 && ascii < 0x7F ) { /* druckbar             */
            if ( len < maxlen - 1 ) {
                buf[2+len] = ascii;
                con_out( ascii );
                len++;
            }
            comp_active = 0;
        }
        else {
            comp_active = 0;                       /* Extended/ignoriert     */
        }
    }
}

/* -------- INT 21h Hook: AH=0Ah selbst behandeln, sonst chainen ---------- */

void __interrupt __far new21( union INTPACK r )
{
    if ( r.h.ah == 0x0A && *indos_ptr == 0 ) {
        do_readline( r.w.ds, r.w.dx );
        return;                       /* IRET - wir haben 0Ah erledigt      */
    }
    _chain_intr( old21 );
}

/* -------- Installation --------------------------------------------------*/

int main( void )
{
    union REGS r; struct SREGS s;
    unsigned para;

    msg( "TABTSR v" TABTSR_VERSION " - TAB-Completion fuer DOS\r\n" );
    msg( "Eigener Editor, nur INT 21h/0Ah gehookt. Jetzt resident.\r\n" );

    segread( &s );                    /* InDOS-Flag-Zeiger holen (ES:BX)    */
    r.h.ah = 0x34;
    intdosx( &r, &r, &s );
    indos_ptr = (unsigned char far *)MK_FP( s.es, r.x.bx );

    old21 = _dos_getvect( 0x21 );
    _dos_setvect( 0x21, new21 );

    para = (get_ss() - _psp) + (get_sp() / 16) + 16;

    _dos_keep( 0, para );
    return 0;
}
