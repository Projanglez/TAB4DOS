/*
 * DOSTAB - TAB filename completion for MS-DOS
 * v0.8  -  Open Watcom C (16-bit, Real Mode)
 *
 * Architecture: standalone line editor, hooks INT 21h / AH=0Ah only.
 * Keys read via INT 16h (as normal caller, no hook). InDOS==0 guaranteed on entry.
 * SS != DS in hook: all buffers must be global (DGROUP). No int86/intdos in resident code.
 *
 * Features: TAB completion (files, dirs, paths, DOS commands, PATH executables),
 * command history (Up/Down), mid-line editing (Left/Right, Home, End, Del, Ins,
 * Ctrl+Left/Right), ESC to clear line, /u uninstall.
 *
 * Build: build.bat   Test: DOSBox (load check only), then real 386.
 */

#include <dos.h>
#include <i86.h>

#define DOSTAB_VERSION "0.8.0"

extern unsigned _psp;

unsigned get_ss( void );
#pragma aux get_ss = "mov ax, ss" value [ax];
unsigned get_sp( void );
#pragma aux get_sp = "mov ax, sp" value [ax];
unsigned get_ds( void );
#pragma aux get_ds = "mov ax, ds" value [ax];

/* -------- Hook + InDOS -------------------------------------------------- */
static void (__interrupt __far *old21)();
static unsigned char far *indos_ptr = 0;

/* -------- Signature for /u detection ------------------------------------ */
static const char sig[] = "DOSTAB-RES-1";
static unsigned my_psp_seg = 0;

/* -------- File name cache ----------------------------------------------- */
#define MAX_FILES 64
#define NAME_LEN  13

static char          file_cache[MAX_FILES][NAME_LEN];
static unsigned char file_is_dir[MAX_FILES];
static int           file_count;

/* DTA must be global (SS != DS in hook) */
static unsigned char dta_buf[64];

/* -------- Command name cache (DOS internals + PATH executables) ---------- */
#define CMD_MAX 128
static char cmd_cache[CMD_MAX][NAME_LEN];
static int  cmd_count = 0;

/* -------- Completion state ----------------------------------------------- */
#define COMP_FILE 0
#define COMP_CMD  1
static char comp_base[NAME_LEN];
static int  comp_base_len  = 0;
static int  comp_active    = 0;
static int  comp_index     = 0;
static int  shown_len      = 0;
static int  comp_mode      = COMP_FILE;
static int  comp_first_word = 0;

/* Path completion: directory prefix (global: SS != DS) */
static char scan_pat[80];
static char dir_prefix[68];
static int  dir_prefix_len = 0;

/* -------- History ring buffer ------------------------------------------- */
#define HIST_COUNT  20
#define HIST_MAXLEN 128
static char hist_buf[HIST_COUNT][HIST_MAXLEN];
static int  hist_len_arr[HIST_COUNT];
static int  hist_head    = 0;
static int  hist_total   = 0;
static int  hist_idx     = -1;
static char hist_tmp[HIST_MAXLEN];
static int  hist_tmp_len = 0;

/* -------- String helpers ------------------------------------------------- */
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

static int strcmp_ci( const char *a, const char *b )
{
    unsigned char ca, cb;
    for ( ;; ) {
        ca = (unsigned char)*a++; cb = (unsigned char)*b++;
        if ( ca >= 'a' && ca <= 'z' ) ca -= 0x20;
        if ( cb >= 'a' && cb <= 'z' ) cb -= 0x20;
        if ( ca != cb ) return (int)ca - (int)cb;
        if ( ca == 0 ) return 0;
    }
}

/* -------- Console output via BIOS INT 10h/0Eh --------------------------- */
void con_out( char c );
#pragma aux con_out =   \
    "mov ah,0x0E"       \
    "mov bx,0x0007"     \
    "int 0x10"          \
    parm [al]           \
    modify [ah bx];

static void msg( char *s ) { while ( *s ) con_out( *s++ ); }

/* -------- Key read via INT 16h/00h (direct opcode, no int86) ------------ */
unsigned get_key( void );
#pragma aux get_key = "mov ah,0" "int 0x16" value [ax];

/* -------- Direct INT 21h helpers ---------------------------------------- */
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

/* -------- Init/uninstall DOS helpers (direct INT 21h, no CRT wrappers) --- */
/* NOTE: every modify list MUST cover the full volatile set [bx cx dx si di es]
   that an INT 21h call may clobber (the DOS dispatch and any chained handler
   are free to trash these). A too-narrow list let the compiler keep a live
   value (the resident PSP) in SI across the set-vector call in do_uninstall,
   which then freed a garbage segment -> MCB corruption -> hang. */

