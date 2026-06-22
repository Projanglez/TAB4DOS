/*
 * TAB4DOS - TAB filename completion for MS-DOS
 * Open Watcom C (16-bit, real mode). Version: see TAB4DOS_VERSION below.
 *
 * Architecture: standalone line editor, hooks INT 21h / AH=0Ah only.
 * Keys read via INT 16h (as normal caller, no hook). InDOS==0 guaranteed on entry.
 * SS != DS in hook: all buffers must be global (DGROUP). No int86/intdos in resident code.
 *
 * Features: TAB completion (files, dirs, paths, DOS commands, PATH executables),
 * command history (Up/Down), mid-line editing (Left/Right, Home, End, Del, Ins,
 * Ctrl+Left/Right), ESC to clear line, /u uninstall.
 *
 * Build: build.bat   Test: real DOS hardware (DOSBox masks our TAB completion).
 */

#include <dos.h>
#include <i86.h>

#define TAB4DOS_VERSION "0.10.0"

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
static const char sig[] = "TAB4DOS-RES-1";
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
/* Built once at install, written incrementally to a NAME_LEN-record file in
   %TEMP% and read on demand during completion (SmartDrive keeps it RAM-fast).
   Keeping it out of resident BSS saves CMD_MAX*NAME_LEN bytes; the file is
   built record-by-record with disk-based dedup, so no resident staging buffer
   is needed. */
#define CMD_MAX 128
static int  cmd_count = 0;                 /* number of records in idx_path   */
static char idx_path[80];                  /* full path to the index file     */
static char cmd_name[NAME_LEN];            /* record read buffer / matched name */
static unsigned idx_h;                     /* install-only: idx file write handle */

/* -------- Completion state ----------------------------------------------- */
#define COMP_FILE 0
#define COMP_CMD  1
static char comp_base[NAME_LEN];
static int  comp_base_len  = 0;
static int  comp_active    = 0;
static int  comp_cur       = -1;   /* index of currently shown match, -1=none */
static int  shown_len      = 0;
static int  comp_mode      = COMP_FILE;
static int  comp_first_word = 0;

/* Path completion: directory prefix (global: SS != DS) */
static char scan_pat[80];
static char dir_prefix[68];
static int  dir_prefix_len = 0;

/* -------- History ring buffer (slot contents live on disk, see hist_path) - */
#define HIST_COUNT  64
#define HIST_MAXLEN 128
/* Each ring slot is a fixed HIST_MAXLEN-byte record in the history file:
   rec[0] = length (1..127), rec[1..len] = chars. Slot N is at offset
   N*HIST_MAXLEN. Only the ring indices stay resident. */
static char hist_path[80];               /* full path to the history file     */
static char hist_rec[HIST_MAXLEN];       /* one-slot read/write scratch        */
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
/* Full volatile modify list (CLAUDE.md #6): a TSR that hooks INT 10h (e.g. a
   mouse driver hiding/showing its cursor) may clobber the whole volatile set,
   not just the AH/BX this teletype call itself uses. A narrow list lets the
   compiler park a live value (e.g. old_vec's offset) in DX/SI/... across a
   con_out/msg call -> corruption only under a full config (hooked INT 10h),
   silently fine on a bare boot. */
void con_out( char c );
#pragma aux con_out =   \
    "mov ah,0x0E"       \
    "mov bx,0x0007"     \
    "int 0x10"          \
    parm [al]           \
    modify [ah bx cx dx si di es];

static void msg( char *s ) { while ( *s ) con_out( *s++ ); }

/* -------- Key read via INT 16h/00h (direct opcode, no int86) ------------ */
/* Full volatile modify list (CLAUDE.md #6): KEYB and other TSRs hook INT 16h
   and may clobber the volatile set; without this the compiler could keep a
   live value across a key read and lose it only under a full config. */
unsigned get_key( void );
#pragma aux get_key = "mov ah,0" "int 0x16" value [ax] modify [bx cx dx si di es];

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

