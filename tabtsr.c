/*
 * TABTSR - Minimaler TSR fuer TAB-Dateinamen-Completion unter MS-DOS
 * v0.1  -  Open Watcom C (16-bit, Real Mode)
 *
 * Idee
 * ----
 * COMMAND.COM liest seine Eingabezeile ueber INT 21h / AH=0Ah
 * (Buffered Input). Wir hooken INT 21h, fangen genau diese Funktion ab
 * und implementieren einen eigenen Zeilen-Editor. Bei TAB wird das
 * aktuelle Wort als Dateiname (8.3) im aktuellen Verzeichnis
 * vervollstaendigt; wiederholtes TAB zykelt durch die Treffer (wie 4DOS).
 *
 * Wichtig: Beim Eintritt in unseren 0Ah-Hook ist InDOS noch 0, weil wir
 * INT 21h *vor* dem eigentlichen DOS-Handler abfangen. Deshalb duerfen
 * wir hier gefahrlos selbst INT-21h-Funktionen (02h, 4Eh/4Fh, 1Ah, 2Fh)
 * aufrufen.
 *
 * Build:
 *   wcl -bt=dos -ms -os -zq -fe=tabtsr.exe tabtsr.c
 *
 * Test ZUERST in DOSBox-X oder einer VM, dann auf echter Hardware.
 * Ein fehlerhafter INT-21h-Hook kann den Rechner haengen lassen.
 */

#include <dos.h>
#include <i86.h>

/* -------- Watcom-Globals / Inline-Helfer ------------------------------ */

extern unsigned _psp;                 /* Segment des PSP */

unsigned get_ss( void );
#pragma aux get_ss = "mov ax, ss" value [ax];
unsigned get_sp( void );
#pragma aux get_sp = "mov ax, sp" value [ax];

/* -------- resident: Zustand ------------------------------------------- */

static void (__interrupt __far *old21)();
static int  busy = 0;                 /* Reentranz-Schutz fuer 0Ah        */

static unsigned char dta[64];         /* eigener DTA-Puffer (FindFirst)    */
#define DTA_ATTR (dta[21])
#define DTA_NAME ((char far *)&dta[30])

static char comp_base[16];            /* urspruenglich getipptes Fragment  */
static int  comp_active = 0;          /* laeuft gerade ein TAB-Zyklus?     */
static int  comp_index  = 0;          /* welcher Treffer als naechstes     */
static int  shown_len   = 0;          /* Laenge des aktuell gezeigten Worts*/

/* -------- Forward-Deklarationen --------------------------------------- */

static void do_complete( char far *data, int *plen, unsigned char maxlen );

/* -------- Konsole / Tastatur ------------------------------------------ */

static void con_out( char c )         /* INT 21h AH=02h: Zeichen ausgeben  */
{
    union REGS r;
    r.h.ah = 0x02;
    r.h.dl = (unsigned char)c;
    intdos( &r, &r );
}

static void msg( char *s )            /* einfache String-Ausgabe           */
{
    while ( *s ) con_out( *s++ );
}

static unsigned get_key( void )       /* INT 16h AH=00h: Taste lesen       */
{
    union REGS r;
    r.h.ah = 0x00;
    int86( 0x16, &r, &r );            /* AL=ASCII, AH=Scancode             */
    return r.x.ax;
}

/* -------- DTA / Verzeichnissuche -------------------------------------- */

static void get_dta( unsigned *seg, unsigned *off )   /* INT 21h AH=2Fh    */
{
    union REGS r; struct SREGS s;
    r.h.ah = 0x2F;
    intdosx( &r, &r, &s );
    *seg = s.es; *off = r.x.bx;
}

static void set_dta( unsigned seg, unsigned off )     /* INT 21h AH=1Ah    */
{
    union REGS r; struct SREGS s;
    segread( &s );
    r.h.ah = 0x1A;
    s.ds = seg; r.x.dx = off;
    intdosx( &r, &r, &s );
}

static int find_first( char far *pattern )            /* INT 21h AH=4Eh    */
{
    union REGS r; struct SREGS s;
    char far *fdta = (char far *)dta;

    set_dta( FP_SEG(fdta), FP_OFF(fdta) );
    segread( &s );
    s.ds   = FP_SEG(pattern);
    r.x.dx = FP_OFF(pattern);
    r.h.ah = 0x4E;
    r.x.cx = 0x10;                    /* Normal + Verzeichnisse            */
    intdosx( &r, &r, &s );
    return (r.x.cflag == 0);
}

static int find_next( void )                          /* INT 21h AH=4Fh    */
{
    union REGS r;
    r.h.ah = 0x4F;
    intdos( &r, &r );
    return (r.x.cflag == 0);
}

/* -------- TAB-Completion ---------------------------------------------- */

