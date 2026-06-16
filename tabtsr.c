/*
 * TABTSR - TAB-Dateinamen-Completion fuer MS-DOS
 * v0.7  -  Open Watcom C (16-bit, Real Mode)
 *
 * Architektur: eigenstaendiger Zeileneditor, hooken NUR INT 21h / AH=0Ah.
 * Tasten via INT 16h (Aufrufer, kein Hook). InDOS==0 beim Eintritt garantiert.
 * SS != DS im Hook: alle Puffer global (DGROUP). Kein int86/intdos im Resident.
 *
 * Features: TAB-Completion (Dateien, Verzeichnisse, Pfade, DOS-Befehle, PATH),
 * Command-History (Pfeil Hoch/Runter), Ctrl+R Rueckwaertssuche, Mid-Line-Editing
 * (Pfeil Links/Rechts, Home, End, Del, Ins, Ctrl+Links/Rechts). /u Deinstallation.
 *
 * Build: build.bat   Test: DOSBox (Lade-Check), dann echter 386.
 */

#include <dos.h>
#include <i86.h>

#define TABTSR_VERSION "0.7.1"

extern unsigned _psp;

unsigned get_ss( void );
#pragma aux get_ss = "mov ax, ss" value [ax];
unsigned get_sp( void );
#pragma aux get_sp = "mov ax, sp" value [ax];

/* -------- Hook + InDOS -------------------------------------------------- */
static void (__interrupt __far *old21)();
static unsigned char far *indos_ptr = 0;

/* -------- /u Erkennungs-Signatur ---------------------------------------- */
static const char sig[] = "TABTSR-RES-1";
static unsigned my_psp_seg = 0;

/* -------- Dateiname-Cache ----------------------------------------------- */
#define MAX_FILES 64
#define NAME_LEN  13

static char          file_cache[MAX_FILES][NAME_LEN];
static unsigned char file_is_dir[MAX_FILES];
static int           file_count;

/* DTA MUSS global sein (SS!=DS im Hook) */
static unsigned char dta_buf[64];

/* -------- Befehlsname-Cache (DOS-Intern + PATH-Executables) ------------- */
#define CMD_MAX 128
static char cmd_cache[CMD_MAX][NAME_LEN];
static int  cmd_count = 0;

/* -------- Completion-Zustand -------------------------------------------- */
#define COMP_FILE 0
#define COMP_CMD  1
static char comp_base[NAME_LEN];
static int  comp_base_len = 0;
static int  comp_active   = 0;
static int  comp_index    = 0;
static int  shown_len     = 0;
static int  comp_mode     = COMP_FILE;

/* Pfad-Completion: Verzeichnis-Praefix (global: SS!=DS) */
static char scan_pat[80];
static char dir_prefix[68];
static int  dir_prefix_len = 0;

/* -------- History-Ringpuffer -------------------------------------------- */
#define HIST_COUNT  20
#define HIST_MAXLEN 128
static char hist_buf[HIST_COUNT][HIST_MAXLEN];
static int  hist_len_arr[HIST_COUNT];
static int  hist_head  = 0;
static int  hist_total = 0;
static int  hist_idx   = -1;
static char hist_tmp[HIST_MAXLEN];
static int  hist_tmp_len = 0;

/* Suchpuffer Ctrl+R */
static char srch_needle[32];
static int  srch_needle_len = 0;
static int  srch_skip       = 0;
static char srch_save[HIST_MAXLEN];
static int  srch_save_len   = 0;

/* -------- String-Helfer ------------------------------------------------- */
static int strlen_local( const char *s )
{
    int n = 0; while ( s[n] ) n++; return n;
}

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