/* -------- Direct INT 21h file I/O (open/read/close + create/write) -------- */
/* InDOS==0 in the 0Ah hook, so these are safe in resident code. Direct int
   opcode only (no intdos/int86). Full volatile modify list per CLAUDE.md #6.
   All paths/buffers are DGROUP globals and DS==DGROUP at every call site (the
   __interrupt prolog sets DS in the hook; normal context in init), so we pass a
   NEAR offset and use the live DS as segment. This deliberately avoids a far
   pointer in a custom register pair: Watcom assigns the seg/off halves to the
   two registers by its own rule, not the order listed, which silently put the
   offset where the body expected the segment (DS <- offset -> reads failed,
   /u hung). A single near register sidesteps that entirely. */

/* Open existing file read-only (AH=3Dh, AL=0). Returns handle, 0xFFFF on CF. */
unsigned dos_open( const char *path );
#pragma aux dos_open =      \
    "mov ax,0x3D00"         \
    "int 0x21"              \
    "jnc o_ok"              \
    "mov ax,0xFFFF"         \
    "o_ok:"                 \
    parm   [dx]             \
    value  [ax]             \
    modify [bx cx dx si di es];

/* Read up to n bytes (AH=3Fh) into buf. Returns bytes read, 0xFFFF on CF. */
unsigned dos_read( unsigned handle, char *buf, unsigned n );
#pragma aux dos_read =      \
    "mov ah,0x3F"           \
    "int 0x21"              \
    "jnc r_ok"              \
    "mov ax,0xFFFF"         \
    "r_ok:"                 \
    parm   [bx] [dx] [cx]   \
    value  [ax]             \
    modify [bx cx dx si di es];

/* Close a handle (AH=3Eh). */
void dos_close( unsigned handle );
#pragma aux dos_close =     \
    "mov ah,0x3E"           \
    "int 0x21"              \
    parm   [bx]             \
    modify [ax bx cx dx si di es];

/* Create/truncate file (AH=3Ch, normal attr). Returns handle, 0xFFFF on CF. */
unsigned dos_create( const char *path );
#pragma aux dos_create =    \
    "xor cx,cx"             \
    "mov ah,0x3C"           \
    "int 0x21"              \
    "jnc c_ok"              \
    "mov ax,0xFFFF"         \
    "c_ok:"                 \
    parm   [dx]             \
    value  [ax]             \
    modify [bx cx dx si di es];

/* Write n bytes from buf (AH=40h). Returns bytes written, 0xFFFF on CF. */
unsigned dos_write( unsigned handle, char *buf, unsigned n );
#pragma aux dos_write =     \
    "mov ah,0x40"           \
    "int 0x21"              \
    "jnc w_ok"              \
    "mov ax,0xFFFF"         \
    "w_ok:"                 \
    parm   [bx] [dx] [cx]   \
    value  [ax]             \
    modify [bx cx dx si di es];

/* Seek from start of file (AH=42h, AL=0). off < 64KB (CX=0). Used for the
   fixed-record index dedup and the history slot ring. */
void dos_lseek( unsigned handle, unsigned off );
#pragma aux dos_lseek =     \
    "mov ax,0x4200"         \
    "xor cx,cx"             \
    "int 0x21"              \
    parm   [bx] [dx]        \
    modify [ax bx cx dx si di es];

/* Open existing file read/write (AH=3Dh, AL=2). Returns handle, 0xFFFF on CF.
   Used by hist_save (read-dedup then write on one handle). */
unsigned dos_open_rw( const char *path );
#pragma aux dos_open_rw =   \
    "mov ax,0x3D02"         \
    "int 0x21"              \
    "jnc orw_ok"            \
    "mov ax,0xFFFF"         \
    "orw_ok:"               \
    parm   [dx]             \
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

/* Terminate process directly (AH=4Ch, AL=0). Never returns. Used by /u to end
   the uninstaller WITHOUT the Watcom C-runtime exit path (FiniRtns/atexit/
   null-check), which hangs under a full config after a successful unhook+free
   (set-vector and free both verified to complete; only the C exit hung). */
void dos_exit( void );
#pragma aux dos_exit =      \
    "mov ax,0x4C00"         \
    "int 0x21"              \
    aborts;

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

/* Save line from COMMAND.COM buffer (bseg:boff+2, len chars) to the history
   file. Writes one fixed HIST_MAXLEN record at slot hist_head. Runs in the 0Ah
   hook (InDOS==0) so direct DOS file I/O is safe; any I/O failure just skips
   this entry (never hangs). */
