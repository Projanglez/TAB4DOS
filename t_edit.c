/*
 * T_EDIT - Minimal-Validierung des Editor-Ersatz-Mechanismus (TABTSR v0.3)
 *
 * Zweck: ISOLIERT testen, ob wir INT 21h/AH=0Ah selbst behandeln koennen:
 *   - Tasten via INT 16h/00h lesen (als Aufrufer, KEIN Hook)
 *   - via INT 21h/02h echoen
 *   - Zeile in COMMAND.COMs Puffer schreiben, dann IRET (kein chain)
 *
 * KEIN Verzeichnis-Scan, KEINE Completion. Nur: Tippen, Backspace, ENTER.
 * TAB/Pfeile werden ignoriert.
 *
 * Wenn das auf echter HW (F5-Boot) funktioniert -> Fundament fuer v0.3 ist
 * bestaetigt. Wenn nicht -> der Editor-Ersatz traegt auf dieser HW nicht.
 *
 * Build:  call %WATCOM%\owsetenv.bat & set PATH=%WATCOM%\binnt64;%PATH%
 *         wcl -bt=dos -ms -os -zq -fe=t_edit.exe t_edit.c
 */

#include <dos.h>
#include <i86.h>

extern unsigned _psp;

unsigned get_ss( void );
#pragma aux get_ss = "mov ax, ss" value [ax];
unsigned get_sp( void );
#pragma aux get_sp = "mov ax, sp" value [ax];

static void (__interrupt __far *old21)();
static unsigned char far *indos_ptr = 0;

static void con_out( char c )
{
    union REGS r;
    r.h.ah = 0x02;
    r.h.dl = (unsigned char)c;
    intdos( &r, &r );
}
static void msg( char *s ) { while ( *s ) con_out( *s++ ); }

static unsigned get_key( void )
{
    union REGS r;
    r.h.ah = 0x00;
    int86( 0x16, &r, &r );
    return r.w.ax;
}

static void do_readline( unsigned bseg, unsigned boff )
{
    char far *buf = (char far *)MK_FP( bseg, boff );
    int maxlen = (unsigned char)buf[0];
    int len = 0;
    unsigned k; unsigned char a;

    for ( ;; ) {
        k = get_key();
        a = (unsigned char)( k & 0xFF );

        if ( a == 0x0D ) {                        /* ENTER */
            buf[1] = (char)len;
            buf[2+len] = 0x0D;
            con_out(0x0D); con_out(0x0A);
            return;
        }
        else if ( a == 0x08 ) {                   /* Backspace */
            if ( len > 0 ) { len--; con_out(8); con_out(' '); con_out(8); }
        }
        else if ( a >= 0x20 && a < 0x7F ) {       /* druckbar */
            if ( len < maxlen - 1 ) { buf[2+len] = a; con_out(a); len++; }
        }
        /* TAB / Extended / sonstiges: ignorieren */
    }
}

void __interrupt __far new21( union INTPACK r )
{
    if ( r.h.ah == 0x0A && *indos_ptr == 0 ) {
        do_readline( r.w.ds, r.w.dx );
        return;
    }
    _chain_intr( old21 );
}

int main( void )
{
    union REGS r; struct SREGS s;
    unsigned para;

    msg( "T_EDIT - Minimal-Editor-Test (nur INT 21h/0Ah)\r\n" );
    msg( "Tippen + Backspace + ENTER muss gehen. TAB/Pfeile egal.\r\n" );

    segread( &s );
    r.h.ah = 0x34;
    intdosx( &r, &r, &s );
    indos_ptr = (unsigned char far *)MK_FP( s.es, r.x.bx );

    old21 = _dos_getvect( 0x21 );
    _dos_setvect( 0x21, new21 );

    para = (get_ss() - _psp) + (get_sp() / 16) + 16;

    _dos_keep( 0, para );
    return 0;
}