/* Get current INT 21h vector (AH=35h) -> far pointer in DX:AX */
void far *dos_getvect21( void );
#pragma aux dos_getvect21 = \
    "mov ax,0x3521"         \
    "int 0x21"              \
    "mov ax,bx"             \
    "mov dx,es"             \
    value  [dx ax]          \
    modify [bx cx si di es];

/* Set INT 21h vector (AH=25h), DS:DX = handler (passed as seg:off in dx:ax) */
void dos_setvect21( void far *handler );
#pragma aux dos_setvect21 = \
    "push ds"               \
    "mov ds,dx"             \
    "mov dx,ax"             \
    "mov ax,0x2521"         \
    "int 0x21"              \
    "pop ds"                \
    parm   [dx ax]          \
    modify [ax bx cx dx si di es];

/* Get InDOS flag pointer (AH=34h) -> far pointer in DX:AX */
void far *dos_get_indos( void );
#pragma aux dos_get_indos = \
    "mov ah,0x34"           \
    "int 0x21"              \
    "mov ax,bx"             \
    "mov dx,es"             \
    value  [dx ax]          \
    modify [bx cx si di es];

/* Free a memory block (AH=49h), ES = segment */
void dos_free_seg( unsigned seg );
#pragma aux dos_free_seg =  \
    "mov es,ax"             \
    "mov ah,0x49"           \
    "int 0x21"              \
    parm   [ax]             \
    modify [ax bx cx dx si di es];

/* Terminate and stay resident (AH=31h, AL=0), DX = paragraphs to keep */
void tsr_keep( unsigned para );
#pragma aux tsr_keep =      \
    "mov ax,0x3100"         \
    "int 0x21"              \
    parm   [dx]             \
    modify [ax bx cx si di es];

/* -------- Directory scan ------------------------------------------------- */
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
            int i; char c;
            for ( i = 0; i < NAME_LEN - 1 && dta_buf[30+i]; i++ ) {
                c = dta_buf[30+i];
                if ( c >= 'A' && c <= 'Z' ) c += 0x20; /* store lowercase */
                file_cache[file_count][i] = c;
            }
            file_cache[file_count][i] = 0;
            file_is_dir[file_count] = (dta_buf[21] & 0x10) ? 1 : 0;
            file_count++;
        }
        ok = dos_findnext();
    }
    /* Sort results alphabetically (insertion sort, max 64 entries) */
    {
        int a, j, b; char tmp_name[NAME_LEN]; unsigned char tmp_dir;
        for ( a = 1; a < file_count; a++ ) {
            for ( b = 0; b < NAME_LEN; b++ ) tmp_name[b] = file_cache[a][b];
            tmp_dir = file_is_dir[a];
            j = a - 1;
            while ( j >= 0 && strcmp_ci( file_cache[j], tmp_name ) > 0 ) {
                for ( b = 0; b < NAME_LEN; b++ ) file_cache[j+1][b] = file_cache[j][b];
                file_is_dir[j+1] = file_is_dir[j];
                j--;
            }
            for ( b = 0; b < NAME_LEN; b++ ) file_cache[j+1][b] = tmp_name[b];
            file_is_dir[j+1] = tmp_dir;
        }
    }
    dos_set_dta( save_dta );
}

static void scan_directory( void )
{
    scan_directory_path( (void far *)"*.*" );
}

/* -------- History helpers ------------------------------------------------ */

/* Save line from COMMAND.COM buffer (bseg:boff+2, len chars) to history */
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
            if ( match ) return; /* skip exact duplicate of last entry */
        }
    }
    for ( i = 0; i < len && i < HIST_MAXLEN - 1; i++ )
        hist_buf[hist_head][i] = data[i];
    hist_buf[hist_head][i] = 0;
    hist_len_arr[hist_head] = len;
    hist_head = (hist_head + 1) % HIST_COUNT;
    if ( hist_total < HIST_COUNT ) hist_total++;
}

/* Erase current line and write new content from DGROUP buffer.
   Cursor must be at end of line (old_len chars past prompt) before calling. */
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

/* -------- Move cursor n positions left ---------------------------------- */
static void cursor_left_n( int n )
{
    while ( n-- > 0 ) con_out( 0x08 );
}