/* case-insensitive Substring-Suche; 1=gefunden */
static int strstr_local( const char *hay, const char *needle, int nlen )
{
    int hi, ni; char ch, cn;
    if ( nlen == 0 ) return 1;
    for ( hi = 0; hay[hi]; hi++ ) {
        for ( ni = 0; ni < nlen; ni++ ) {
            ch = hay[hi+ni]; cn = needle[ni];
            if ( !ch ) break;
            if ( ch >= 'a' && ch <= 'z' ) ch -= 0x20;
            if ( cn >= 'a' && cn <= 'z' ) cn -= 0x20;
            if ( ch != cn ) break;
        }
        if ( ni == nlen ) return 1;
    }
    return 0;
}

/* -------- Konsolen-Ausgabe via BIOS INT 10h/0Eh ------------------------- */
void con_out( char c );
#pragma aux con_out =   \
    "mov ah,0x0E"       \
    "mov bx,0x0007"     \
    "int 0x10"          \
    parm [al]           \
    modify [ah bx];

static void msg( char *s ) { while ( *s ) con_out( *s++ ); }

/* -------- Taste lesen via INT 16h/00h (direkter Opcode, kein int86) ----- */
unsigned get_key( void );
#pragma aux get_key = "mov ah,0" "int 0x16" value [ax];

/* -------- Direkte INT-21h-Helfer ---------------------------------------- */
void far *dos_get_dta( void );
#pragma aux dos_get_dta =   \
    "mov ah,0x2F"           \
    "int 0x21"              \
    "mov ax,bx"             \
    "mov dx,es"             \
    value  [dx ax]          \
    modify [bx cx si di es];

void dos_set_dta( void far *p );
#pragma aux dos_set_dta =   \
    "push ds"               \
    "mov ds,dx"             \
    "mov dx,ax"             \
    "mov ah,0x1A"           \
    "int 0x21"              \
    "pop ds"                \
    parm   [dx ax]          \
    modify [ax bx cx si di es];

unsigned dos_findfirst( void far *pat, unsigned attr );
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

unsigned dos_findnext( void );
#pragma aux dos_findnext =  \
    "mov ah,0x4F"           \
    "int 0x21"              \
    "sbb ax,ax"             \
    value  [ax]             \
    modify [bx cx dx si di es];