static void hist_save( unsigned bseg, unsigned boff, int len )
{
    char far *data = (char far *)MK_FP( bseg, boff + 2 );
    unsigned h;
    int i;
    if ( len == 0 || hist_path[0] == 0 ) return;
    if ( len > HIST_MAXLEN - 1 ) len = HIST_MAXLEN - 1;
    h = dos_open_rw( hist_path );
    if ( h == 0xFFFF ) return;
    /* Skip an exact (case-insensitive) duplicate of the most recent entry. */
    if ( hist_total > 0 ) {
        int prev = (hist_head - 1 + HIST_COUNT) % HIST_COUNT;
        dos_lseek( h, (unsigned)prev * HIST_MAXLEN );
        if ( dos_read( h, hist_rec, HIST_MAXLEN ) == HIST_MAXLEN
             && (int)(unsigned char)hist_rec[0] == len ) {
            int match = 1;
            for ( i = 0; i < len; i++ ) {
                char ca = hist_rec[1+i], cb = data[i];
                if ( ca >= 'a' && ca <= 'z' ) ca -= 0x20;
                if ( cb >= 'a' && cb <= 'z' ) cb -= 0x20;
                if ( ca != cb ) { match = 0; break; }
            }
            if ( match ) { dos_close( h ); return; }
        }
    }
    /* Build the record [len][data...0pad] and write it at slot hist_head. */
    hist_rec[0] = (char)len;
    for ( i = 0; i < len; i++ )            hist_rec[1+i] = data[i];
    for ( i = len; i < HIST_MAXLEN - 1; i++ ) hist_rec[1+i] = 0;
    dos_lseek( h, (unsigned)hist_head * HIST_MAXLEN );
    dos_write( h, hist_rec, HIST_MAXLEN );
    dos_close( h );
    hist_head = (hist_head + 1) % HIST_COUNT;
    if ( hist_total < HIST_COUNT ) hist_total++;
}

/* Read history slot `entry` from the file into hist_rec (data at hist_rec+1,
   length in hist_rec[0]). Returns 1 on success, 0 on any failure. Resident;
   runs in the 0Ah hook. */