/* -------- TAB completion ------------------------------------------------- */
static int do_complete( unsigned bseg, unsigned boff, int len )
{
    char far *buf = (char far *)MK_FP( bseg, boff );
    int maxlen = (unsigned char)buf[0];
    int i, target, count, matched, namelen, addlen, avail;

    if ( !comp_active ) {
        int arg_start, slash_pos, j;
        int first_word;

        /* arg_start: position after last space before cursor */
        arg_start = len;
        while ( arg_start > 0 && buf[2+arg_start-1] != ' ' ) arg_start--;
        first_word = ( arg_start == 0 );

        /* slash_pos: position after last \ or / in current argument */
        slash_pos = arg_start;
        for ( j = arg_start; j < len; j++ )
            if ( buf[2+j] == '\\' || buf[2+j] == '/' ) slash_pos = j + 1;

        /* extract dir_prefix */
        dir_prefix_len = slash_pos - arg_start;
        if ( dir_prefix_len >= (int)(sizeof dir_prefix) - 1 )
            dir_prefix_len = (int)(sizeof dir_prefix) - 2;
        for ( j = 0; j < dir_prefix_len; j++ )
            dir_prefix[j] = buf[2+arg_start+j];
        dir_prefix[dir_prefix_len] = 0;

        /* extract comp_base (stem to match against) */
        comp_base_len = len - slash_pos;
        if ( comp_base_len > NAME_LEN - 1 ) comp_base_len = NAME_LEN - 1;
        for ( j = 0; j < comp_base_len; j++ )
            comp_base[j] = buf[2+slash_pos+j];

        /* Scan directory on first TAB: explicit prefix or current directory */
        if ( dir_prefix_len > 0 ) {
            for ( j = 0; j < dir_prefix_len; j++ ) scan_pat[j] = dir_prefix[j];
            scan_pat[dir_prefix_len  ] = '*';
            scan_pat[dir_prefix_len+1] = '.';
            scan_pat[dir_prefix_len+2] = '*';
            scan_pat[dir_prefix_len+3] = 0;
            scan_directory_path( (void far *)scan_pat );
        } else {
            scan_directory();
        }

        comp_mode       = COMP_FILE;   /* always try files first */
        comp_first_word = first_word;  /* remember for cmd fallback */
        comp_index = 0;
        shown_len  = dir_prefix_len + comp_base_len;
    }

    /* Search for match at comp_index */
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
            /* wrap around within current cache */
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
    /* No file match: fall back to cmd_cache (first word only, at least 1 char typed) */
    if ( matched < 0 && comp_mode == COMP_FILE &&
         comp_first_word && comp_base_len > 0 ) {
        comp_mode  = COMP_CMD;
        comp_index = 0;
        for ( i = 0; i < cmd_count; i++ ) {
            if ( strlen_local(cmd_cache[i]) >= comp_base_len &&
                 memcmp_local(cmd_cache[i], comp_base, comp_base_len) == 0 ) {
                matched = i; break;
            }
        }
    }
    if ( matched < 0 ) { con_out(0x07); comp_active = 0; return len; }

    {
        const char *e = (comp_mode == COMP_CMD) ? cmd_cache[matched] : file_cache[matched];
        namelen = strlen_local( e );
    }

    if ( comp_mode == COMP_CMD ) {
        addlen = namelen + 1;                     /* +1 for trailing space   */
    } else {
        addlen = namelen + (file_is_dir[matched] ? 1 : 0);
    }

    avail = (maxlen - 1) - (len - shown_len);
    if ( dir_prefix_len + addlen > avail ) {
        con_out(0x07); comp_active = 0; return len;
    }

    /* Erase the previously shown completion */
    for ( i = 0; i < shown_len; i++ )
        { con_out(0x08); con_out(' '); con_out(0x08); }
    len -= shown_len;

    /* Write dir_prefix */
    for ( i = 0; i < dir_prefix_len; i++ )
        { buf[2+len] = dir_prefix[i]; con_out(dir_prefix[i]); len++; }

    /* Write matched name */
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
        comp_active = 1;   /* next TAB cycles within same dir, not descends */
        comp_index++;
    } else {
        shown_len   = dir_prefix_len + addlen;
        comp_active = 1;
        comp_index++;
    }

    return len;
}