static void do_complete( char far *data, int *plen, unsigned char maxlen )
{
    unsigned save_seg, save_off;
    char pattern[20];
    char namebuf[16];
    int  wstart, wlen, i, k, target, count, nl;
    int  matched = 0;

    /* aktuelles Wort = letzter Lauf ohne Leerzeichen */
    wstart = *plen;
    while ( wstart > 0 && data[wstart-1] != ' ' ) wstart--;
    wlen = *plen - wstart;

    if ( !comp_active ) {                     /* neuen Zyklus starten      */
        if ( wlen > 12 ) return;              /* zu lang fuers 8.3-Muster  */
        for ( i = 0; i < wlen; i++ ) comp_base[i] = data[wstart+i];
        comp_base[wlen] = 0;
        comp_index  = 0;
        shown_len   = wlen;
        comp_active = 1;
    }

    /* Suchmuster bilden: <fragment>*   (leer -> *) */
    k = 0;
    for ( i = 0; comp_base[i]; i++ ) pattern[k++] = comp_base[i];
    pattern[k++] = '*';
    pattern[k]   = 0;

    /* DTA sichern, Treffer Nr. comp_index suchen */
    get_dta( &save_seg, &save_off );

    target = comp_index;
    if ( find_first( (char far *)pattern ) ) {
        count = 0;
        for (;;) {
            if ( DTA_NAME[0] != '.' ) {       /* "." / ".." ueberspringen  */
                if ( count == target ) { matched = 1; break; }
                count++;
            }
            if ( !find_next() ) break;
        }
        if ( !matched && target > 0 ) {       /* ueber Ende -> Anfang      */
            comp_index = 0;
            if ( find_first( (char far *)pattern ) ) {
                for (;;) {
                    if ( DTA_NAME[0] != '.' ) { matched = 1; break; }
                    if ( !find_next() ) break;
                }
            }
        }
    }

    set_dta( save_seg, save_off );            /* COMMAND.COM-DTA zurueck   */

    if ( !matched ) { con_out( 0x07 ); return; }   /* kein Treffer: piep   */

    /* altes angezeigtes Wort vom Schirm + Puffer loeschen */
    for ( i = 0; i < shown_len; i++ ) {
        con_out( 0x08 ); con_out( ' ' ); con_out( 0x08 );
    }
    *plen -= shown_len;

    /* Treffer einfuegen (bei Verzeichnis ein '\' anhaengen) */
    nl = 0;
    for ( i = 0; DTA_NAME[i] && nl < 13; i++ ) namebuf[nl++] = DTA_NAME[i];
    if ( DTA_ATTR & 0x10 ) namebuf[nl++] = '\\';
    namebuf[nl] = 0;

    for ( i = 0; i < nl; i++ ) {
        if ( *plen < (int)maxlen - 1 ) {
            data[(*plen)++] = namebuf[i];
            con_out( namebuf[i] );
        }
    }
    shown_len = nl;
    comp_index++;
}

/* -------- eigener 0Ah-Zeilen-Editor ----------------------------------- */

static void do_readline( char far *buf )
{
    unsigned char maxlen = (unsigned char)buf[0];   /* max Zeichen        */
    char far *data = buf + 2;                        /* Nutzdaten ab Off 2 */
    int len = 0;
    unsigned key;

    for (;;) {
        key = get_key();

        if ( (key & 0xFF) == 0x0D ) {                /* ENTER              */
            data[len] = 0x0D;                        /* CR-Konvention 0Ah  */
            buf[1] = (unsigned char)len;
            con_out( 0x0D ); con_out( 0x0A );
            return;
        }
        else if ( (key & 0xFF) == 0x08 ) {           /* BACKSPACE          */
            comp_active = 0;
            if ( len > 0 ) {
                len--;
                con_out( 0x08 ); con_out( ' ' ); con_out( 0x08 );
            }
        }
        else if ( (key & 0xFF) == 0x09 ) {           /* TAB -> Completion  */
            do_complete( data, &len, maxlen );
        }
        else if ( (key & 0xFF) >= 0x20 && (key & 0xFF) < 0x7F ) {
            comp_active = 0;                         /* druckbares Zeichen */
            if ( len < (int)maxlen - 1 ) {
                data[len++] = (char)(key & 0xFF);
                con_out( (char)(key & 0xFF) );
            } else {
                con_out( 0x07 );                     /* Puffer voll: piep  */
            }
        }
        /* Pfeile / Sondertasten werden in v0.1 ignoriert */
    }
}

/* -------- INT-21h-Hook ------------------------------------------------ */

void __interrupt __far new21( union INTPACK r )
{
    /* HINWEIS: INTPACK-Member ggf. an deine OW-Version anpassen
       (r.w.ds / r.w.dx in 16-bit Watcom).                              */
    if ( r.h.ah == 0x0A && !busy ) {
        busy = 1;
        do_readline( (char far *)MK_FP( r.w.ds, r.w.dx ) );
        busy = 0;
        return;                          /* IRET, NICHT weiterreichen     */
    }
    _chain_intr( old21 );                /* alle anderen 21h-Funktionen   */
}

/* -------- Installation ------------------------------------------------ */

int main( void )
{
    unsigned para;

    msg( "TABTSR v0.1 - TAB-Dateinamen-Completion fuer DOS\r\n" );
    msg( "INT 21h/0Ah gehookt, jetzt resident.\r\n" );

    old21 = _dos_getvect( 0x21 );
    _dos_setvect( 0x21, new21 );

    /* Zu behaltende Paragraphen: PSP .. oberes Ende des Stacksegments.
       (get_ss()-_psp) deckt PSP + Code + Daten ab, (get_sp()/16) den
       benutzten Stack; +16 als Sicherheitsmarge. RISKANTESTE ZEILE -
       bei Instabilitaet zuerst hier pruefen.                            */
    para = (get_ss() - _psp) + (get_sp() / 16) + 16;

    _dos_keep( 0, para );
    return 0;                            /* wird nie erreicht             */
}