static int hist_load( int entry )
{
    unsigned h;
    if ( hist_path[0] == 0 ) return 0;
    h = dos_open( hist_path );
    if ( h == 0xFFFF ) return 0;
    dos_lseek( h, (unsigned)entry * HIST_MAXLEN );
    if ( dos_read( h, hist_rec, HIST_MAXLEN ) != HIST_MAXLEN ) {
        dos_close( h );
        return 0;
    }
    dos_close( h );
    return 1;
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

/* -------- Command lookup from the index file ----------------------------- */
/* Find the target-th (0-based) command in idx_path whose name matches
   comp_base[0..comp_base_len). On success copies the name into cmd_name and
   returns 1; on no-match or any I/O error returns 0 (caller beeps, never hangs).
   Re-reads the file from the start each call; SmartDrive keeps it RAM-fast. */
static int cmd_find( int target )
{
    unsigned h, got;
    int i, count;

    h = dos_open( idx_path );
    if ( h == 0xFFFF ) return 0;

    /* Read each record straight into cmd_name; on the target-th match it is
       already in place, so just close and return. cmd_name is only consulted
       by the caller after a successful (return 1) find. */
    count = 0;
    for ( i = 0; i < cmd_count; i++ ) {
        got = dos_read( h, cmd_name, NAME_LEN );
        if ( got != NAME_LEN ) break;          /* short read / EOF / error */
        if ( comp_base_len == 0 ||
             ( strlen_local(cmd_name) >= comp_base_len &&
               memcmp_local(cmd_name, comp_base, comp_base_len) == 0 ) ) {
            if ( count == target ) { dos_close( h ); return 1; }
            count++;
        }
    }
    dos_close( h );
    return 0;
}

/* True if cache entry e matches the typed stem comp_base[0..comp_base_len). */
static int comp_matches( const char *e )
{
    return comp_base_len == 0 ||
           ( strlen_local(e) >= comp_base_len &&
             memcmp_local(e, comp_base, comp_base_len) == 0 );
}

/* Count matching command records in idx_path (0 on any I/O error). */
static int cmd_count_matches( void )
{
    unsigned h, got;
    int i, n = 0;
    h = dos_open( idx_path );
    if ( h == 0xFFFF ) return 0;
    for ( i = 0; i < cmd_count; i++ ) {
        got = dos_read( h, cmd_name, NAME_LEN );
        if ( got != NAME_LEN ) break;
        if ( comp_matches( cmd_name ) ) n++;
    }
    dos_close( h );
    return n;
}

/* Count matching file_cache entries. */
static int file_count_matches( void )
{
    int i, n = 0;
    for ( i = 0; i < file_count; i++ )
        if ( comp_matches( file_cache[i] ) ) n++;
    return n;
}

/* Return file_cache index of the target-th match, or -1. */
static int file_nth_match( int target )
{
    int i, c = 0;
    for ( i = 0; i < file_count; i++ )
        if ( comp_matches( file_cache[i] ) ) {
            if ( c == target ) return i;
            c++;
        }
    return -1;
}

/* -------- TAB completion ------------------------------------------------- */
/* dir = +1 (TAB, forward) or -1 (Shift+TAB, backward). */
static int do_complete( unsigned bseg, unsigned boff, int len, int dir )
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
        comp_cur   = -1;               /* nothing shown yet */
        shown_len  = dir_prefix_len + comp_base_len;
    }

    /* Count matches in the current mode. If file mode has none and we may fall
       back to commands (first word, >=1 char typed), switch to command mode. */
    count = (comp_mode == COMP_CMD) ? cmd_count_matches() : file_count_matches();
    if ( count == 0 && comp_mode == COMP_FILE &&
         comp_first_word && comp_base_len > 0 ) {
        comp_mode = COMP_CMD;
        comp_cur  = -1;
        count     = cmd_count_matches();
    }
    if ( count == 0 ) { con_out(0x07); comp_active = 0; return len; }

    /* Pick the next match index in the requested direction, wrapping. From a
       fresh start (comp_cur < 0) forward shows the first, backward the last. */
    if ( comp_cur < 0 )
        target = ( dir > 0 ) ? 0 : count - 1;
    else
        target = ( comp_cur + dir + count ) % count;

    /* Resolve the chosen match. Commands land in cmd_name; files give an index
       into file_cache. matched is unused (0) in command mode. */
    matched = 0;
    if ( comp_mode == COMP_CMD ) {
        if ( !cmd_find( target ) ) { con_out(0x07); comp_active = 0; return len; }
        namelen = strlen_local( cmd_name );
    } else {
        matched = file_nth_match( target );
        if ( matched < 0 ) { con_out(0x07); comp_active = 0; return len; }
        namelen = strlen_local( file_cache[matched] );
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
        const char *e = (comp_mode == COMP_CMD) ? cmd_name : file_cache[matched];
        for ( i = 0; i < namelen; i++ )
            { buf[2+len] = e[i]; con_out(e[i]); len++; }
    }

    if ( comp_mode == COMP_CMD ) {
        buf[2+len] = ' '; con_out(' '); len++;
    } else if ( file_is_dir[matched] ) {
        buf[2+len] = '\\'; con_out('\\'); len++;  /* cycles within dir, not descends */
    }
    shown_len   = dir_prefix_len + addlen;
    comp_active = 1;
    comp_cur    = target;

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

    comp_active = 0; comp_base_len = 0; shown_len = 0; comp_cur = -1;
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

        else if ( ascii == 0x09 ) {               /* TAB: forward, end of line */
            if ( cur < len ) { con_out(0x07); }
            else {
                len = do_complete( bseg, boff, len, +1 );
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

            if ( scan == 0x0F ) {                 /* Shift+TAB: backward    */
                if ( cur < len ) { con_out(0x07); }
                else {
                    len = do_complete( bseg, boff, len, -1 );
                    cur = len;
                }
                continue;                         /* keep comp_active for cycling */
            }

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
                    if ( hist_load( entry ) )
                        len = line_replace( buf, len, hist_rec+1,
                                            (unsigned char)hist_rec[0] );
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
                    if ( hist_load( entry ) )
                        len = line_replace( buf, len, hist_rec+1,
                                            (unsigned char)hist_rec[0] );
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
/* Proven v0.8 teardown: read the resident DGROUP via the (still-installed) hook
   vector, verify our signature, then restore the saved INT 21h vector (AH=25h)
   and free the resident program block (AH=49h). The %TEMP% index file is left
   behind intentionally (harmless; a fresh install truncates it). Silent mode
   (/s) is meaningful for startup but not for a deliberate uninstall, so it is
   intentionally ignored here. */
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
        msg( "Error: TAB4DOS is not the topmost INT 21h hook.\r\n" );
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
    msg( "TAB4DOS uninstalled.\r\n" );

    dos_setvect21( old_vec );

    /* Free the resident program block (PSP + code/data). The environment block
       was already freed at install time (see main), so there is nothing else
       to release and no orphaned block to break repeated install/uninstall
       cycles. */
    dos_free_seg( psp_seg );

    /* Terminate directly (AH=4Ch). Skips the C-runtime exit, which hung here
       under a full config even though the unhook+free above both completed. */
    dos_exit();
    return 0;   /* unreachable; dos_exit never returns */
}

/* -------- Check whether TAB4DOS is already resident ---------------------- */
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

/* -------- Init: command index is built directly on disk ------------------ */
/* The list is written incrementally into idx_h (a NAME_LEN-record file) as it
   is discovered, so no resident staging buffer is needed. Dedup re-reads the
   records already written (see scan_one_path_dir). */

/* Build a zero-padded NAME_LEN record from src[0..n) into rec. */
static void cmd_rec_pack( char *rec, const char *src, int n )
{
    int j;
    if ( n > NAME_LEN - 1 ) n = NAME_LEN - 1;
    for ( j = 0; j < n; j++ ) rec[j] = src[j];
    for ( ; j < NAME_LEN; j++ ) rec[j] = 0;
}

/* -------- Init: write DOS internal commands as index records ------------ */
static void load_dos_cmds( void )
{
    static const char * const cmds[] = {
        "CD","CHDIR","CLS","COPY","DATE","DEL","DIR","ECHO","EXIT",
        "FOR","GOTO","IF","MD","MKDIR","PATH","PAUSE","PROMPT",
        "RD","REM","REN","RENAME","RMDIR","SET","TIME","TYPE",
        "VER","VERIFY","VOL", 0
    };
    char rec[NAME_LEN];
    int i;
    for ( i = 0; cmds[i] && cmd_count < CMD_MAX; i++ ) {
        cmd_rec_pack( rec, cmds[i], strlen_local( cmds[i] ) );
        dos_write( idx_h, rec, NAME_LEN );
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
            char name[9], rec[NAME_LEN];
            ni = 0;
            while ( dta2[30+ni] && dta2[30+ni] != '.' && ni < 8 ) {
                char c = dta2[30+ni];
                if ( c >= 'a' && c <= 'z' ) c -= 0x20; /* store uppercase */
                name[ni] = c; ni++;
            }
            name[ni] = 0;

            /* Dedup against records already written: re-read them via LSEEK. */
            dup = 0;
            dos_lseek( idx_h, 0 );
            for ( i = 0; i < cmd_count; i++ ) {
                if ( dos_read( idx_h, rec, NAME_LEN ) != NAME_LEN ) break;
                if ( strlen_local(rec) == ni &&
                     memcmp_local(rec, name, ni) == 0 )
                    { dup = 1; break; }
            }
            if ( !dup ) {
                cmd_rec_pack( rec, name, ni );
                dos_lseek( idx_h, (unsigned)cmd_count * NAME_LEN ); /* append */
                dos_write( idx_h, rec, NAME_LEN );
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

/* -------- Init: does env entry e start with "<name>="? (case-insensitive) - */
static int env_is( char far *e, const char *name )
{
    int i;
    for ( i = 0; name[i]; i++ ) {
        char c = e[i];
        if ( c >= 'a' && c <= 'z' ) c -= 0x20;
        if ( c != name[i] ) return 0;
    }
    return e[i] == '=';
}

/* -------- Init: build idx_path + hist_path from %TEMP% (or %TMP%) ------- */
/* Must run before the environment block is freed. idx_path = "<tempdir>\
   TAB4DOS.IDX"; falls back to "C:\TAB4DOS.IDX". hist_path is the same path with
   the extension changed to .HST. */
static void build_paths( void )
{
    unsigned env_seg = *(unsigned far *)MK_FP( _psp, 0x2C );
    char far *env    = (char far *)MK_FP( env_seg, 0 );
    int tlen = 0, prio = 0;           /* 2 = TEMP found, 1 = TMP found */
    int i;

    /* Accumulate the chosen temp directory directly into idx_path; a later
       higher-priority match (TEMP over TMP) simply overwrites from the start. */
    while ( *env ) {
        int take = 0;
        if ( env_is( env, "TEMP" ) )          take = 2;
        else if ( env_is( env, "TMP" ) )      take = 1;
        if ( take > prio ) {
            char far *v = env;
            while ( *v && *v != '=' ) v++;
            if ( *v == '=' ) v++;
            tlen = 0;
            while ( *v && tlen < 64 ) idx_path[tlen++] = *v++;
            prio = take;
        }
        while ( *env ) env++;
        env++;
    }

    if ( prio == 0 ) {
        static const char fb[] = "C:\\TAB4DOS.IDX";
        for ( i = 0; fb[i]; i++ ) idx_path[i] = fb[i];
        idx_path[i] = 0;
    } else {
        i = tlen;
        if ( i > 0 && idx_path[i-1] != '\\' && idx_path[i-1] != '/' )
            idx_path[i++] = '\\';
        {
            static const char fn[] = "TAB4DOS.IDX";
            int k;
            for ( k = 0; fn[k]; k++ ) idx_path[i++] = fn[k];
            idx_path[i] = 0;
        }
    }

    /* hist_path = idx_path with the .IDX extension replaced by .HST */
    for ( i = 0; idx_path[i]; i++ ) hist_path[i] = idx_path[i];
    hist_path[i] = 0;
    if ( i >= 3 ) { hist_path[i-3] = 'H'; hist_path[i-2] = 'S'; hist_path[i-1] = 'T'; }
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
        msg( "TAB4DOS v" TAB4DOS_VERSION " - Tab-Completion for MS-DOS\r\n" );
        msg( "(c) 2026 Projanglez - www.github.com/projanglez/tab4dos\r\n" );
        msg( "\r\n" );
        msg( "Usage: TAB4DOS [/s] [/u] [/h]\r\n" );
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
        msg( "TAB4DOS v" TAB4DOS_VERSION " - Tab-Completion for MS-DOS\r\n" );
        msg( "(c) 2026 Projanglez - www.github.com/projanglez/tab4dos\r\n" );
        msg( "\r\n" );
    }

    if ( already_loaded() ) {
        if ( !silent ) msg( "Error loading TSR: Already loaded\r\n\r\n" );
        return 1;
    }

    /* Resolve the %TEMP%/%TMP% paths (must run before the env block is freed),
       then build the command index file incrementally on disk (no resident
       staging buffer) and create an empty history file. */
    build_paths();

    idx_h = dos_create( idx_path );
    if ( idx_h == 0xFFFF ) {
        if ( !silent )
            msg( "Warning: could not write command index; "
                 "command completion disabled.\r\n" );
        cmd_count = 0;
    } else {
        load_dos_cmds();    /* write DOS internals as records */
        scan_path_env();    /* append PATH executables (deduped) */
        dos_close( idx_h );
    }

    /* Create an empty history file; disable history if it cannot be created. */
    {
        unsigned hh = dos_create( hist_path );
        if ( hh == 0xFFFF ) hist_path[0] = 0;
        else dos_close( hh );
    }

    /* Free our environment block now: PATH/TEMP have been read and the resident
       copy never needs the environment again. Done here in normal process
       context (safe), which (a) frees ~the env block worth of resident memory
       and (b) avoids leaving an orphaned env block that would break repeated
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
       (see tab4dos.lnk), so it lies past SS:SP and is freed by this. Computed
       from SS:SP (not a code offset) so it is independent of how the linker
       frames the INIT_TEXT segment. */
    para = (get_ss() - _psp) + (get_sp() / 16) + 16;
    tsr_keep( para );
    return 0;
}