/* -------- Line editor ---------------------------------------------------- */
static void do_readline( unsigned bseg, unsigned boff )
{
    char far *buf = (char far *)MK_FP( bseg, boff );
    int maxlen = (unsigned char)buf[0];
    int len = 0;
    int cur = 0;                                  /* cursor position 0..len  */
    int ins = 1;                                  /* 1=insert, 0=overwrite   */
    unsigned k; unsigned char ascii, scan;
    int i;

    comp_active = 0; comp_base_len = 0; shown_len = 0; comp_index = 0;
    hist_idx = -1;

    for ( ;; ) {
        k = get_key();
        ascii = (unsigned char)( k & 0xFF );
        scan  = (unsigned char)( k >> 8 );

        if ( ascii == 0x0D ) {                    /* ENTER                  */
            while ( cur < len ) { con_out( buf[2+cur] ); cur++; }
            hist_save( bseg, boff, len );
            hist_idx = -1;
            buf[1] = (char)len;
            buf[2+len] = 0x0D;
            con_out(0x0D); con_out(0x0A);
            return;
        }

        else if ( ascii == 0x09 ) {               /* TAB: only at end of line */
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

        else if ( ascii == 0x1B ) {               /* ESC: clear line        */
            while ( cur < len ) { con_out( buf[2+cur] ); cur++; }
            for ( i = 0; i < len; i++ ) { con_out(0x08); con_out(' '); con_out(0x08); }
            len = 0; cur = 0;
            comp_active = 0; hist_idx = -1;
        }

        else if ( ascii == 0x00 ) {               /* Extended key           */
            comp_active = 0;

            if ( scan == 0x48 ) {                 /* Up: history            */
                if ( hist_total == 0 ) { con_out(0x07); continue; }
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

            else if ( scan == 0x50 ) {            /* Down: history          */
                if ( hist_idx < 0 ) { con_out(0x07); continue; }
                while ( cur < len ) { con_out( buf[2+cur] ); cur++; }
                hist_idx--;
                if ( hist_idx < 0 ) {
                    len = line_replace( buf, len, hist_tmp, hist_tmp_len );
                } else {
                    int entry = (hist_head - 1 - hist_idx + HIST_COUNT*2) % HIST_COUNT;
                    len = line_replace( buf, len, hist_buf[entry], hist_len_arr[entry] );
                }
                cur = len;
            }

            else if ( scan == 0x4B ) {            /* Left arrow             */
                if ( cur > 0 ) { cur--; con_out(0x08); }
            }

            else if ( scan == 0x4D ) {            /* Right arrow            */
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

            else if ( scan == 0x52 ) {            /* Ins: toggle mode       */
                ins ^= 1;
            }

            else if ( scan == 0x73 ) {            /* Ctrl+Left: word jump   */
                int steps = 0;
                while ( cur > 0 && buf[2+cur-1] == ' ' ) { cur--; steps++; }
                while ( cur > 0 && buf[2+cur-1] != ' ' ) { cur--; steps++; }
                cursor_left_n( steps );
            }

            else if ( scan == 0x74 ) {            /* Ctrl+Right: word jump  */
                while ( cur < len && buf[2+cur] != ' ' )
                    { con_out( buf[2+cur] ); cur++; }
                while ( cur < len && buf[2+cur] == ' ' )
                    { con_out( buf[2+cur] ); cur++; }
            }
            /* All other extended keys: ignore */
        }

        else if ( ascii >= 0x20 && ascii < 0x7F ) { /* Printable            */
            if ( cur == len ) {
                /* Append */
                if ( len < maxlen - 1 ) {
                    buf[2+len] = ascii; con_out(ascii); len++; cur++;
                }
            } else if ( ins ) {
                /* Insert mid-line */
                if ( len < maxlen - 1 ) {
                    for ( i = len; i > cur; i-- ) buf[2+i] = buf[2+i-1];
                    buf[2+cur] = ascii; len++;
                    con_out(ascii); cur++;
                    for ( i = cur; i < len; i++ ) con_out( buf[2+i] );
                    cursor_left_n( len - cur );
                }
            } else {
                /* Overwrite */
                buf[2+cur] = ascii; con_out(ascii); cur++;
                if ( cur > len ) len = cur;
            }
            comp_active = 0; hist_idx = -1;
        }
        /* Other control characters: ignore */
    }
}

/* -------- INT 21h hook -------------------------------------------------- */
void __interrupt __far new21( union INTPACK r )
{
    if ( r.h.ah == 0x0A && *indos_ptr == 0 ) {
        do_readline( r.w.ds, r.w.dx );
        return;
    }
    _chain_intr( old21 );
}

/* ======================================================================== */
/* Transient (install/uninstall) code below runs only once. It is placed in  */
/* a separate segment (INIT_TEXT/INIT_CODE) that the linker orders above the */
/* resident image + stack, so _dos_keep frees it. NOTHING resident may call  */
/* into here. Everything from here to EOF is transient.                      */
/* ======================================================================== */
#pragma code_seg ( "INIT_TEXT", "INIT_CODE" )

/* -------- Uninstall (/u) ------------------------------------------------- */
/* Always prints its result. Silent mode (/s) is meaningful for startup but not
   for a deliberate uninstall, so it is intentionally ignored here. */
static int do_uninstall( void )
{
    void far *cur;
    unsigned my_off, delta, res_seg, sig_off, psp_off, old21_off;
    char far *remote_sig;
    void far *far *p_old21;
    unsigned far *p_psp;
    void far *old_vec;
    unsigned psp_seg;
    int i;

    cur = dos_getvect21();
    my_off = FP_OFF( (void far *)new21 );
    if ( FP_OFF( cur ) != my_off ) {
        msg( "Error: DOSTAB is not the topmost INT 21h hook.\r\n" );
        return 1;
    }

    /* Resident DGROUP = resident _TEXT (= new21's segment) + (our DS - our _TEXT) */
    delta   = get_ds() - FP_SEG( (void far *)new21 );
    res_seg = FP_SEG( cur ) + delta;

    sig_off    = FP_OFF( (void far *)sig );
    remote_sig = (char far *)MK_FP( res_seg, sig_off );
    for ( i = 0; sig[i]; i++ ) {
        if ( remote_sig[i] != sig[i] ) {
            msg( "Error: Signature mismatch. Uninstall aborted.\r\n" );
            return 1;
        }
    }

    psp_off = FP_OFF( (void far *)&my_psp_seg );
    p_psp   = (unsigned far *)MK_FP( res_seg, psp_off );
    psp_seg = *p_psp;

    old21_off = FP_OFF( (void far *)&old21 );
    p_old21   = (void far * far *)MK_FP( res_seg, old21_off );
    old_vec   = *p_old21;

    /* Confirm before touching the vector/memory, so the message is emitted
       while the machine is in a fully normal state. */
    msg( "DOSTAB uninstalled.\r\n" );

    dos_setvect21( old_vec );

    /* Free the resident program block (PSP + code/data). The environment block
       was already freed at install time (see main), so there is nothing else
       to release and no orphaned block to break repeated install/uninstall
       cycles. */
    dos_free_seg( psp_seg );

    return 0;
}

/* -------- Check whether DOSTAB is already resident ---------------------- */
static int already_loaded( void )
{
    void far *cur; unsigned my_off, delta, res_seg, sig_off;
    char far *remote_sig; int i;

    cur    = dos_getvect21();
    my_off = FP_OFF( (void far *)new21 );
    if ( FP_OFF( cur ) != my_off ) return 0;

    delta   = get_ds() - FP_SEG( (void far *)new21 );
    res_seg = FP_SEG( cur ) + delta;

    sig_off    = FP_OFF( (void far *)sig );
    remote_sig = (char far *)MK_FP( res_seg, sig_off );
    for ( i = 0; sig[i]; i++ )
        if ( remote_sig[i] != sig[i] ) return 0;
    return 1;
}

/* -------- Init: load DOS internal commands into cmd_cache --------------- */
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

/* -------- Init: scan one PATH directory for executables ----------------- */
static void scan_one_path_dir( char far *dir, int dlen )
{
    static char pat[82];
    static unsigned char dta2[64];
    static const char exts[3][4] = { "EXE", "COM", "BAT" };
    int e, i, ni, plen, dup;
    unsigned ok;

    for ( e = 0; e < 3 && cmd_count < CMD_MAX; e++ ) {
        plen = 0;
        for ( i = 0; i < dlen && plen < 76; i++ ) pat[plen++] = dir[i];
        if ( plen > 0 && pat[plen-1] != '\\' && pat[plen-1] != '/' )
            pat[plen++] = '\\';
        pat[plen++] = '*'; pat[plen++] = '.';
        pat[plen++] = exts[e][0]; pat[plen++] = exts[e][1]; pat[plen++] = exts[e][2];
        pat[plen]   = 0;

        dos_set_dta( (void far *)dta2 );
        ok = dos_findfirst( (void far *)pat, 0x20 );

        while ( ok == 0 && cmd_count < CMD_MAX ) {
            char name[9];
            ni = 0;
            while ( dta2[30+ni] && dta2[30+ni] != '.' && ni < 8 ) {
                char c = dta2[30+ni];
                if ( c >= 'a' && c <= 'z' ) c -= 0x20; /* store uppercase */
                name[ni] = c; ni++;
            }
            name[ni] = 0;

            /* Skip duplicates */
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

            ok = dos_findnext();
        }
    }
}

/* -------- Init: scan PATH environment variable -------------------------- */
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

/* -------- Install ------------------------------------------------------- */
int main( void )
{
    unsigned para;
    int silent = 0, do_help = 0, do_uninst = 0;

    /* Parse command tail at PSP:0x80 (len byte, chars, 0x0D-terminated).
       Avoids pulling the CRT argc/argv parser into the resident image. */
    {
        unsigned char far *tail = (unsigned char far *)MK_FP( _psp, 0x80 );
        int n = tail[0], i;
        for ( i = 1; i <= n; i++ ) {
            if ( tail[i] == '/' || tail[i] == '-' ) {
                char sw = tail[i+1];
                if ( sw == 's' || sw == 'S' )                    silent    = 1;
                else if ( sw == 'u' || sw == 'U' )               do_uninst = 1;
                else if ( sw == 'h' || sw == 'H' || sw == '?' )  do_help   = 1;
            }
        }
    }

    if ( do_help ) {
        msg( "DOSTAB v" DOSTAB_VERSION " - Tab-Completion for MS-DOS\r\n" );
        msg( "(c) 2026 Projanglez - www.github.com/projanglez/dostab\r\n" );
        msg( "\r\n" );
        msg( "Usage: DOSTAB [/s] [/u] [/h]\r\n" );
        msg( "\r\n" );
        msg( "  /s   Silent mode (suppress all output)\r\n" );
        msg( "  /u   Uninstall TSR from memory\r\n" );
        msg( "  /h   Show this help\r\n" );
        msg( "\r\n" );
        msg( "Keys:\r\n" );
        msg( "  TAB              Complete filename or command\r\n" );
        msg( "  Up/Down          Browse command history\r\n" );
        msg( "  Left/Right       Move cursor\r\n" );
        msg( "  Home/End         Jump to line start/end\r\n" );
        msg( "  Del/Ins          Delete char / toggle insert mode\r\n" );
        msg( "  Ctrl+Left/Right  Jump word left/right\r\n" );
        msg( "  ESC              Clear current line\r\n" );
        return 0;
    }

    if ( do_uninst ) {
        return do_uninstall();
    }

    if ( !silent ) {
        msg( "DOSTAB v" DOSTAB_VERSION " - Tab-Completion for MS-DOS\r\n" );
        msg( "(c) 2026 Projanglez - www.github.com/projanglez/dostab\r\n" );
        msg( "\r\n" );
    }

    if ( already_loaded() ) {
        if ( !silent ) msg( "Error loading TSR: Already loaded\r\n\r\n" );
        return 1;
    }

    /* Load DOS commands and PATH executables into cmd_cache */
    load_dos_cmds();
    scan_path_env();

    /* Free our environment block now: PATH has been read and the resident copy
       never needs the environment again. Done here in normal process context
       (safe), which (a) frees ~the env block worth of resident memory and
       (b) avoids leaving an orphaned env block that would break repeated
       install/uninstall cycles. Zero PSP:0x2C so nothing references it. */
    {
        unsigned far *env_field = (unsigned far *)MK_FP( _psp, 0x2C );
        if ( *env_field ) {
            dos_free_seg( *env_field );
            *env_field = 0;
        }
    }

    /* Get InDOS flag pointer (INT 21h AH=34h) */
    indos_ptr = (unsigned char far *)dos_get_indos();

    my_psp_seg = _psp;

    old21 = (void (__interrupt __far *)())dos_getvect21();
    dos_setvect21( (void far *)new21 );

    if ( !silent ) msg( "TSR successfully loaded\r\n\r\n" );

    /* Keep PSP + code + DGROUP + stack. INIT_TEXT is linked ABOVE the stack
       (see dostab.lnk), so it lies past SS:SP and is freed by this. Computed
       from SS:SP (not a code offset) so it is independent of how the linker
       frames the INIT_TEXT segment. */
    para = (get_ss() - _psp) + (get_sp() / 16) + 16;
    tsr_keep( para );
    return 0;
}