/* -------- Verzeichnis-Scan ---------------------------------------------- */
static void scan_directory_path( void far *pat )
{
    void far *save_dta;
    unsigned ok;
    file_count = 0;
    save_dta = dos_get_dta();
    dos_set_dta( (void far *)dta_buf );
    ok = dos_findfirst( pat, 0x10 );
    while ( ok == 0 && file_count < MAX_FILES ) {
        if ( dta_buf[30] != '.' ) {
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

static void scan_directory( void )
{
    scan_directory_path( (void far *)"*.*" );
}

/* -------- History-Hilfsfunktionen --------------------------------------- */

/* Zeile aus COMMAND.COMs Puffer (bseg:boff+2, len Zeichen) in History speichern */
static void hist_save( unsigned bseg, unsigned boff, int len )
{
    char far *data = (char far *)MK_FP( bseg, boff + 2 );
    int i, prev;
    if ( len == 0 ) return;
    if ( hist_total > 0 ) {
        prev = (hist_head - 1 + HIST_COUNT) % HIST_COUNT;
        if ( hist_len_arr[prev] == len ) {
            int match = 1;
            for ( i = 0; i < len; i++ ) {
                char ca = hist_buf[prev][i], cb = data[i];
                if ( ca >= 'a' && ca <= 'z' ) ca -= 0x20;
                if ( cb >= 'a' && cb <= 'z' ) cb -= 0x20;
                if ( ca != cb ) { match = 0; break; }
            }
            if ( match ) return;
        }
    }
    for ( i = 0; i < len && i < HIST_MAXLEN - 1; i++ )
        hist_buf[hist_head][i] = data[i];
    hist_buf[hist_head][i] = 0;
    hist_len_arr[hist_head] = len;
    hist_head = (hist_head + 1) % HIST_COUNT;
    if ( hist_total < HIST_COUNT ) hist_total++;
}

/* Aktuelle Zeile loeschen und neue aus DGROUP-Puffer schreiben.
   Cursor muss VOR dem Aufruf am Zeilenende stehen (old_len Zeichen nach Prompt). */
static int line_replace( char far *buf, int old_len,
                          const char *new_data, int new_len )
{
    int i, maxlen;
    maxlen = (unsigned char)buf[0];
    for ( i = 0; i < old_len; i++ )
        { con_out(0x08); con_out(' '); con_out(0x08); }
    if ( new_len > maxlen - 1 ) new_len = maxlen - 1;
    for ( i = 0; i < new_len; i++ ) {
        buf[2+i] = new_data[i];
        con_out( new_data[i] );
    }
    return new_len;
}

/* -------- Cursor N Stellen nach links ----------------------------------- */
static void cursor_left_n( int n )
{
    while ( n-- > 0 ) con_out( 0x08 );
}

/* -------- Ctrl+R: Rueckwaertssuche in History --------------------------- */
static int do_search( char far *buf, int old_len )
{
    unsigned k; unsigned char ascii;
    int i, steps, midx, mlen, disp;

    srch_needle_len = 0; srch_skip = 0;

    /* Zeile loeschen, Prompt ausgeben */
    for ( i = 0; i < old_len; i++ )
        { con_out(0x08); con_out(' '); con_out(0x08); }
    msg( "(search): " );
    disp = 0;

    for ( ;; ) {
        /* Vorherige Anzeige loeschen */
        for ( i = 0; i < disp; i++ )
            { con_out(0x08); con_out(' '); con_out(0x08); }
        disp = 0;

        /* Treffer suchen */
        midx = -1; mlen = 0; steps = 0;
        for ( i = 0; i < hist_total; i++ ) {
            int idx = (hist_head - 1 - i + HIST_COUNT * 2) % HIST_COUNT;
            if ( strstr_local( hist_buf[idx], srch_needle, srch_needle_len ) ) {
                if ( steps == srch_skip ) {
                    midx = idx; mlen = hist_len_arr[idx]; break;
                }
                steps++;
            }
        }

        /* Anzeige: Needle + ": " + Treffer */
        for ( i = 0; i < srch_needle_len; i++ )
            { con_out( srch_needle[i] ); disp++; }
        if ( midx >= 0 ) {
            con_out(':'); con_out(' '); disp += 2;
            for ( i = 0; i < mlen; i++ )
                { con_out( hist_buf[midx][i] ); disp++; }
        } else {
            msg( ": (kein Treffer)" ); disp += 16;
        }

        k = get_key();
        ascii = (unsigned char)( k & 0xFF );

        if ( ascii == 0x0D ) {                    /* ENTER: Treffer uebernehmen */
            for ( i = 0; i < disp; i++ )
                { con_out(0x08); con_out(' '); con_out(0x08); }
            for ( i = 0; i < 10; i++ )
                { con_out(0x08); con_out(' '); con_out(0x08); }
            if ( midx >= 0 )
                return line_replace( buf, 0, hist_buf[midx], mlen );
            return 0;
        }
        else if ( ascii == 0x1B ) {               /* ESC: abbrechen             */
            for ( i = 0; i < disp; i++ )
                { con_out(0x08); con_out(' '); con_out(0x08); }
            for ( i = 0; i < 10; i++ )
                { con_out(0x08); con_out(' '); con_out(0x08); }
            return -1;
        }
        else if ( ascii == 0x08 ) {
            if ( srch_needle_len > 0 ) srch_needle_len--;
            srch_skip = 0;
        }
        else if ( ascii == 0x12 ) {               /* Ctrl+R: naechst-aelterer   */
            if ( midx >= 0 ) srch_skip++;
        }
        else if ( ascii >= 0x20 && ascii < 0x7F ) {
            if ( srch_needle_len < (int)(sizeof srch_needle) - 1 ) {
                srch_needle[srch_needle_len++] = ascii;
                srch_needle[srch_needle_len]   = 0;
            }
            srch_skip = 0;
        }
    }
}

/* -------- TAB-Completion ------------------------------------------------ */
static int do_complete( unsigned bseg, unsigned boff, int len )
{
    char far *buf = (char far *)MK_FP( bseg, boff );
    int maxlen = (unsigned char)buf[0];
    int i, target, count, matched, namelen, addlen, avail;

    if ( !comp_active ) {
        int arg_start, slash_pos, j;
        int first_word;

        /* arg_start: letztes Space vor len */
        arg_start = len;
        while ( arg_start > 0 && buf[2+arg_start-1] != ' ' ) arg_start--;
        first_word = ( arg_start == 0 );

        /* slash_pos: letzter \ oder / im Argument */
        slash_pos = arg_start;
        for ( j = arg_start; j < len; j++ )
            if ( buf[2+j] == '\\' || buf[2+j] == '/' ) slash_pos = j + 1;

        /* dir_prefix */
        dir_prefix_len = slash_pos - arg_start;
        if ( dir_prefix_len >= (int)(sizeof dir_prefix) - 1 )
            dir_prefix_len = (int)(sizeof dir_prefix) - 2;
        for ( j = 0; j < dir_prefix_len; j++ )
            dir_prefix[j] = buf[2+arg_start+j];
        dir_prefix[dir_prefix_len] = 0;

        /* comp_base */
        comp_base_len = len - slash_pos;
        if ( comp_base_len > NAME_LEN - 1 ) comp_base_len = NAME_LEN - 1;
        for ( j = 0; j < comp_base_len; j++ )
            comp_base[j] = buf[2+slash_pos+j];

        /* Pfad-Scan wenn noetig */
        if ( !first_word && dir_prefix_len > 0 ) {
            for ( j = 0; j < dir_prefix_len; j++ ) scan_pat[j] = dir_prefix[j];
            scan_pat[dir_prefix_len  ] = '*';
            scan_pat[dir_prefix_len+1] = '.';
            scan_pat[dir_prefix_len+2] = '*';
            scan_pat[dir_prefix_len+3] = 0;
            scan_directory_path( (void far *)scan_pat );
        }

        comp_mode  = first_word ? COMP_CMD : COMP_FILE;
        comp_index = 0;
        shown_len  = dir_prefix_len + comp_base_len;
    }

    /* Treffer suchen */
    target = comp_index; matched = -1; count = 0;
    {
        int csz = (comp_mode == COMP_CMD) ? cmd_count : file_count;
        for ( i = 0; i < csz; i++ ) {
            const char *e = (comp_mode == COMP_CMD) ? cmd_cache[i] : file_cache[i];
            if ( comp_base_len == 0 ||
                 ( strlen_local(e) >= comp_base_len &&
                   memcmp_local(e, comp_base, comp_base_len) == 0 ) ) {
                if ( count == target ) { matched = i; break; }
                count++;
            }
        }
        if ( matched < 0 && target > 0 ) {
            comp_index = 0;
            for ( i = 0; i < csz; i++ ) {
                const char *e = (comp_mode == COMP_CMD) ? cmd_cache[i] : file_cache[i];
                if ( comp_base_len == 0 ||
                     ( strlen_local(e) >= comp_base_len &&
                       memcmp_local(e, comp_base, comp_base_len) == 0 ) ) {
                    matched = i; break;
                }
            }
        }
    }
    if ( matched < 0 ) { con_out(0x07); comp_active = 0; return len; }

    {
        const char *e = (comp_mode == COMP_CMD) ? cmd_cache[matched] : file_cache[matched];
        namelen = strlen_local( e );
    }

    if ( comp_mode == COMP_CMD ) {
        addlen = namelen + 1;                     /* + Leerzeichen           */
    } else {
        addlen = namelen + (file_is_dir[matched] ? 1 : 0);
    }

    avail = (maxlen - 1) - (len - shown_len);
    if ( dir_prefix_len + addlen > avail ) {
        con_out(0x07); comp_active = 0; return len;
    }

    /* Sichtbaren Teil loeschen */
    for ( i = 0; i < shown_len; i++ )
        { con_out(0x08); con_out(' '); con_out(0x08); }
    len -= shown_len;

    /* dir_prefix */
    for ( i = 0; i < dir_prefix_len; i++ )
        { buf[2+len] = dir_prefix[i]; con_out(dir_prefix[i]); len++; }

    /* Treffer */
    {
        const char *e = (comp_mode == COMP_CMD) ? cmd_cache[matched] : file_cache[matched];
        for ( i = 0; i < namelen; i++ )
            { buf[2+len] = e[i]; con_out(e[i]); len++; }
    }

    if ( comp_mode == COMP_CMD ) {
        buf[2+len] = ' '; con_out(' '); len++;
        shown_len  = dir_prefix_len + addlen;
        comp_active = 1;
        comp_index++;
    } else if ( file_is_dir[matched] ) {
        buf[2+len] = '\\'; con_out('\\'); len++;
        shown_len   = dir_prefix_len + addlen;
        comp_active = 0;                          /* naechstes TAB: Unterverzeichnis */
    } else {
        shown_len   = dir_prefix_len + addlen;
        comp_active = 1;
        comp_index++;
    }

    return len;
}

/* -------- Zeilen-Editor ------------------------------------------------- */
static void do_readline( unsigned bseg, unsigned boff )
{
    char far *buf = (char far *)MK_FP( bseg, boff );
    int maxlen = (unsigned char)buf[0];
    int len = 0;
    int cur = 0;                                  /* Cursor-Position 0..len  */
    int ins = 1;                                  /* 1=Einfuegen, 0=Ueberschr */
    unsigned k; unsigned char ascii, scan;
    int i;

    scan_directory();
    comp_active = 0; comp_base_len = 0; shown_len = 0; comp_index = 0;
    hist_idx = -1;

    for ( ;; ) {
        k = get_key();
        ascii = (unsigned char)( k & 0xFF );
        scan  = (unsigned char)( k >> 8 );

        if ( ascii == 0x0D ) {                    /* ENTER                  */
            /* Cursor ans Ende */
            while ( cur < len ) { con_out( buf[2+cur] ); cur++; }
            hist_save( bseg, boff, len );
            hist_idx = -1;
            buf[1] = (char)len;
            buf[2+len] = 0x0D;
            con_out(0x0D); con_out(0x0A);
            return;
        }

        else if ( ascii == 0x09 ) {               /* TAB: nur am Zeilenende */
            if ( cur < len ) { con_out(0x07); }
            else {
                len = do_complete( bseg, boff, len );
                cur = len;
            }
        }

        else if ( ascii == 0x08 ) {               /* Backspace              */
            if ( cur > 0 ) {
                cur--;
                con_out(0x08);
                for ( i = cur; i < len - 1; i++ ) buf[2+i] = buf[2+i+1];
                len--;
                for ( i = cur; i < len; i++ ) con_out( buf[2+i] );
                con_out(' ');
                cursor_left_n( len - cur + 1 );
            }
            comp_active = 0; hist_idx = -1;
        }

        else if ( ascii == 0x12 ) {               /* Ctrl+R                 */
            /* Cursor ans Ende, dann Suche */
            while ( cur < len ) { con_out( buf[2+cur] ); cur++; }
            /* aktuelle Zeile sichern fuer ESC */
            for ( i = 0; i < len && i < HIST_MAXLEN-1; i++ )
                srch_save[i] = buf[2+i];
            srch_save_len = len;
            len = do_search( buf, len );
            if ( len < 0 )
                len = line_replace( buf, 0, srch_save, srch_save_len );
            cur = len;
            comp_active = 0; hist_idx = -1;
        }

        else if ( ascii == 0x00 ) {               /* Extended Key           */
            comp_active = 0;

            if ( scan == 0x48 ) {                 /* Pfeil Hoch: History    */
                if ( hist_total == 0 ) { con_out(0x07); continue; }
                /* Cursor ans Ende */
                while ( cur < len ) { con_out( buf[2+cur] ); cur++; }
                if ( hist_idx == -1 ) {
                    for ( i = 0; i < len && i < HIST_MAXLEN-1; i++ )
                        hist_tmp[i] = buf[2+i];
                    hist_tmp_len = len;
                    hist_idx = 0;
                } else if ( hist_idx < hist_total - 1 ) {
                    hist_idx++;
                } else { con_out(0x07); continue; }
                {
                    int entry = (hist_head - 1 - hist_idx + HIST_COUNT*2) % HIST_COUNT;
                    len = line_replace( buf, len, hist_buf[entry], hist_len_arr[entry] );
                    cur = len;
                }
            }

            else if ( scan == 0x50 ) {            /* Pfeil Runter: History  */
                if ( hist_idx < 0 ) { con_out(0x07); continue; }
                while ( cur < len ) { con_out( buf[2+cur] ); cur++; }
                hist_idx--;
                if ( hist_idx < 0 ) {
                    len = line_replace( buf, len, hist_tmp, hist_tmp_len );
                } else {
                    int entry = (hist_head - 1 - hist_idx + HIST_COUNT*2) % HIST_COUNT;
                    len = line_replace( buf, len, hist_buf[entry], hist_len_arr[entry] );
                }
                cur = len; hist_idx = hist_idx; /* bleibt */
            }

            else if ( scan == 0x4B ) {            /* Pfeil Links            */
                if ( cur > 0 ) { cur--; con_out(0x08); }
            }

            else if ( scan == 0x4D ) {            /* Pfeil Rechts           */
                if ( cur < len ) { con_out( buf[2+cur] ); cur++; }
            }

            else if ( scan == 0x47 ) {            /* Home                   */
                cursor_left_n( cur ); cur = 0;
            }

            else if ( scan == 0x4F ) {            /* End                    */
                while ( cur < len ) { con_out( buf[2+cur] ); cur++; }
            }

            else if ( scan == 0x53 ) {            /* Del                    */
                if ( cur < len ) {
                    for ( i = cur; i < len - 1; i++ ) buf[2+i] = buf[2+i+1];
                    len--;
                    for ( i = cur; i < len; i++ ) con_out( buf[2+i] );
                    con_out(' ');
                    cursor_left_n( len - cur + 1 );
                }
            }

            else if ( scan == 0x52 ) {            /* Ins: Modus wechseln    */
                ins ^= 1;
            }

            else if ( scan == 0x73 ) {            /* Ctrl+Links: Wortsprung */
                int steps = 0;
                while ( cur > 0 && buf[2+cur-1] == ' ' ) { cur--; steps++; }
                while ( cur > 0 && buf[2+cur-1] != ' ' ) { cur--; steps++; }
                cursor_left_n( steps );
            }

            else if ( scan == 0x74 ) {            /* Ctrl+Rechts: Wortsprung */
                while ( cur < len && buf[2+cur] != ' ' )
                    { con_out( buf[2+cur] ); cur++; }
                while ( cur < len && buf[2+cur] == ' ' )
                    { con_out( buf[2+cur] ); cur++; }
            }
            /* Alle anderen Extended-Tasten: ignorieren */
        }

        else if ( ascii >= 0x20 && ascii < 0x7F ) { /* Druckbar             */
            if ( cur == len ) {
                /* Anhaengen */
                if ( len < maxlen - 1 ) {
                    buf[2+len] = ascii; con_out(ascii); len++; cur++;
                }
            } else if ( ins ) {
                /* Einfuegen mid-line */
                if ( len < maxlen - 1 ) {
                    for ( i = len; i > cur; i-- ) buf[2+i] = buf[2+i-1];
                    buf[2+cur] = ascii; len++;
                    con_out(ascii); cur++;
                    for ( i = cur; i < len; i++ ) con_out( buf[2+i] );
                    cursor_left_n( len - cur );
                }
            } else {
                /* Ueberschreiben */
                buf[2+cur] = ascii; con_out(ascii); cur++;
                if ( cur > len ) len = cur;
            }
            comp_active = 0; hist_idx = -1;
        }
        /* Sonstige Steuerzeichen: ignorieren */
    }
}

/* -------- INT 21h Hook -------------------------------------------------- */
void __interrupt __far new21( union INTPACK r )
{
    if ( r.h.ah == 0x0A && *indos_ptr == 0 ) {
        do_readline( r.w.ds, r.w.dx );
        return;
    }
    _chain_intr( old21 );
}

/* -------- Deinstallation (/u) ------------------------------------------- */
static int do_uninstall( void )
{
    void far *cur;
    unsigned my_off, delta, res_seg, sig_off, psp_off, old21_off;
    struct SREGS s;
    union REGS r;
    char far *remote_sig;
    void far *far *p_old21;
    unsigned far *p_psp;
    void far *old_vec;
    unsigned psp_seg;
    int i;

    cur = _dos_getvect( 0x21 );
    my_off = FP_OFF( (void far *)new21 );
    if ( FP_OFF( cur ) != my_off ) {
        msg( "TABTSR ist nicht (mehr) der oberste INT-21h-Hook.\r\n" );
        return 1;
    }

    segread( &s );
    delta   = s.ds - s.cs;
    res_seg = FP_SEG( cur ) + delta;

    sig_off    = FP_OFF( (void far *)sig );
    remote_sig = (char far *)MK_FP( res_seg, sig_off );
    for ( i = 0; sig[i]; i++ ) {
        if ( remote_sig[i] != sig[i] ) {
            msg( "Signatur passt nicht. Deinstallation abgebrochen.\r\n" );
            return 1;
        }
    }

    psp_off = FP_OFF( (void far *)&my_psp_seg );
    p_psp   = (unsigned far *)MK_FP( res_seg, psp_off );
    psp_seg = *p_psp;

    old21_off = FP_OFF( (void far *)&old21 );
    p_old21   = (void far * far *)MK_FP( res_seg, old21_off );
    old_vec   = *p_old21;

    _dos_setvect( 0x21, (void (__interrupt __far *)())old_vec );

    segread( &s );
    s.es = psp_seg;
    r.h.ah = 0x49;
    intdosx( &r, &r, &s );

    msg( "TABTSR entfernt.\r\n" );
    return 0;
}

/* -------- Init: DOS-interne Befehle in cmd_cache laden ------------------ */
static void load_dos_cmds( void )
{
    static const char * const cmds[] = {
        "CD","CHDIR","CLS","COPY","DATE","DEL","DIR","ECHO","EXIT",
        "FOR","GOTO","IF","MD","MKDIR","PATH","PAUSE","PROMPT",
        "RD","REM","REN","RENAME","RMDIR","SET","TIME","TYPE",
        "VER","VERIFY","VOL", 0
    };
    int i, j;
    for ( i = 0; cmds[i] && cmd_count < CMD_MAX; i++ ) {
        for ( j = 0; cmds[i][j] && j < NAME_LEN-1; j++ )
            cmd_cache[cmd_count][j] = cmds[i][j];
        cmd_cache[cmd_count][j] = 0;
        cmd_count++;
    }
}

/* -------- Init: Eines Verzeichnisses Executables in cmd_cache ----------- */
static void scan_one_path_dir( char far *dir, int dlen )
{
    static char pat[82];
    static unsigned char dta2[64];
    static const char exts[3][4] = { "EXE", "COM", "BAT" };
    union REGS r; struct SREGS s;
    int e, i, ni, plen, dup;

    for ( e = 0; e < 3 && cmd_count < CMD_MAX; e++ ) {
        plen = 0;
        for ( i = 0; i < dlen && plen < 76; i++ ) pat[plen++] = dir[i];
        if ( plen > 0 && pat[plen-1] != '\\' && pat[plen-1] != '/' )
            pat[plen++] = '\\';
        pat[plen++] = '*'; pat[plen++] = '.';
        pat[plen++] = exts[e][0]; pat[plen++] = exts[e][1]; pat[plen++] = exts[e][2];
        pat[plen]   = 0;

        /* DTA setzen */
        segread( &s );
        r.h.ah = 0x1A; r.x.dx = (unsigned)dta2;
        intdosx( &r, &r, &s );

        /* FindFirst */
        segread( &s );
        r.h.ah = 0x4E; r.x.cx = 0x20; r.x.dx = (unsigned)pat;
        intdosx( &r, &r, &s );

        while ( !r.x.cflag && cmd_count < CMD_MAX ) {
            char name[9];
            ni = 0;
            while ( dta2[30+ni] && dta2[30+ni] != '.' && ni < 8 )
                { name[ni] = dta2[30+ni]; ni++; }
            name[ni] = 0;

            /* Duplikat-Check */
            dup = 0;
            for ( i = 0; i < cmd_count; i++ ) {
                if ( strlen_local(cmd_cache[i]) == ni &&
                     memcmp_local(cmd_cache[i], name, ni) == 0 )
                    { dup = 1; break; }
            }
            if ( !dup ) {
                for ( i = 0; i <= ni; i++ ) cmd_cache[cmd_count][i] = name[i];
                cmd_count++;
            }

            r.h.ah = 0x4F;
            intdos( &r, &r );
        }
    }
}

/* -------- Init: PATH-Umgebungsvariable scannen -------------------------- */
static void scan_path_env( void )
{
    unsigned env_seg = *(unsigned far *)MK_FP( _psp, 0x2C );
    char far *env    = (char far *)MK_FP( env_seg, 0 );
    static char dir[82];
    int dlen;
    char far *p;

    while ( *env ) {
        if ( env[0]=='P' && env[1]=='A' && env[2]=='T' && env[3]=='H' && env[4]=='=' ) {
            p = env + 5; dlen = 0;
            while ( *p ) {
                if ( *p == ';' ) {
                    if ( dlen > 0 ) scan_one_path_dir( (char far *)dir, dlen );
                    dlen = 0;
                } else {
                    if ( dlen < 80 ) dir[dlen++] = *p;
                }
                p++;
            }
            if ( dlen > 0 ) scan_one_path_dir( (char far *)dir, dlen );
            break;
        }
        while ( *env ) env++;
        env++;
    }
}

/* -------- Installation -------------------------------------------------- */
int main( int argc, char *argv[] )
{
    union REGS r; struct SREGS s;
    unsigned para;

    if ( argc > 1 && (argv[1][0] == '/' || argv[1][0] == '-') &&
         (argv[1][1] == 'u' || argv[1][1] == 'U') && argv[1][2] == 0 ) {
        return do_uninstall();
    }

    msg( "TABTSR v" TABTSR_VERSION
         " - TAB/History/Pfade/Befehle/PATH\r\n" );
    msg( "TAB=Completion  Hoch/Runter=History  Ctrl+R=Suche\r\n" );
    msg( "Links/Rechts/Home/End/Del/Ins=Editor  /u=Deinstall\r\n" );

    /* DOS-Befehle und PATH-Executables laden */
    load_dos_cmds();
    scan_path_env();

    /* InDOS-Flag */
    segread( &s );
    r.h.ah = 0x34;
    intdosx( &r, &r, &s );
    indos_ptr = (unsigned char far *)MK_FP( s.es, r.x.bx );

    my_psp_seg = _psp;

    old21 = _dos_getvect( 0x21 );
    _dos_setvect( 0x21, new21 );

    para = (get_ss() - _psp) + (get_sp() / 16) + 16;
    _dos_keep( 0, para );
    return 0;
}
