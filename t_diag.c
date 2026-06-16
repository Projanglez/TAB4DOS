/*
 * T_DIAG - Diagnose-TSR: lokalisiert den Haenger im Editor-Ersatz.
 *
 * Schreibt Marker DIREKT ins Video-RAM (B800 + B000, ohne DOS/BIOS), damit
 * die Hardware selbst zeigt, wie weit der Code kommt. Liest Tasten per
 * DIREKTEM INT 16h (nicht int86), um die Lib als Ursache auszuschliessen.
 *
 * Marker oben links (Spalten 0,2,4,6,8):
 *   Spalte 0 = 'A' : INT-21h-Hook fuer AH=0Ah betreten
 *   Spalte 2 = 'R' : im do_readline angekommen
 *   Spalte 4 = 'K' : VOR dem Tasten-Lesen
 *   Spalte 6 = '+' : Tasten-Lesen ZURUECKGEKEHRT
 *   Spalte 8 = <Zeichen> : gelesenes Zeichen
 *   Spalte 0 = 'Z' : do_readline sauber beendet (ENTER)
 *
 * Auswertung nach dem Haenger:
 *   nur 'A'            -> Haenger in do_readline-Setup oder direkt davor
 *   'A R K' aber kein '+' (auch nach vielen Tasten) -> INT 16h kehrt nie zurueck
 *   'A R K + <Zeichen>' aber kein Echo + Haenger     -> con_out (INT 21h/02h)
 *   gar kein 'A'        -> Hook wird fuer AH=0Ah nicht aufgerufen
 *
 * Build: wcl -bt=dos -ms -os -s -zq -fe=t_diag.exe t_diag.c
 */

#include <dos.h>
#include <i86.h>

extern unsigned _psp;

unsigned get_ss( void );
#pragma aux get_ss = "mov ax, ss" value [ax];
unsigned get_sp( void );
#pragma aux get_sp = "mov ax, sp" value [ax];

unsigned bios_key( void );             /* direkter INT 16h/00h, KEIN int86 */
#pragma aux bios_key = \
    "mov ah,0"  \
    "int 0x16"  \
    value [ax];

static void (__interrupt __far *old21)();

static void poke( int col, char ch )
{
    *(unsigned char far *)MK_FP( 0xB800, col*2     ) = (unsigned char)ch;
    *(unsigned char far *)MK_FP( 0xB800, col*2 + 1 ) = 0x1F;   /* color text */
    *(unsigned char far *)MK_FP( 0xB000, col*2     ) = (unsigned char)ch;
    *(unsigned char far *)MK_FP( 0xB000, col*2 + 1 ) = 0x70;   /* mono text  */
}

static void con_out( char c )
{
    union REGS r;
    r.h.ah = 0x02;
    r.h.dl = (unsigned char)c;
    intdos( &r, &r );
}
static void msg( char *s ) { while ( *s ) con_out( *s++ ); }

static void do_readline( unsigned bseg, unsigned boff )
{
    char far *buf = (char far *)MK_FP( bseg, boff );
    int maxlen = (unsigned char)buf[0];
    int len = 0;
    unsigned k; unsigned char a;

    poke( 2, 'R' );
    for ( ;; ) {
        poke( 4, 'K' );
        k = bios_key();
        poke( 6, '+' );
        a = (unsigned char)( k & 0xFF );
        poke( 8, (char)a );

        if ( a == 0x0D ) {
            buf[1] = (char)len; buf[2+len] = 0x0D;
            con_out(0x0D); con_out(0x0A);
            return;
        }
        else if ( a == 0x08 ) {
            if ( len > 0 ) { len--; con_out(8); con_out(' '); con_out(8); }
        }
        else if ( a >= 0x20 && a < 0x7F ) {
            if ( len < maxlen - 1 ) { buf[2+len] = a; con_out(a); len++; }
        }
    }
}

void __interrupt __far new21( union INTPACK r )
{
    if ( r.h.ah == 0x0A ) {
        poke( 0, 'A' );
        do_readline( r.w.ds, r.w.dx );
        poke( 0, 'Z' );
        return;
    }
    _chain_intr( old21 );
}

int main( void )
{
    unsigned para;

    msg( "T_DIAG - Marker oben links: 0=A 2=R 4=K 6=+ 8=Zeichen\r\n" );
    msg( "Tippen; danach Bildschirm-Ecke ablesen.\r\n" );

    old21 = _dos_getvect( 0x21 );
    _dos_setvect( 0x21, new21 );

    para = (get_ss() - _psp) + (get_sp() / 16) + 16;
    _dos_keep( 0, para );
    return 0;
}
