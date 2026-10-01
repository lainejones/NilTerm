/*
 * NilTerm - an ANSI BBS terminal for the Amiga (and the NilBBS sysop's terminal).
 *
 * Calls over telnet, raw TCP, rlogin, or a modem / direct line on serial.device (conn_*),
 * from a SyncTERM-style dialing directory: per system its login (auto-login at the name /
 * password prompts, or Alt+L), a PC or Amiga screen (Topaz from ROM, Latin-1, 0x9B = CSI),
 * an emulated line speed (Alt+Up/Down) and iCE colours; blinking text blinks.
 *
 *   NilTerm [PORT=]n [HOST=name] [MODEID=hex] [NATIVE] [SMALL]
 *
 * Opens its own 640x400 16-colour screen (the VGA palette) with 80x25 cells of
 * 8x16, draws every byte 0x20-0xFF as its CP437 glyph (the IBM VGA 8x16 font in
 * nilfont.h), and telnets to NilBBS on this Amiga (127.0.0.1; with no PORT= it
 * uses the port the running NilBBS listens on, else port= in NilBBS.cfg, else 2323).  It answers the node's terminal detection like SyncTERM does:
 * TTYPE "ANSI", NAWS 80x25, a cursor position report for ESC[6n - so the node
 * picks ANSI + CP437 + 80x25.
 *
 * Screen mode: MODEID= if given, else BestModeID() for 640x400x4 (the RTG mode
 * on a graphics card), else hires-interlaced (default / PAL / NTSC monitor).
 * NATIVE skips BestModeID (tests the chipset path on an RTG machine).  With no
 * 400-line mode, or SMALL, it runs 640x200 with half-height glyphs (each pair
 * of font rows OR'ed into one).
 *
 * Drawing: the font is a TextFont made in memory over a 1-plane strip in chip
 * RAM, so a run of same-coloured characters is ONE Text() call (the blitter on
 * a chipset screen, the card's driver on RTG).  A whole recv() burst is parsed
 * into a cell buffer first; only the dirty spans are drawn, and scrolling is
 * one ScrollRaster per burst, however many lines went by.
 *
 * Keys: arrows ESC[A-D, Shift+Up/Down = PgUp/PgDn (ESC[5~ ESC[6~), Shift+Left/
 * Right = Home/End (ESC[H ESC[K), Del ESC[3~, Backspace 08, Return CR; the
 * Home/End/PgUp/PgDn/Insert keys of a PC-style keyboard too.  Help = about.
 * Amiga-Q quits; so does the BBS hanging up, and CTRL-C (Break).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/text.h>
#include <graphics/rastport.h>
#include <graphics/modeid.h>
#include <graphics/displayinfo.h>
#include <devices/inputevent.h>
#include <devices/serial.h>
#include <devices/timer.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/icon.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/keymap.h>
#include <proto/asl.h>
#include <libraries/asl.h>
#include <proto/iffparse.h>
#include <libraries/iffparse.h>
#include <proto/timer.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <proto/bsdsocket.h>

#include "../common/bbs.h"
#include "ntzio.h"
#include "../node/zmodem.h"
#include "nilfont.h"

static const char __attribute__((used)) verstag[] =
    "$VER: NilTerm " BBS_VERSION " (" BBS_VERDATE ") font: IBM VGA 8x16 by VileR (int10h.org), CC BY-SA 4.0";

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *KeymapBase;
struct Library *SocketBase;
struct Library *AslBase, *IFFParseBase;     /* optional: the file requester, the clipboard */

#define COLS 80
#define SROWS 25                    /* screen rows: the terminal + the status bar */
static int ROWS = SROWS - 1;        /* terminal rows: 24 under the status bar, 25 without one (WATCH=) */

/* ---- the screen ------------------------------------------------------------------- */
static struct Screen *scr;
static struct Window *win;
static struct RastPort *rp;
static UBYTE *strip;                /* the font: 256 glyphs side by side, 1 plane, chip RAM */
static ULONG charloc[257];
static struct TextFont vfont;
static int CH = 16;                 /* cell height: 16, or 8 on a 200-line screen */
static ULONG modeid = INVALID_ID;
static char modename[DISPLAYNAMELEN + 1];

/* the VGA palette, in ANSI colour order (30-37, then the bright ones) */
static const UBYTE vga[16][3] = {
    {0,0,0}, {170,0,0}, {0,170,0}, {170,85,0}, {0,0,170}, {170,0,170}, {0,170,170}, {170,170,170},
    {85,85,85}, {255,85,85}, {85,255,85}, {255,255,85}, {85,85,255}, {255,85,255}, {85,255,255}, {255,255,255}
};

/* ---- the terminal state ------------------------------------------------------------ */
static UBYTE chr[SROWS][COLS], att[SROWS][COLS];     /* att = fg pen | bg pen << 4 */
static UBYTE blk[SROWS][COLS];                      /* 1 = the cell blinks (SGR 5 without iCE colours) */
static BOOL blink_off;                              /* the blink phase: blinking text hidden */
static BOOL ice, ice_force;                         /* iCE colours: blink = bright background (ESC[?33h) */
static BOOL amiga_mode;                             /* Topaz + Latin-1, 0x9B is CSI (an Amiga BBS) */
static BYTE dmin[SROWS], dmax[SROWS];                 /* dirty span per row (dmin > dmax = clean) */
static int cx, cy;                                  /* cursor, 0-based */
static int stop = 0, sbot = SROWS - 2;              /* scroll region */
static int fg = 7, bg = 0, bold, rev;
static UBYTE curattr = 0x07;
static BOOL autowrap = TRUE, curvis = TRUE;
static int sv_x, sv_y, sv_fg = 7, sv_bg, sv_bold, sv_rev;   /* ESC 7 / ESC[s */
static int pend_n, pend_top, pend_bot;              /* scrolling not yet done on screen */
static BOOL cur_drawn;
static int cur_dx, cur_dy;
static BOOL beeped;

static enum { S_NORM, S_ESC, S_CSI, S_SKIP1 } pstate;
#define MAXPAR 16
static int par[MAXPAR], npar;
static BOOL par_any;
static char priv;                                   /* '?', '=', '<', '>' or 0 */
static BOOL inter;                                  /* an intermediate byte: not a sequence we know */

/* ---- the connection ------------------------------------------------------------------ *
 * Telnet, raw TCP and rlogin run over a socket; a modem (or a direct serial line) over
 * serial.device.  conn_send() / conn_wait() hide which. */
enum { CT_TELNET, CT_RAW, CT_RLOGIN, CT_MODEM };
static int ctype = CT_TELNET;
static LONG sock = -1;
static BOOL closed;
static UBYTE ob[1024];
static int obn;

struct Device *TimerBase;
static struct MsgPort *tm_port;
static struct timerequest *tm_io;       /* waits when there's no socket to WaitSelect() on */

static struct MsgPort *ser_port, *ser_rport;
static struct IOExtSer *ser_io, *ser_rd;   /* writes / queries (synchronous), the 1-byte read kept waiting */
static UBYTE ser_rbyte;
static BOOL ser_open_ok, ser_rd_busy;

static BOOL ser_stuck;                  /* a write never went: no CTS from the modem / cable */
static ULONG sig_wait(ULONG ms, ULONG mask);
static void ser_write(const UBYTE *p, LONG n)
{
    int t = 0;
    if (!ser_open_ok || n <= 0 || ser_stuck) return;
    ser_io->IOSer.io_Command = CMD_WRITE;
    ser_io->IOSer.io_Data = (APTR)p;
    ser_io->IOSer.io_Length = n;
    SendIO((struct IORequest *)ser_io);
    while (!CheckIO((struct IORequest *)ser_io)) {
        if (++t > 200) {                /* 10 s: handshake says "wait" and never stops */
            AbortIO((struct IORequest *)ser_io);
            WaitIO((struct IORequest *)ser_io);
            ser_stuck = closed = TRUE;
            return;
        }
        sig_wait(50, 1UL << ser_port->mp_SigBit);
    }
    if (WaitIO((struct IORequest *)ser_io)) closed = TRUE;
}

static void ser_arm(void)
{
    if (!ser_open_ok || ser_rd_busy) return;
    ser_rd->IOSer.io_Command = CMD_READ;
    ser_rd->IOSer.io_Data = &ser_rbyte;
    ser_rd->IOSer.io_Length = 1;
    SendIO((struct IORequest *)ser_rd);
    ser_rd_busy = TRUE;
}

/* what has arrived, up to max bytes; never blocks (0 = nothing yet) */
static LONG ser_read(UBYTE *buf, LONG max)
{
    LONG n = 0, more;
    if (!ser_open_ok || max <= 0) return 0;
    if (!ser_rd_busy) ser_arm();
    if (!CheckIO((struct IORequest *)ser_rd)) return 0;
    WaitIO((struct IORequest *)ser_rd);
    ser_rd_busy = FALSE;
    if (ser_rd->IOSer.io_Error) { ser_arm(); return 0; }
    buf[n++] = ser_rbyte;
    ser_io->IOSer.io_Command = SDCMD_QUERY;
    DoIO((struct IORequest *)ser_io);
    more = (LONG)ser_io->IOSer.io_Actual;
    if (more > max - 1) more = max - 1;
    if (more > 0) {
        ser_io->IOSer.io_Command = CMD_READ;
        ser_io->IOSer.io_Data = buf + 1;
        ser_io->IOSer.io_Length = more;
        DoIO((struct IORequest *)ser_io);   /* an overrun error still hands over what it read */
        n += ser_io->IOSer.io_Actual;
    }
    ser_arm();
    return n;
}

static void ser_close(void)
{
    if (ser_open_ok) {
        if (ser_rd_busy) {
            AbortIO((struct IORequest *)ser_rd);
            WaitIO((struct IORequest *)ser_rd);
            ser_rd_busy = FALSE;
        }
        CloseDevice((struct IORequest *)ser_io);      /* DTR drops: most modems hang up */
        ser_open_ok = FALSE;
    }
    if (ser_rd) { DeleteIORequest((struct IORequest *)ser_rd); ser_rd = NULL; }
    if (ser_io) { DeleteIORequest((struct IORequest *)ser_io); ser_io = NULL; }
    if (ser_rport) { DeleteMsgPort(ser_rport); ser_rport = NULL; }
    if (ser_port) { DeleteMsgPort(ser_port); ser_port = NULL; }
}

static BOOL ser_open(const char *dev, LONG unit, LONG baud, BOOL rtscts)
{
    if (!(ser_port = CreateMsgPort()) || !(ser_rport = CreateMsgPort())) { ser_close(); return FALSE; }
    if (!(ser_io = (struct IOExtSer *)CreateIORequest(ser_port, sizeof(struct IOExtSer)))) { ser_close(); return FALSE; }
    ser_io->io_SerFlags = rtscts ? SERF_7WIRE : 0;
    if (OpenDevice((STRPTR)dev, unit, (struct IORequest *)ser_io, 0)) { ser_close(); return FALSE; }
    ser_open_ok = TRUE;
    ser_stuck = FALSE;
    ser_io->io_Baud = baud;
    ser_io->io_RBufLen = 16384;
    ser_io->io_ReadLen = ser_io->io_WriteLen = 8;
    ser_io->io_StopBits = 1;
    ser_io->io_SerFlags = (rtscts ? SERF_7WIRE : 0) | SERF_XDISABLED;
    ser_io->IOSer.io_Command = SDCMD_SETPARAMS;
    DoIO((struct IORequest *)ser_io);           /* a speed it refuses: it keeps its own */
    if (!(ser_rd = (struct IOExtSer *)CreateIORequest(ser_rport, sizeof(struct IOExtSer)))) { ser_close(); return FALSE; }
    CopyMem(ser_io, ser_rd, sizeof(struct IOExtSer));
    ser_rd->IOSer.io_Message.mn_ReplyPort = ser_rport;
    ser_arm();
    return TRUE;
}

/* sleep until one of `mask` or `ms` pass (timer.device); returns the signals that came */
static ULONG sig_wait(ULONG ms, ULONG mask)
{
    ULONG got, tsig;
    if (!tm_io) { got = SetSignal(0, mask) & mask; if (!got) { Delay(ms / 20 + 1); got = SetSignal(0, mask) & mask; } return got; }
    tsig = 1UL << tm_port->mp_SigBit;
    tm_io->tr_node.io_Command = TR_ADDREQUEST;
    tm_io->tr_time.tv_secs = ms / 1000;
    tm_io->tr_time.tv_micro = (ms % 1000) * 1000;
    SetSignal(0, tsig);
    SendIO((struct IORequest *)tm_io);
    got = Wait(mask | tsig);
    if (!CheckIO((struct IORequest *)tm_io)) AbortIO((struct IORequest *)tm_io);
    WaitIO((struct IORequest *)tm_io);
    return got & mask;
}

/* microseconds, for the speed emulation */
static ULONG now_us(void)
{
    struct timeval tv;
    if (!TimerBase) { struct DateStamp ds; DateStamp(&ds); return (ULONG)(ds.ds_Minute * 60 + ds.ds_Tick / 50) * 1000000UL + (ds.ds_Tick % 50) * 20000UL; }
    GetSysTime(&tv);
    return tv.tv_secs * 1000000UL + tv.tv_micro;
}

/*
 * Wait up to ms for input (ms 0: just look).  Returns the bytes read into buf, 0 for none,
 * -1 when the other end has gone; *sigs gets whichever of `mask` arrived.
 */
static LONG conn_wait(UBYTE *buf, LONG max, ULONG ms, ULONG mask, ULONG *sigs)
{
    LONG n;
    *sigs = 0;
    if (ctype == CT_MODEM) {
        if (!ser_open_ok) return -1;
        if ((n = ser_read(buf, max)) > 0) { *sigs = SetSignal(0, 0) & mask; return n; }
        if (!ms) { *sigs = SetSignal(0, 0) & mask; return 0; }
        *sigs = sig_wait(ms, mask | (1UL << ser_rport->mp_SigBit)) & mask;
        return ser_read(buf, max);
    }
    if (sock < 0) { if (ms) *sigs = sig_wait(ms, mask); return -1; }
    {
        fd_set r;
        struct timeval tv;
        ULONG s = mask;
        FD_ZERO(&r);
        FD_SET(sock, &r);
        tv.tv_secs = ms / 1000; tv.tv_micro = (ms % 1000) * 1000;
        n = WaitSelect(sock + 1, &r, NULL, NULL, &tv, ms ? &s : NULL);
        *sigs = ms ? (s & mask) : (SetSignal(0, 0) & mask);
        if (n < 0) return -1;
        if (n > 0 && FD_ISSET(sock, &r)) {
            n = recv(sock, buf, max, 0);
            return n > 0 ? n : -1;
        }
    }
    return 0;
}

static void net_send(const UBYTE *p, int n)
{
    if (ctype == CT_MODEM) { ser_write(p, n); return; }
    while (n > 0 && sock >= 0) {
        LONG k = send(sock, (APTR)p, n, 0);
        if (k <= 0) { closed = TRUE; return; }
        p += k; n -= k;
    }
}
static void ob_flush(void) { if (obn) net_send(ob, obn); obn = 0; }
static void ob_put(UBYTE c) { if (obn >= (int)sizeof(ob)) ob_flush(); ob[obn++] = c; }
static void ob_str(const char *s) { while (*s) ob_put((UBYTE)*s++); }
static void ob_data(UBYTE c) { ob_put(c); if (c == 255 && ctype == CT_TELNET) ob_put(255); }   /* IAC doubled */

/* ---- drawing ------------------------------------------------------------------------ */
static void mark(int y, int x0, int x1)
{
    if (x0 < dmin[y]) dmin[y] = x0;
    if (x1 > dmax[y]) dmax[y] = x1;
}

static void draw_cells(int y, int x0, int x1)
{
#define EATT(y, x) (blink_off && blk[y][x] ? (UBYTE)((att[y][x] & 0xF0) | (att[y][x] >> 4)) : att[y][x])
    while (x0 <= x1) {
        UBYTE a = EATT(y, x0);
        int e = x0 + 1;
        while (e <= x1 && EATT(y, e) == a) e++;
        SetABPenDrMd(rp, a & 15, a >> 4, JAM2);
        Move(rp, x0 * 8, y * CH + vfont.tf_Baseline);
        Text(rp, (STRPTR)&chr[y][x0], e - x0);
        x0 = e;
    }
}

/* put the pending scroll and every dirty span on the screen */
static void flush_draw(void)
{
    int y;
    if (pend_n) {
        int h = pend_bot - pend_top + 1;
        if (pend_n < h)
            ScrollRaster(rp, 0, pend_n * CH, 0, pend_top * CH, COLS * 8 - 1, (pend_bot + 1) * CH - 1);
        pend_n = 0;             /* (a scroll of the whole region left every row dirty anyway) */
    }
    for (y = 0; y < SROWS; y++)
        if (dmin[y] <= dmax[y]) {
            draw_cells(y, dmin[y], dmax[y]);
            dmin[y] = COLS; dmax[y] = -1;
        }
}

static void cursor_hide(void)
{
    if (!cur_drawn) return;
    draw_cells(cur_dy, cur_dx, cur_dx);
    cur_drawn = FALSE;
}

static void cursor_show(void)
{
    int x = cx < COLS ? cx : COLS - 1;
    if (!curvis || cur_drawn) return;
    SetAPen(rp, (att[cy][x] & 15) == (att[cy][x] >> 4) ? 7 : att[cy][x] & 15);
    SetDrMd(rp, JAM1);
    RectFill(rp, x * 8, cy * CH + CH - 2, x * 8 + 7, cy * CH + CH - 1);
    cur_drawn = TRUE; cur_dx = x; cur_dy = cy;
}

static void flush(void) { flush_draw(); cursor_show(); }

/* ---- the cell buffer ------------------------------------------------------------------ */
static void blank(int y, int x0, int x1)
{
    int x;
    if (x0 < 0) x0 = 0;
    if (x1 > COLS - 1) x1 = COLS - 1;
    if (x0 > x1) return;
    for (x = x0; x <= x1; x++) { chr[y][x] = ' '; att[y][x] = curattr; blk[y][x] = 0; }
    mark(y, x0, x1);
}

/* ---- scrollback: lines that scroll off the top of the whole screen ------------------------- */
#define SBMAX 2000
static UBYTE *sb_chr, *sb_att;          /* SBMAX lines of COLS, a ring */
static int sb_head, sb_count;           /* next slot, lines held */

static void sb_push(int y)
{
    if (!sb_chr) return;
    memcpy(sb_chr + sb_head * COLS, chr[y], COLS);
    memcpy(sb_att + sb_head * COLS, att[y], COLS);
    sb_head = (sb_head + 1) % SBMAX;
    if (sb_count < SBMAX) sb_count++;
}

static void scroll_up(int top, int bot, int n)
{
    int h = bot - top + 1, y;
    if (n <= 0 || top > bot) return;
    if (n > h) n = h;
    if (top == 0 && bot == ROWS - 1)            /* a whole-screen scroll: keep what goes off the top */
        for (y = 0; y < n; y++) sb_push(y);
    if (pend_n && (pend_top != top || pend_bot != bot)) flush_draw();
    for (y = top; y + n <= bot; y++) {
        memcpy(chr[y], chr[y + n], COLS);
        memcpy(att[y], att[y + n], COLS);
        memcpy(blk[y], blk[y + n], COLS);
        dmin[y] = dmin[y + n]; dmax[y] = dmax[y + n];
    }
    for (y = bot - n + 1; y <= bot; y++) { dmin[y] = COLS; dmax[y] = -1; blank(y, 0, COLS - 1); }
    pend_top = top; pend_bot = bot;
    pend_n += n;
    if (pend_n > h) pend_n = h;
}

static void scroll_down(int top, int bot, int n)
{
    int h = bot - top + 1, y;
    if (n <= 0 || top > bot) return;
    if (n > h) n = h;
    flush_draw();               /* the screen matches the buffer now: move both the same way */
    if (n < h) ScrollRaster(rp, 0, -n * CH, 0, top * CH, COLS * 8 - 1, (bot + 1) * CH - 1);
    for (y = bot; y - n >= top; y--) {
        memcpy(chr[y], chr[y - n], COLS);
        memcpy(att[y], att[y - n], COLS);
        memcpy(blk[y], blk[y - n], COLS);
    }
    for (y = top; y < top + n; y++) blank(y, 0, COLS - 1);
}

static void linefeed(void)
{
    if (cy == sbot) scroll_up(stop, sbot, 1);
    else if (cy < ROWS - 1) cy++;
}

static int blink;
static UBYTE curblink;
static void set_attr(void)
{
    int f = fg < 8 && bold ? fg + 8 : fg, b = bg;
    if (blink && (ice || ice_force)) b |= 8;    /* iCE: blink means a bright background */
    if (rev) { int t = f; f = b; b = t; }
    curattr = (UBYTE)(f | (b << 4));
    curblink = blink && !(ice || ice_force);
}

static void reset_term(void)
{
    int y;
    fg = 7; bg = 0; bold = rev = blink = 0; ice = FALSE; set_attr();
    stop = 0; sbot = ROWS - 1; autowrap = TRUE; curvis = TRUE;
    for (y = 0; y < ROWS; y++) blank(y, 0, COLS - 1);
    cx = cy = 0;
}

static void put_glyph(UBYTE c)
{
    chr[cy][cx] = c; att[cy][cx] = curattr; blk[cy][cx] = curblink;
    mark(cy, cx, cx);
    if (++cx >= COLS) {
        /* wrap at once, as SyncTERM (cterm) does: a CR LF after 80 columns double-spaces */
        if (autowrap) { cx = 0; linefeed(); }
        else cx = COLS - 1;
    }
}

/* ---- the ANSI parser --------------------------------------------------------------- */
static int P(int i, int def) { return i < npar && par[i] > 0 ? par[i] : def; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void sgr(void)
{
    int i;
    if (npar == 0) { npar = 1; par[0] = 0; }
    for (i = 0; i < npar; i++) {
        int p = par[i];
        if (p == 0) { fg = 7; bg = 0; bold = rev = blink = 0; }
        else if (p == 1) bold = 1;
        else if (p == 5 || p == 6) blink = 1;
        else if (p == 25) blink = 0;
        else if (p == 2 || p == 22) bold = 0;
        else if (p == 7) rev = 1;
        else if (p == 27) rev = 0;
        else if (p >= 30 && p <= 37) fg = p - 30;
        else if (p == 39) fg = 7;
        else if (p >= 40 && p <= 47) bg = p - 40;
        else if (p == 49) bg = 0;
        else if (p >= 90 && p <= 97) fg = p - 90 + 8;
        else if (p >= 100 && p <= 107) bg = p - 100 + 8;
        else if (p == 38 || p == 48) {          /* 256-colour / true colour: skip its arguments */
            if (i + 1 < npar && par[i + 1] == 5) i += 2;
            else if (i + 1 < npar && par[i + 1] == 2) i += 4;
        }
        /* 4/24 underline, 8 conceal: shown as plain text */
    }
    set_attr();
}

static void csi_final(UBYTE f)
{
    int n, y;
    char tmp[24];
    if (inter) return;
    if (priv) {
        if (priv == '?' && (f == 'h' || f == 'l')) {
            int i;
            for (i = 0; i < npar; i++) {
                if (par[i] == 25) curvis = (f == 'h');
                else if (par[i] == 7) autowrap = (f == 'h');
                else if (par[i] == 33) { ice = (f == 'h'); set_attr(); }     /* iCE colours */
            }
        }
        return;                 /* ESC[=..h etc.: ignored */
    }
    switch (f) {
    case 'A': cy = clampi(cy - P(0, 1), cy >= stop ? stop : 0, ROWS - 1); break;
    case 'B': case 'e': cy = clampi(cy + P(0, 1), 0, cy <= sbot ? sbot : ROWS - 1); break;
    case 'C': case 'a': cx = clampi(cx + P(0, 1), 0, COLS - 1); break;
    case 'D': cx = clampi(cx - P(0, 1), 0, COLS - 1); break;
    case 'E': cy = clampi(cy + P(0, 1), 0, ROWS - 1); cx = 0; break;
    case 'F': cy = clampi(cy - P(0, 1), 0, ROWS - 1); cx = 0; break;
    case 'G': case '`': cx = clampi(P(0, 1) - 1, 0, COLS - 1); break;
    case 'd': cy = clampi(P(0, 1) - 1, 0, ROWS - 1); break;
    case 'H': case 'f':
        cy = clampi(P(0, 1) - 1, 0, ROWS - 1);
        cx = clampi(P(1, 1) - 1, 0, COLS - 1);
        break;
    case 'I': n = P(0, 1); while (n-- > 0) cx = (cx / 8 + 1) * 8; if (cx > COLS - 1) cx = COLS - 1; break;
    case 'Z': n = P(0, 1); while (n-- > 0 && cx > 0) cx = (cx - 1) / 8 * 8; break;
    case 'J':
        n = P(0, 0);
        if (n == 0) { blank(cy, cx, COLS - 1); for (y = cy + 1; y < ROWS; y++) blank(y, 0, COLS - 1); }
        else if (n == 1) { for (y = 0; y < cy; y++) blank(y, 0, COLS - 1); blank(cy, 0, cx); }
        else if (n == 2) { for (y = 0; y < ROWS; y++) blank(y, 0, COLS - 1); cx = cy = 0; }  /* ANSI.SYS homes */
        else if (n == 3) sb_head = sb_count = 0;    /* xterm: the scrollback only - the screen stays
                                                     * (Craps sends it with every prompt; it blanked the game) */
        break;
    case 'K':
        n = P(0, 0);
        if (n == 0) blank(cy, cx, COLS - 1);
        else if (n == 1) blank(cy, 0, cx);
        else blank(cy, 0, COLS - 1);
        break;
    case 'X': blank(cy, cx, cx + P(0, 1) - 1); break;
    case '@':               /* insert blanks */
        n = clampi(P(0, 1), 1, COLS - cx);
        memmove(&chr[cy][cx + n], &chr[cy][cx], COLS - cx - n);
        memmove(&att[cy][cx + n], &att[cy][cx], COLS - cx - n);
        memmove(&blk[cy][cx + n], &blk[cy][cx], COLS - cx - n);
        mark(cy, cx, COLS - 1);
        blank(cy, cx, cx + n - 1);
        break;
    case 'P':               /* delete characters */
        n = clampi(P(0, 1), 1, COLS - cx);
        memmove(&chr[cy][cx], &chr[cy][cx + n], COLS - cx - n);
        memmove(&att[cy][cx], &att[cy][cx + n], COLS - cx - n);
        memmove(&blk[cy][cx], &blk[cy][cx + n], COLS - cx - n);
        mark(cy, cx, COLS - 1);
        blank(cy, COLS - n, COLS - 1);
        break;
    case 'L': if (cy >= stop && cy <= sbot) scroll_down(cy, sbot, P(0, 1)); cx = 0; break;
    case 'M': if (cy >= stop && cy <= sbot) scroll_up(cy, sbot, P(0, 1)); cx = 0; break;
    case 'S': scroll_up(stop, sbot, P(0, 1)); break;
    case 'T': scroll_down(stop, sbot, P(0, 1)); break;
    case 'r': {
        int t = P(0, 1) - 1, b = P(1, ROWS) - 1;
        if (t < 0) t = 0;
        if (b > ROWS - 1) b = ROWS - 1;
        if (t < b) { stop = t; sbot = b; } else { stop = 0; sbot = ROWS - 1; }
        cx = cy = 0;
        break;
    }
    case 'm': sgr(); break;
    case 's': sv_x = cx; sv_y = cy; break;
    case 'u': cx = sv_x; cy = sv_y; break;
    case 'n':
        if (P(0, 0) == 6) {                 /* cursor position report: what the node detects us by */
            sprintf(tmp, "\x1b[%d;%dR", cy + 1, cx + 1);
            ob_str(tmp);
        } else if (P(0, 0) == 5) ob_str("\x1b[0n");
        break;
    default: break;         /* 'c' (DA) unanswered, like ANSI.SYS; 't', 'q'...: ignored */
    }
}

static void ansi_byte(UBYTE c)
{
    switch (pstate) {
    case S_SKIP1: pstate = S_NORM; return;
    case S_ESC:
        pstate = S_NORM;
        switch (c) {
        case '[': pstate = S_CSI; npar = 0; par_any = FALSE; priv = 0; inter = FALSE; par[0] = 0; return;
        case '7': sv_x = cx; sv_y = cy; sv_fg = fg; sv_bg = bg; sv_bold = bold; sv_rev = rev; return;
        case '8': cx = sv_x; cy = sv_y; fg = sv_fg; bg = sv_bg; bold = sv_bold; rev = sv_rev; set_attr(); return;
        case 'D': linefeed(); return;
        case 'E': cx = 0; linefeed(); return;
        case 'M':
            if (cy == stop) scroll_down(stop, sbot, 1);
            else if (cy > 0) cy--;
            return;
        case 'c': reset_term(); return;
        case '(': case ')': case '*': case '+': case '#': case '%': pstate = S_SKIP1; return;
        case 27: pstate = S_ESC; return;
        default: return;    /* ESC = / ESC > etc. */
        }
    case S_CSI:
        if (c >= '0' && c <= '9') {
            if (!par_any) { npar = 1; par_any = TRUE; }
            if (par[npar - 1] < 10000) par[npar - 1] = par[npar - 1] * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (!par_any) { npar = 1; par_any = TRUE; }
            if (npar < MAXPAR) par[npar++] = 0;
            return;
        }
        if (c >= '<' && c <= '?') { if (!par_any && !priv) priv = c; else inter = TRUE; return; }
        if (c == ':') { inter = TRUE; return; }
        if (c >= 0x20 && c <= 0x2F) { inter = TRUE; return; }
        if (c >= 0x40 && c <= 0x7E) { pstate = S_NORM; csi_final(c); return; }
        if (c == 27) { pstate = S_ESC; return; }
        if (c == 24 || c == 26) { pstate = S_NORM; return; }    /* CAN / SUB abort it */
        if (c >= 0x20) return;                                  /* DEL */
        /* other control characters inside a sequence act as usual */
        break;
    case S_NORM:
        break;
    }

    if (amiga_mode && c >= 0x80 && c < 0xA0) {       /* Amiga: 0x9B is CSI, the rest are controls */
        if (c == 0x9B) { pstate = S_CSI; npar = 0; par_any = FALSE; priv = 0; inter = FALSE; par[0] = 0; }
        return;
    }
    if (c >= 0x20) { put_glyph(c); return; }
    switch (c) {
    case 7: if (!beeped) { DisplayBeep(scr); beeped = TRUE; } break;
    case 8: if (cx > 0) cx--; break;
    case 9: cx = (cx / 8 + 1) * 8; if (cx > COLS - 1) cx = COLS - 1; break;
    case 10: case 11: linefeed(); break;
    case 12: { int y; for (y = 0; y < ROWS; y++) blank(y, 0, COLS - 1); cx = cy = 0; break; }  /* ^L: clear */
    case 13: cx = 0; break;
    case 27: pstate = S_ESC; break;
    default: break;         /* other control bytes: nothing */
    }
}

static void term_write(const UBYTE *p, LONG n)
{
    while (n-- > 0) ansi_byte(*p++);
}
static void term_str(const char *s) { term_write((const UBYTE *)s, strlen(s)); }

/* ---- capture: the session to a file, raw ANSI or plain text ---------------------------------- */
static BPTR cap_fh;
static BOOL cap_plain;
static UBYTE cap_buf[2048];
static int cap_n, cap_esc;              /* plain: 0 text, 1 after ESC, 2 inside ESC[ ... */

static void cap_flush(void) { if (cap_fh && cap_n) Write(cap_fh, cap_buf, cap_n); cap_n = 0; }
static void cap_put(UBYTE c) { if (cap_n >= (int)sizeof(cap_buf)) cap_flush(); cap_buf[cap_n++] = c; }

static void capture_byte(UBYTE c)
{
    if (!cap_plain) { cap_put(c); return; }
    if (cap_esc == 1) { cap_esc = c == '[' ? 2 : 0; return; }
    if (cap_esc == 2) { if (c >= 0x40 && c <= 0x7E) cap_esc = 0; return; }
    if (c == 27) { cap_esc = 1; return; }
    if (c == '\n' || c == '\t' || c >= 0x20) cap_put(c);    /* CRs dropped: LF ends a line */
}

/* ---- the current line, as text: auto-login looks for the prompts, a modem for NO CARRIER ---- */
static char lg_line[80];                /* lower case, escape sequences left out */
static int lg_n, lg_esc;
static char lg_user[40], lg_pass[40];   /* the entry's login (auto-login, Alt+L, rlogin) */
static BOOL lg_auto;                    /* answer the prompts by ourselves */
static int lg_stage;                    /* 0 user name next, 1 password next, 2 done */
static ULONG lg_until;                  /* auto-login gives up after this (seconds) */
static void line_watch(UBYTE c)
{
    if (lg_esc == 1) { lg_esc = (c == '[') ? 2 : 0; return; }
    if (lg_esc == 2) { if (c >= 0x40 && c <= 0x7E) lg_esc = 0; return; }
    if (c == 27) { lg_esc = 1; return; }
    if (c == 0x9B && amiga_mode) { lg_esc = 2; return; }
    if (c == '\r' || c == '\n') {
        lg_line[lg_n] = 0;
        if (ctype == CT_MODEM && !strncmp(lg_line, "no carrier", 10)) closed = TRUE;
        lg_n = 0;
        return;
    }
    if (c == 8) { if (lg_n) lg_n--; return; }
    if (c < 0x20) return;
    if (lg_n >= (int)sizeof(lg_line) - 1) { memmove(lg_line, lg_line + 1, lg_n - 1); lg_n--; }
    lg_line[lg_n++] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* every byte of the session that isn't telnet - the screen, the log, the ZMODEM watcher;
 * while a transfer runs, the transfer's input ring instead */
static void zm_watch(UBYTE c);
static BOOL xfer;
static void zin_put(UBYTE c);
static void data_byte(UBYTE c)
{
    if (xfer) { zin_put(c); return; }
    if (cap_fh) capture_byte(c);
    zm_watch(c);
    line_watch(c);
    ansi_byte(c);
}

/* ---- telnet --------------------------------------------------------------------------- */
#define IAC  255
#define DONT 254
#define DO   253
#define WONT 252
#define WILL 251
#define SB   250
#define SE   240
#define O_BINARY 0
#define O_ECHO   1
#define O_SGA    3
#define O_TTYPE  24
#define O_NAWS   31

static UBYTE us[256], them[256];
static enum { T_DATA, T_IAC, T_OPT, T_SB, T_SBIAC } tstate;
static UBYTE tverb;
static UBYTE sbbuf[64];
static int sblen;

static void tn_cmd(UBYTE verb, UBYTE opt) { ob_put(IAC); ob_put(verb); ob_put(opt); }

static void tn_naws(void)
{
    UBYTE naws[] = { IAC, SB, O_NAWS, 0, COLS, 0, 0, IAC, SE };
    int i;
    naws[6] = (UBYTE)ROWS;
    for (i = 0; i < (int)sizeof(naws); i++) ob_put(naws[i]);
}

static void tn_option(UBYTE verb, UBYTE opt)
{
    switch (verb) {
    case DO:                    /* may we ...? */
        if (opt == O_TTYPE || opt == O_NAWS || opt == O_BINARY || opt == O_SGA) {
            if (!us[opt]) { us[opt] = 1; tn_cmd(WILL, opt); }
            if (opt == O_NAWS) tn_naws();
        } else tn_cmd(WONT, opt);
        break;
    case DONT:
        if (us[opt]) { us[opt] = 0; tn_cmd(WONT, opt); }
        break;
    case WILL:                  /* the server offers */
        if (opt == O_ECHO || opt == O_SGA || opt == O_BINARY) {
            if (!them[opt]) { them[opt] = 1; tn_cmd(DO, opt); }
        } else tn_cmd(DONT, opt);
        break;
    case WONT:
        if (them[opt]) { them[opt] = 0; tn_cmd(DONT, opt); }
        break;
    }
}

static void tn_sb(void)
{
    if (sblen >= 2 && sbbuf[0] == O_TTYPE && sbbuf[1] == 1) {      /* TTYPE SEND -> IS "ANSI" */
        static const UBYTE is[] = { IAC, SB, O_TTYPE, 0, 'A', 'N', 'S', 'I', IAC, SE };
        int i;
        for (i = 0; i < (int)sizeof(is); i++) ob_put(is[i]);
    }
}

static void tn_byte(UBYTE c)
{
    switch (tstate) {
    case T_DATA:
        if (c == IAC) tstate = T_IAC;
        else data_byte(c);
        break;
    case T_IAC:
        tstate = T_DATA;
        if (c == IAC) data_byte(IAC);
        else if (c >= WILL && c <= DONT) { tverb = c; tstate = T_OPT; }
        else if (c == SB) { sblen = 0; tstate = T_SB; }
        break;                  /* NOP, GA, AYT...: nothing */
    case T_OPT:
        tn_option(tverb, c);
        tstate = T_DATA;
        break;
    case T_SB:
        if (c == IAC) tstate = T_SBIAC;
        else if (sblen < (int)sizeof(sbbuf)) sbbuf[sblen++] = c;
        break;
    case T_SBIAC:
        if (c == SE) { tn_sb(); tstate = T_DATA; }
        else { if (sblen < (int)sizeof(sbbuf)) sbbuf[sblen++] = c; tstate = T_SB; }
        break;
    }
}

/* a byte from the other end: telnet unpicks it; raw / rlogin / a modem pass it straight on */
static BOOL rl_first;                   /* rlogin: the server's first byte is a NUL, "go ahead" */
static void rx_byte(UBYTE c)
{
    if (ctype == CT_TELNET) { tn_byte(c); return; }
    if (rl_first) { rl_first = FALSE; if (!c) return; }
    data_byte(c);
}

/* ---- keyboard --------------------------------------------------------------------------- */
/* Latin-1 0xA0-0xFF (what the Amiga keymap gives) -> CP437 (what the BBS thinks in) */
static const UBYTE lat2cp[96] = {
    0x20,0xAD,0x9B,0x9C,0x3F,0x9D,0x3F,0x3F,0x3F,0x3F,0xA6,0xAE,0xAA,0x3F,0x3F,0x3F,
    0xF8,0xF1,0xFD,0x3F,0x3F,0xE6,0x3F,0xFA,0x3F,0x3F,0xA7,0xAF,0xAC,0xAB,0x3F,0xA8,
    0x3F,0x3F,0x3F,0x3F,0x8E,0x8F,0x92,0x80,0x3F,0x90,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,
    0x3F,0xA5,0x3F,0x3F,0x3F,0x3F,0x99,0x3F,0x3F,0x3F,0x3F,0x3F,0x9A,0x3F,0x3F,0xE1,
    0x85,0xA0,0x83,0x3F,0x84,0x86,0x91,0x87,0x8A,0x82,0x88,0x89,0x8D,0xA1,0x8C,0x8B,
    0x3F,0xA4,0x95,0xA2,0x93,0x3F,0x94,0xF6,0x3F,0x97,0xA3,0x96,0x81,0x3F,0x3F,0x98
};

static void about(void)
{
    struct EasyStruct es = {
        sizeof(struct EasyStruct), 0, (UBYTE *)"About NilTerm",
        (UBYTE *)"NilTerm " BBS_VERSION " - an ANSI BBS terminal for the Amiga\n"
                 "Connected to %s port %ld  (%s)\n\n"
                 "Font: IBM VGA 8x16 from The Ultimate Oldschool PC Font Pack\n"
                 "by VileR, https://int10h.org/oldschool-pc-fonts/  (CC BY-SA 4.0)\n\n"
                 "Alt+D  dialing directory      Alt+H  hang up\n"
                 "Alt+B  scrollback             Alt+C  capture to a file (on/off)\n"
                 "Alt+U  upload (Z/Y/XMODEM)    Alt+R  receive (Z/Y/XMODEM)\n"
                 "Alt+L  send user name + password (the directory entry's)\n"
                 "Alt+Up / Alt+Down  emulated line speed faster / slower\n"
                 "Alt+S  settings (folders, ZMODEM, modem)\n"
                 "ZMODEM downloads start by themselves; Esc cancels a transfer\n"
                 "Amiga+C  copy the screen      Amiga+V  paste\n"
                 "Alt+X / Amiga+Q  exit         Help  this\n\n"
                 "Arrows; Shift+Up/Down = PgUp/PgDn; Shift+Left/Right = Home/End.",
        (UBYTE *)"OK"
    };
    extern char host_name[];
    extern LONG host_port;
    ULONG args[3];
    args[0] = (ULONG)host_name; args[1] = (ULONG)host_port; args[2] = (ULONG)modename;
    EasyRequestArgs(win, &es, NULL, (APTR)args);
}

static BOOL quit;
/* hotkeys the session loop acts on (key() only records them) */
enum { ACT_NONE, ACT_DIR, ACT_HANGUP, ACT_BACK, ACT_LOG, ACT_UPLOAD, ACT_RECV, ACT_SET, ACT_COPY, ACT_PASTE,
       ACT_LOGIN };
static int action;
static int rate_steps;                  /* Alt+Up / Alt+Down presses not acted on yet */
static int watch_node;          /* WATCH=n: show node n's caller's screen (read-only) instead of logging on */

static void send_key_str(const char *s) { ob_str(s); ob_flush(); }

static void local_echo(const UBYTE *p, int n)
{
    if (them[O_ECHO]) return;           /* the BBS echoes (it always does) */
    cursor_hide();
    term_write(p, n);
    flush();
}

static void key(struct IntuiMessage *m, UWORD code, UWORD qual, APTR prev)
{
    BOOL shift = (qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) != 0;
    BOOL amigakey = (qual & (IEQUALIFIER_LCOMMAND | IEQUALIFIER_RCOMMAND)) != 0;
    struct InputEvent ie;
    UBYTE buf[16];
    LONG n, i;

    if (code & IECODE_UP_PREFIX) return;
    if (!watch_node && (qual & (IEQUALIFIER_LALT | IEQUALIFIER_RALT)) && !amigakey) {
        switch (code) {                 /* raw key positions, whatever the keymap */
        case 0x22: action = ACT_DIR; return;        /* D */
        case 0x25: action = ACT_HANGUP; return;     /* H */
        case 0x32: quit = TRUE; return;             /* X */
        case 0x35: action = ACT_BACK; return;       /* B */
        case 0x33: action = ACT_LOG; return;        /* C: capture (as SyncTERM) */
        case 0x28: action = ACT_LOGIN; return;      /* L: send the user name / password */
        case 0x4C: rate_steps++; return;            /* Up: the speed emulation faster */
        case 0x4D: rate_steps--; return;            /* Down: slower */
        case 0x16: action = ACT_UPLOAD; return;     /* U */
        case 0x13: action = ACT_RECV; return;       /* R */
        case 0x21: action = ACT_SET; return;        /* S */
        }
    }
    if (!watch_node && amigakey) {
        if (code == 0x33) { action = ACT_COPY; return; }    /* Amiga+C */
        if (code == 0x34) { action = ACT_PASTE; return; }   /* Amiga+V */
    }
    if (watch_node) {                   /* watching: nothing goes to the caller - Esc/Q quit */
        if (code == 0x45 || code == 0x10) quit = TRUE;
        else if (code == 0x5F) about();
        return;
    }
    switch (code) {
    case 0x4C: send_key_str(shift ? "\x1b[5~" : "\x1b[A"); return;     /* up    (PgUp) */
    case 0x4D: send_key_str(shift ? "\x1b[6~" : "\x1b[B"); return;     /* down  (PgDn) */
    case 0x4E: send_key_str(shift ? "\x1b[K" : "\x1b[C"); return;      /* right (End)  */
    case 0x4F: send_key_str(shift ? "\x1b[H" : "\x1b[D"); return;      /* left  (Home) */
    case 0x46: send_key_str("\x1b[3~"); return;                        /* Del */
    case 0x47: send_key_str("\x1b[2~"); return;                        /* Insert (PC keyboard) */
    case 0x48: send_key_str("\x1b[5~"); return;                        /* Page Up */
    case 0x49: send_key_str("\x1b[6~"); return;                        /* Page Down */
    case 0x70: send_key_str("\x1b[H"); return;                         /* Home */
    case 0x71: send_key_str("\x1b[K"); return;                         /* End (as SyncTERM sends it) */
    case 0x5F: about(); return;                                        /* Help */
    case 0x41: if (amigakey) return; buf[0] = 8; ob_put(8); ob_flush(); local_echo(buf, 1); return;
    case 0x43: case 0x44: if (amigakey) return; ob_put('\r'); ob_flush(); buf[0] = '\r'; buf[1] = '\n'; local_echo(buf, 2); return;
    }
    ie.ie_NextEvent = NULL;
    ie.ie_Class = IECLASS_RAWKEY;
    ie.ie_SubClass = 0;
    ie.ie_Code = code;
    ie.ie_Qualifier = qual & ~(IEQUALIFIER_LCOMMAND | IEQUALIFIER_RCOMMAND);  /* Amiga-Q: the plain letter */
    ie.ie_EventAddress = prev;
    n = MapRawKey(&ie, (STRPTR)buf, sizeof(buf), NULL);
    if (n <= 0) return;
    if (amigakey) {
        if (n == 1 && (buf[0] == 'q' || buf[0] == 'Q')) quit = TRUE;
        return;                         /* other Amiga-key combinations are the system's */
    }
    if (buf[0] == 0x9B) return;         /* F-keys etc.: CSI sequences we don't pass on */
    for (i = 0; i < n; i++) {
        UBYTE c = buf[i];
        if (c >= 0xA0) { if (!amiga_mode) c = lat2cp[c - 0xA0]; }     /* an Amiga BBS: Latin-1 as it is */
        else if (c >= 0x80) continue;
        buf[i] = c;
        ob_data(c);
    }
    ob_flush();
    local_echo(buf, n);
}

static void handle_idcmp(void)
{
    struct IntuiMessage *m;
    while ((m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
        ULONG cls = m->Class;
        UWORD code = m->Code, qual = m->Qualifier;
        APTR prev = m->IAddress ? *(APTR *)m->IAddress : NULL;
        if (cls == IDCMP_REFRESHWINDOW) { BeginRefresh(win); EndRefresh(win, TRUE); }
        ReplyMsg((struct Message *)m);
        if (cls == IDCMP_RAWKEY) key(NULL, code, qual, prev);
        if (closed || quit) break;
    }
}

/* ---- the screen: mode, font, open/close ---------------------------------------------------- */
static BOOL mode_fits(ULONG id, int w, int h, BOOL bounded)
{
    struct DimensionInfo dim;
    int nw, nh;
    if (id == INVALID_ID || ModeNotAvailable(id)) return FALSE;
    if (!GetDisplayInfoData(NULL, (UBYTE *)&dim, sizeof(dim), DTAG_DIMS, id)) return FALSE;
    nw = dim.Nominal.MaxX - dim.Nominal.MinX + 1;
    nh = dim.Nominal.MaxY - dim.Nominal.MinY + 1;
    if (dim.MaxDepth < 4 || nw < w || nh < h) return FALSE;
    if (bounded && (nw > 800 || nh > 600)) return FALSE;   /* a 1024x768 card mode: rather lace */
    return TRUE;
}

/* the glyphs: the IBM VGA font (CP437), or the ROM's Topaz 8 (Latin-1) for an Amiga BBS,
 * each row doubled on a 16-line cell */
static void font_glyphs(BOOL topaz)
{
    struct TextAttr ta = { (STRPTR)"topaz.font", 8, FS_NORMAL, FPF_ROMFONT };
    struct TextFont *tf = topaz ? OpenFont(&ta) : NULL;
    int c, r;
    if (topaz && tf && tf->tf_YSize == 8 && !(tf->tf_Flags & FPF_PROPORTIONAL)) {
        memset(strip, 0, 256 * CH);
        for (c = tf->tf_LoChar; c <= tf->tf_HiChar && c < 256; c++) {
            ULONG loc = ((ULONG *)tf->tf_CharLoc)[c - tf->tf_LoChar];
            int off = loc >> 16, w = loc & 0xFFFF;
            if (c < 0x20 || (c >= 0x7F && c < 0xA0) || !w) continue;
            if (w > 8) w = 8;
            for (r = 0; r < 8; r++) {
                const UBYTE *row = (const UBYTE *)tf->tf_CharData + r * tf->tf_Modulo;
                UBYTE b = 0;
                int i;
                for (i = 0; i < w; i++)
                    if (row[(off + i) >> 3] & (0x80 >> ((off + i) & 7))) b |= 0x80 >> i;
                if (CH == 16) { strip[(2 * r) * 256 + c] = b; strip[(2 * r + 1) * 256 + c] = b; }
                else strip[r * 256 + c] = b;
            }
        }
    } else {
        for (c = 0; c < 256; c++)
            for (r = 0; r < CH; r++)
                strip[r * 256 + c] = CH == 16 ? nilfont[c][r] : (nilfont[c][2 * r] | nilfont[c][2 * r + 1]);
    }
    if (tf) CloseFont(tf);
}

static BOOL make_font(void)
{
    int c;
    if (!(strip = AllocVec(256 * CH, MEMF_CHIP | MEMF_CLEAR))) return FALSE;
    font_glyphs(FALSE);
    for (c = 0; c < 256; c++) charloc[c] = ((ULONG)(c * 8) << 16) | 8;
    charloc[256] = charloc['?'];
    memset(&vfont, 0, sizeof(vfont));
    vfont.tf_Message.mn_Node.ln_Name = (char *)"nilterm.font";
    vfont.tf_Message.mn_Node.ln_Type = NT_FONT;
    vfont.tf_YSize = CH;
    vfont.tf_Style = FS_NORMAL;
    vfont.tf_Flags = FPF_DESIGNED;
    vfont.tf_XSize = 8;
    vfont.tf_Baseline = CH == 16 ? 12 : 6;
    vfont.tf_BoldSmear = 1;
    vfont.tf_Accessors = 1;
    vfont.tf_LoChar = 0;
    vfont.tf_HiChar = 255;
    vfont.tf_CharData = strip;
    vfont.tf_Modulo = 256;
    vfont.tf_CharLoc = charloc;
    return TRUE;
}

static BOOL try_open(ULONG id, int ch)
{
    static ULONG colors[16 * 3 + 2];
    static UWORD pens[] = { 0, 15, 0, 15, 8, 4, 15, 7, 15, 0, 7, 0, (UWORD)~0 };
    int i;
    colors[0] = (16UL << 16) | 0;
    for (i = 0; i < 16; i++) {
        colors[1 + i * 3]     = vga[i][0] * 0x01010101UL;
        colors[1 + i * 3 + 1] = vga[i][1] * 0x01010101UL;
        colors[1 + i * 3 + 2] = vga[i][2] * 0x01010101UL;
    }
    colors[1 + 48] = 0;
    scr = OpenScreenTags(NULL,
        SA_DisplayID, id,
        SA_Width, COLS * 8, SA_Height, SROWS * ch, SA_Depth, 4,
        SA_Type, CUSTOMSCREEN, SA_Quiet, TRUE, SA_ShowTitle, FALSE,
        SA_Title, (ULONG)"NilTerm",
        SA_Colors32, (ULONG)colors,
        SA_Pens, (ULONG)pens,
        SA_Interleaved, TRUE,
        SA_AutoScroll, TRUE,
        TAG_END);
    if (!scr) return FALSE;
    CH = ch;
    modeid = id;
    {
        struct NameInfo ni;
        if (GetDisplayInfoData(NULL, (UBYTE *)&ni, sizeof(ni), DTAG_NAME, id))
            strncpy(modename, (char *)ni.Name, DISPLAYNAMELEN);
        else sprintf(modename, "mode 0x%08lx", id);
    }
    return TRUE;
}

static BOOL open_screen(ULONG want_id, BOOL native, BOOL small)
{
    static const ULONG lace[] = { DEFAULT_MONITOR_ID | HIRESLACE_KEY, PAL_MONITOR_ID | HIRESLACE_KEY,
                                  NTSC_MONITOR_ID | HIRESLACE_KEY };
    static const ULONG hires[] = { DEFAULT_MONITOR_ID | HIRES_KEY, PAL_MONITOR_ID | HIRES_KEY,
                                   NTSC_MONITOR_ID | HIRES_KEY };
    ULONG id;
    int i;

    if (want_id != INVALID_ID) {
        if (mode_fits(want_id, 640, 400, FALSE) && try_open(want_id, 16)) return TRUE;
        if (mode_fits(want_id, 640, 200, FALSE) && try_open(want_id, 8)) return TRUE;
    }
    if (!small) {
        if (!native) {
            id = BestModeID(BIDTAG_NominalWidth, 640, BIDTAG_NominalHeight, 400,
                            BIDTAG_DesiredWidth, 640, BIDTAG_DesiredHeight, 400,
                            BIDTAG_Depth, 4, TAG_END);
            if (mode_fits(id, 640, 400, TRUE) && try_open(id, 16)) return TRUE;
        }
        for (i = 0; i < 3; i++)
            if (mode_fits(lace[i], 640, 400, FALSE) && try_open(lace[i], 16)) return TRUE;
    }
    /* no 400-line mode: 640x200, half-height glyphs */
    if (!native && !small) {
        id = BestModeID(BIDTAG_NominalWidth, 640, BIDTAG_NominalHeight, 200,
                        BIDTAG_DesiredWidth, 640, BIDTAG_DesiredHeight, 200, BIDTAG_Depth, 4, TAG_END);
        if (mode_fits(id, 640, 200, TRUE) && try_open(id, 8)) return TRUE;
    }
    for (i = 0; i < 3; i++)
        if (mode_fits(hires[i], 640, 200, FALSE) && try_open(hires[i], 8)) return TRUE;
    return FALSE;
}

/* ---- main ------------------------------------------------------------------------------ */
char host_name[128] = "127.0.0.1";
LONG host_port = 0;                         /* 0 = find it: the running NilBBS, then NilBBS.cfg, then 2323 */
static ULONG arg_modeid = INVALID_ID;
static BOOL arg_native, arg_small, from_cli;

static void say(const char *s) { if (from_cli) { PutStr((STRPTR)s); Flush(Output()); } }

static BOOL connect_host(void)
{
    struct sockaddr_in sa;
    char msg[200];
    if (!SocketBase) {
        term_str("\x1b[1;31mNilTerm: no bsdsocket.library - start your TCP/IP stack first\x1b[0m\r\n");
        return FALSE;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((UWORD)host_port);
    sa.sin_addr.s_addr = inet_addr((STRPTR)host_name);
    if (sa.sin_addr.s_addr == (ULONG)-1) {
        struct hostent *he = gethostbyname((STRPTR)host_name);
        if (!he) {
            sprintf(msg, "\x1b[1;31mNilTerm: can't find host %s\x1b[0m\r\n", host_name);
            term_str(msg);
            return FALSE;
        }
        memcpy(&sa.sin_addr, he->h_addr, 4);
    }
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) return FALSE;
    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        sprintf(msg, "\x1b[1;31mNilTerm: no answer from %s port %ld - is NilBBS running?\x1b[0m\r\n",
                host_name, (long)host_port);
        term_str(msg);
        CloseSocket(sock); sock = -1;
        return FALSE;
    }
    if (ctype == CT_RLOGIN) {
        /* \0 client-user \0 server-user \0 terminal/speed \0.  As SyncTERM does it (and
         * Synchronet expects it): the password goes as the client user, the name as the server one */
        UBYTE hs[128];
        int n = 0;
        const char *cu = lg_pass[0] ? lg_pass : lg_user;
        hs[n++] = 0;
        strcpy((char *)hs + n, cu); n += strlen(cu) + 1;
        strcpy((char *)hs + n, lg_user); n += strlen(lg_user) + 1;
        strcpy((char *)hs + n, "ansi-bbs/115200"); n += 16;
        net_send(hs, n);
        rl_first = TRUE;
        lg_stage = 1;                   /* the name went in the handshake; a password prompt may follow */
    }
    return TRUE;
}

static void wait_any_key(int secs);
static void default_port(void);

/* WATCH=n: the node copies everything it sends its caller to our public port
 * NILBBS.WATCH.<n> (node/spy.c) - the last 4 KB first, so we start with the screen as it
 * is.  The bytes are the telnet stream as sent, which tn_byte() already understands; our
 * answers (cursor reports, telnet replies) go nowhere since there's no socket.  The caller
 * is never told.  Esc / Q / Amiga-Q stop; so does the caller leaving. */
static int watch_loop(void)
{
    struct BBSShared *S = shared_find();
    struct MsgPort *port;
    char name[32], msg[160];
    int idle = 0, n = watch_node;
    if (!S) { term_str("\x1b[1;31mNilTerm: NilBBS isn't running\x1b[0m\r\n"); wait_any_key(10); return RETURN_WARN; }
    if (n < 1 || n > S->nodes || S->node[n - 1].state == NS_FREE) {
        sprintf(msg, "\x1b[1;31mNilTerm: nobody on node %d\x1b[0m\r\n", n);
        term_str(msg); wait_any_key(10); return RETURN_WARN;
    }
    sprintf(name, SPY_PORTFMT, n);
    if (!(port = CreateMsgPort())) return RETURN_FAIL;
    Forbid();
    if (FindPort((STRPTR)name)) {
        Permit(); DeleteMsgPort(port);
        term_str("\x1b[1;31mNilTerm: that node is already being watched\x1b[0m\r\n"); wait_any_key(10);
        return RETURN_WARN;
    }
    port->mp_Node.ln_Name = name;
    port->mp_Node.ln_Pri = 0;
    AddPort(port);
    Permit();
    bbs_log(BBS_SYSLOG, "NilTerm: sysop watching node %ld (%s)", (LONG)n,
            S->node[n - 1].user[0] ? S->node[n - 1].user : "(logging in)");
    S->node[n - 1].spy = 1;                     /* the node starts copying, history first */
    while (!quit) {
        ULONG mask = (1UL << port->mp_SigBit) | (1UL << win->UserPort->mp_SigBit) | SIGBREAKF_CTRL_C;
        ULONG got = SetSignal(0, mask) & mask;  /* polled: the 0.2 s tick notices a caller who left */
        struct SpyMsg *m;
        if (got & SIGBREAKF_CTRL_C) break;
        if (got & (1UL << win->UserPort->mp_SigBit)) handle_idcmp();
        if ((m = (struct SpyMsg *)GetMsg(port))) {
            cursor_hide();
            beeped = FALSE;
            do { LONG i; for (i = 0; i < m->len; i++) tn_byte(m->data[i]); FreeVec(m); }
            while ((m = (struct SpyMsg *)GetMsg(port)));
            obn = 0;                            /* replies meant for a socket: dropped */
            flush();
        }
        if (S->node[n - 1].state == NS_FREE) {  /* ~0.6 s free: the caller has gone */
            if (++idle > 2) break;
        } else idle = 0;
        if (!got) Delay(10);
    }
    S->node[n - 1].spy = 0;
    Forbid();
    RemPort(port);
    Permit();
    { struct Message *m; while ((m = GetMsg(port))) FreeVec(m); }
    DeleteMsgPort(port);
    if (S->node[n - 1].state == NS_FREE && !quit) {
        term_str("\r\n\x1b[0;36m-- the caller has left --\x1b[0m\r\n");
        flush();
        Delay(100);
    }
    return RETURN_OK;
}

static void wait_any_key(int secs)
{
    /* show an error until a key is pressed (or a while passes) */
    int ticks = secs * 10;
    flush();
    while (ticks-- > 0) {
        struct IntuiMessage *m;
        BOOL got = FALSE;
        while ((m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            if (m->Class == IDCMP_RAWKEY && !(m->Code & IECODE_UP_PREFIX)) got = TRUE;
            ReplyMsg((struct Message *)m);
        }
        if (got || (SetSignal(0, 0) & SIGBREAKF_CTRL_C)) break;
        Delay(5);
    }
}

/* ---- the status bar (the 25th line, under an 80x24 terminal) ---------------------------------- */
static BOOL statusbar;                  /* off in WATCH= mode: the node's own 25 lines fill the screen */
static char st_name[48];                /* the system we're on, or what the directory is doing */
static ULONG st_since;                  /* seconds, when the call started (0 = not online) */
static char st_mode[64];                /* scrollback / transfer progress, shown instead of the clock */
static LONG rate;                       /* the emulated line speed, bps (0 = as fast as it comes) */

static ULONG now_secs(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return (ULONG)ds.ds_Days * 86400 + ds.ds_Minute * 60 + ds.ds_Tick / TICKS_PER_SECOND;
}

static void status_draw(void)
{
    char line[COLS + 16], t[24];
    int y = SROWS - 1, x, n;
    if (!statusbar) return;
    if (st_mode[0]) {                   /* a wider field: "BACK 23", transfer progress */
        n = sprintf(line, " %-24.24s %.53s", st_name, st_mode);
        goto fill;
    }
    if (st_since) {
        ULONG s = now_secs() - st_since;
        sprintf(t, "%02lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    } else strcpy(t, "offline");
    {
        char r[12] = "";
        if (rate) sprintf(r, "%ld", (long)rate);
        n = sprintf(line, " %-19.19s %-8s %3s %6s  Alt: D-ir H-ang B-ack C-ap U-p R-cv X",
                    st_name, t, cap_fh ? "CAP" : "   ", r);
    }
fill:
    for (x = 0; x < COLS; x++) {
        chr[y][x] = x < n ? (UBYTE)line[x] : ' ';
        att[y][x] = 0x70;               /* black on grey, like SyncTERM's */
        blk[y][x] = 0;
    }
    mark(y, 0, COLS - 1);
}

/* ---- keys for the menus (the directory, prompts, scrollback) -------------------------------- */
#define K_UP    0x100
#define K_DOWN  0x101
#define K_PGUP  0x102
#define K_PGDN  0x103
#define K_HOME  0x104
#define K_END   0x105
#define K_ESC   27
#define K_ENTER 13
#define K_BS    8
#define K_DEL   0x106
#define K_HELP  0x107
#define K_QUIT  0x108               /* Amiga+Q, Alt+X, CTRL-C */
#define K_LEFT  0x109
#define K_RIGHT 0x10A

static int ui_key(void)
{
    for (;;) {
        struct IntuiMessage *m;
        while ((m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            ULONG cls = m->Class;
            UWORD code = m->Code, qual = m->Qualifier;
            APTR prev = m->IAddress ? *(APTR *)m->IAddress : NULL;
            BOOL shift = (qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) != 0;
            BOOL amigak = (qual & (IEQUALIFIER_LCOMMAND | IEQUALIFIER_RCOMMAND)) != 0;
            BOOL alt = (qual & (IEQUALIFIER_LALT | IEQUALIFIER_RALT)) != 0;
            struct InputEvent ie;
            UBYTE b[8];
            LONG n;
            if (cls == IDCMP_REFRESHWINDOW) { BeginRefresh(win); EndRefresh(win, TRUE); }
            ReplyMsg((struct Message *)m);
            if (cls != IDCMP_RAWKEY || (code & IECODE_UP_PREFIX)) continue;
            switch (code) {
            case 0x4C: return shift ? K_PGUP : K_UP;
            case 0x4D: return shift ? K_PGDN : K_DOWN;
            case 0x4E: return K_RIGHT;
            case 0x4F: return K_LEFT;
            case 0x48: return K_PGUP;
            case 0x49: return K_PGDN;
            case 0x70: return K_HOME;
            case 0x71: return K_END;
            case 0x45: return K_ESC;
            case 0x43: case 0x44: return K_ENTER;
            case 0x41: return K_BS;
            case 0x46: return K_DEL;
            case 0x5F: return K_HELP;
            }
            if (alt && code == 0x32) return K_QUIT;             /* Alt+X */
            ie.ie_NextEvent = NULL; ie.ie_Class = IECLASS_RAWKEY; ie.ie_SubClass = 0;
            ie.ie_Code = code; ie.ie_Qualifier = qual & ~(IEQUALIFIER_LCOMMAND | IEQUALIFIER_RCOMMAND);
            ie.ie_EventAddress = prev;
            n = MapRawKey(&ie, (STRPTR)b, sizeof(b), NULL);
            if (n != 1) continue;
            if (amigak) { if (b[0] == 'q' || b[0] == 'Q') return K_QUIT; continue; }
            if (b[0] >= 0x20 && b[0] != 0x7F && b[0] != 0x9B) return b[0];
        }
        if (Wait((1UL << win->UserPort->mp_SigBit) | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) return K_QUIT;
    }
}

/* write a line of text at (x,y) in one attribute, straight into the cells */
static void put_at(int y, int x, UBYTE a, const char *s, int width)
{
    int i;
    for (i = 0; i < width && x + i < COLS; i++) {
        chr[y][x + i] = *s ? (UBYTE)*s++ : ' ';
        att[y][x + i] = a;
        blk[y][x + i] = 0;
    }
    mark(y, x, x + i - 1);
}

/* a one-line editor on row y: returns FALSE on Esc (buf left as it was); `hide` shows *s */
static BOOL edit_line_x(int y, const char *prompt, char *buf, int max, BOOL hide)
{
    char w[128], shown[128];
    int len, pl = strlen(prompt), k;
    strncpy(w, buf, sizeof(w) - 1); w[sizeof(w) - 1] = 0;
    if (max > (int)sizeof(w) - 1) max = sizeof(w) - 1;
    if (max > COLS - pl - 3) max = COLS - pl - 3;
    len = strlen(w); if (len > max) { len = max; w[len] = 0; }
    for (;;) {
        put_at(y, 0, 0x0F, "", COLS);
        put_at(y, 1, 0x0B, prompt, pl);
        if (hide) { memset(shown, '*', len); shown[len] = 0; }
        put_at(y, pl + 2, 0x4F, hide ? shown : w, max + 1);
        cursor_hide();
        cx = pl + 2 + len; cy = y;
        curvis = TRUE;
        status_draw();
        flush();
        k = ui_key();
        if (k == K_ESC || k == K_QUIT) { put_at(y, 0, 0x07, "", COLS); cursor_hide(); flush(); return FALSE; }
        if (k == K_ENTER) break;
        if (k == K_BS) { if (len > 0) w[--len] = 0; continue; }
        if (k >= 0x20 && k < 0x100 && len < max) { w[len++] = (char)k; w[len] = 0; }
    }
    put_at(y, 0, 0x07, "", COLS);
    cursor_hide();
    flush();
    strcpy(buf, w);
    return TRUE;
}
static BOOL edit_line(int y, const char *prompt, char *buf, int max) { return edit_line_x(y, prompt, buf, max, FALSE); }

/* ---- scrollback viewer (Alt+B): the saved lines, then the screen as it is ------------------- */
static void draw_row_from(int y, const UBYTE *c, const UBYTE *a)
{
    memcpy(chr[y], c, COLS);
    memcpy(att[y], a, COLS);
    memset(blk[y], 0, COLS);
    mark(y, 0, COLS - 1);
}

static void scrollback(void)
{
    static UBYTE sc[SROWS][COLS], sa[SROWS][COLS], sk[SROWS][COLS];      /* the live screen, put back afterwards */
    int total, top, y, k, oldx = cx, oldy = cy;
    BOOL oldvis = curvis;
    memcpy(sc, chr, sizeof(sc)); memcpy(sa, att, sizeof(sa)); memcpy(sk, blk, sizeof(sk));
    total = sb_count + ROWS;
    top = sb_count;                                    /* first shown line: the screen itself */
    if (!sb_count) { DisplayBeep(scr); return; }
    cursor_hide();
    curvis = FALSE;
    for (;;) {
        for (y = 0; y < ROWS; y++) {
            int l = top + y;
            if (l < sb_count) {
                int slot = (sb_head - sb_count + l + SBMAX) % SBMAX;
                draw_row_from(y, sb_chr + slot * COLS, sb_att + slot * COLS);
            } else draw_row_from(y, sc[l - sb_count], sa[l - sb_count]);
        }
        sprintf(st_mode, "BACK %d", sb_count - top > 0 ? sb_count - top : 0);
        status_draw();
        flush();
        k = ui_key();
        if (k == K_UP) top--;
        else if (k == K_DOWN) top++;
        else if (k == K_PGUP) top -= ROWS - 1;
        else if (k == K_PGDN) top += ROWS - 1;
        else if (k == K_HOME) top = 0;
        else if (k == K_END) top = sb_count;
        else break;                                    /* Esc, Enter, anything else: back */
        if (top < 0) top = 0;
        if (top > total - ROWS) top = total - ROWS;
    }
    memcpy(chr, sc, sizeof(sc)); memcpy(att, sa, sizeof(sa)); memcpy(blk, sk, sizeof(sk));
    for (y = 0; y < SROWS; y++) mark(y, 0, COLS - 1);
    st_mode[0] = 0;
    cx = oldx; cy = oldy; curvis = oldvis;
    status_draw();
    flush();
}

/* ---- a file requester (asl.library) ----------------------------------------------------- */
static BOOL ask_file(const char *title, char *path, int max, BOOL save)
{
    struct FileRequester *fr;
    BOOL ok = FALSE;
    char drawer[200], *file;
    if (!AslBase) return FALSE;
    strncpy(drawer, path, sizeof(drawer) - 1); drawer[sizeof(drawer) - 1] = 0;
    file = (char *)FilePart((STRPTR)drawer);
    {   char f[64];
        strncpy(f, file, sizeof(f) - 1); f[sizeof(f) - 1] = 0;
        *PathPart((STRPTR)drawer) = 0;
        fr = AllocAslRequestTags(ASL_FileRequest,
                ASLFR_Window, (ULONG)win, ASLFR_TitleText, (ULONG)title,
                ASLFR_InitialDrawer, (ULONG)drawer, ASLFR_InitialFile, (ULONG)f,
                ASLFR_DoSaveMode, save, ASLFR_SleepWindow, TRUE, TAG_END);
    }
    if (!fr) return FALSE;
    if (AslRequest(fr, NULL) && fr->fr_File[0]) {
        strncpy(path, (char *)fr->fr_Drawer, max - 1); path[max - 1] = 0;
        AddPart((STRPTR)path, fr->fr_File, max);
        ok = TRUE;
    }
    FreeAslRequest(fr);
    return ok;
}

/* a drawer requester (asl.library): TRUE and the drawer in `dir` if one was picked */
static BOOL ask_drawer(const char *title, char *dir, int max)
{
    struct FileRequester *fr;
    BOOL ok = FALSE;
    if (!AslBase) return FALSE;
    if (!(fr = AllocAslRequestTags(ASL_FileRequest, ASLFR_Window, (ULONG)win,
            ASLFR_TitleText, (ULONG)title, ASLFR_DrawersOnly, TRUE,
            ASLFR_InitialDrawer, (ULONG)(dir[0] ? dir : "RAM:"), ASLFR_SleepWindow, TRUE, TAG_END))) return FALSE;
    if (AslRequest(fr, NULL)) {
        strncpy(dir, (char *)fr->fr_Drawer, max - 1); dir[max - 1] = 0;
        if (!dir[0]) strcpy(dir, "RAM:");
        ok = TRUE;
    }
    FreeAslRequest(fr);
    return ok;
}

/* ---- settings: ENVARC:NilTerm.prefs ---------------------------------------------------------- */
#define PREFS_FILE "ENVARC:NilTerm.prefs"
static struct {
    char dl[200];                       /* downloads go here (an entry's own folder wins); "" = ask once */
    char ul[200];                       /* the upload requester starts here */
    char cap[200];                      /* capture files go here */
    LONG autozm;                        /* start ZMODEM downloads by themselves */
    char serdev[64];                    /* the modem: serial.device (or a card's driver) */
    LONG serunit;
    LONG serbaud;                       /* Amiga <-> modem speed */
    LONG rtscts;                        /* hardware handshake */
    char init[64];                      /* "" = send nothing first */
    char dial[24];                      /* put before the number: ATDT */
} prefs;

static void prefs_save(void)
{
    BPTR fh;
    char l[600];
    if (!(fh = Open((STRPTR)PREFS_FILE, MODE_NEWFILE))) return;
    FPuts(fh, (STRPTR)"; NilTerm settings (Settings: S in the dialing directory, Alt+S during a call)\n");
    sprintf(l, "download=%s\nupload=%s\ncapture=%s\nautozmodem=%s\n"
               "serial_device=%s\nserial_unit=%ld\nserial_baud=%ld\nserial_rtscts=%s\nmodem_init=%s\nmodem_dial=%s\n",
            prefs.dl, prefs.ul, prefs.cap, prefs.autozm ? "yes" : "no",
            prefs.serdev, (long)prefs.serunit, (long)prefs.serbaud, prefs.rtscts ? "yes" : "no", prefs.init, prefs.dial);
    FPuts(fh, (STRPTR)l);
    Close(fh);
}

static void prefs_load(void)
{
    BPTR fh;
    char l[260];
    strcpy(prefs.ul, "RAM:");
    strcpy(prefs.cap, "RAM:");
    prefs.autozm = TRUE;
    strcpy(prefs.serdev, "serial.device");
    prefs.serbaud = 38400;
    prefs.rtscts = TRUE;
    strcpy(prefs.init, "ATZ");
    strcpy(prefs.dial, "ATDT");
    if ((fh = Open((STRPTR)PREFS_FILE, MODE_OLDFILE))) {
        while (FGets(fh, (STRPTR)l, sizeof(l))) {
            char *e = l + strlen(l);
            while (e > l && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
            if (!strncmp(l, "download=", 9)) strncpy(prefs.dl, l + 9, sizeof(prefs.dl) - 1);
            else if (!strncmp(l, "upload=", 7) && l[7]) strncpy(prefs.ul, l + 7, sizeof(prefs.ul) - 1);
            else if (!strncmp(l, "capture=", 8) && l[8]) strncpy(prefs.cap, l + 8, sizeof(prefs.cap) - 1);
            else if (!strncmp(l, "autozmodem=", 11)) prefs.autozm = (l[11] == 'y' || l[11] == 'Y');
            else if (!strncmp(l, "serial_device=", 14) && l[14]) strncpy(prefs.serdev, l + 14, sizeof(prefs.serdev) - 1);
            else if (!strncmp(l, "serial_unit=", 12)) prefs.serunit = atol(l + 12);
            else if (!strncmp(l, "serial_baud=", 12) && atol(l + 12) > 0) prefs.serbaud = atol(l + 12);
            else if (!strncmp(l, "serial_rtscts=", 14)) prefs.rtscts = (l[14] == 'y' || l[14] == 'Y');
            else if (!strncmp(l, "modem_init=", 11)) strncpy(prefs.init, l + 11, sizeof(prefs.init) - 1);
            else if (!strncmp(l, "modem_dial=", 11)) strncpy(prefs.dial, l + 11, sizeof(prefs.dial) - 1);
        }
        Close(fh);
    } else if (GetVar((STRPTR)"NilTerm/DownloadDir", (STRPTR)prefs.dl, sizeof(prefs.dl), GVF_GLOBAL_ONLY) > 0)
        prefs_save();                   /* the first builds kept the download folder in an ENV variable */
}

/* ---- capture on / off (Alt+L) --------------------------------------------------------------- */
static void capture_toggle(void)
{
    static char path[256] = "RAM:NilTerm.cap";
    struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"NilTerm capture",
        (UBYTE *)"Log this session to a file:\nRaw ANSI keeps the colours (view it in NilTerm or SyncTERM),\n"
                 "Plain text keeps only the words.", (UBYTE *)"Raw ANSI|Plain text|Cancel" };
    LONG r;
    if (cap_fh) {
        cap_flush(); Close(cap_fh); cap_fh = 0;
        status_draw(); flush();
        return;
    }
    r = EasyRequestArgs(win, &es, NULL, NULL);
    if (r == 0) return;
    cap_plain = (r == 2);
    if (!strcmp(path, "RAM:NilTerm.cap") && prefs.cap[0]) path_join(path, prefs.cap, "NilTerm.cap");
    if (!ask_file(cap_plain ? "Capture to a text file" : "Capture to an ANSI file", path, sizeof(path), TRUE)) return;
    if ((cap_fh = Open((STRPTR)path, MODE_READWRITE))) {
        Seek(cap_fh, 0, OFFSET_END);            /* an existing log grows, like SyncTERM's */
        cap_n = 0; cap_esc = 0;
    } else DisplayBeep(scr);
    status_draw(); flush();
}

/* ---- the clipboard: Amiga+C copies the screen, Amiga+V pastes -------------------------------- */
static UBYTE cp2lat[128];               /* CP437 0x80-0xFF -> Latin-1 (the reverse of lat2cp), '?' if none */

static void make_cp2lat(void)
{
    int i;
    for (i = 0; i < 128; i++) cp2lat[i] = '?';
    for (i = 0; i < 96; i++) if (lat2cp[i] >= 0x80) cp2lat[lat2cp[i] - 0x80] = (UBYTE)(0xA0 + i);
    /* box drawing and blocks: the nearest ASCII */
    for (i = 0xB3; i <= 0xDA; i++) cp2lat[i - 0x80] = '+';
    cp2lat[0xB3 - 0x80] = cp2lat[0xBA - 0x80] = '|';
    cp2lat[0xC4 - 0x80] = cp2lat[0xCD - 0x80] = '-';
    for (i = 0xB0; i <= 0xB2; i++) cp2lat[i - 0x80] = '#';
    for (i = 0xDB; i <= 0xDF; i++) cp2lat[i - 0x80] = '#';
}

static void clip_copy(void)
{
    struct IFFHandle *iff;
    static char text[SROWS * (COLS + 1)];
    int y, x, n = 0;
    if (!IFFParseBase) { DisplayBeep(scr); return; }
    for (y = 0; y < ROWS; y++) {
        int e = COLS - 1;
        while (e >= 0 && chr[y][e] == ' ') e--;
        for (x = 0; x <= e; x++) {
            UBYTE c = chr[y][x];
            text[n++] = c >= 0x80 && !amiga_mode ? cp2lat[c - 0x80] : c < 0x20 ? ' ' : c;
        }
        text[n++] = '\n';
    }
    while (n > 1 && text[n - 1] == '\n' && text[n - 2] == '\n') n--;   /* no trail of blank lines */
    if (!(iff = AllocIFF())) return;
    if ((iff->iff_Stream = (ULONG)OpenClipboard(0))) {
        InitIFFasClip(iff);
        if (!OpenIFF(iff, IFFF_WRITE)) {
            if (!PushChunk(iff, MAKE_ID('F','T','X','T'), ID_FORM, IFFSIZE_UNKNOWN)) {
                if (!PushChunk(iff, 0, MAKE_ID('C','H','R','S'), IFFSIZE_UNKNOWN)) {
                    WriteChunkBytes(iff, text, n);
                    PopChunk(iff);
                }
                PopChunk(iff);
            }
            CloseIFF(iff);
        }
        CloseClipboard((struct ClipboardHandle *)iff->iff_Stream);
    }
    FreeIFF(iff);
    DisplayBeep(scr);                   /* "copied" */
}

static void clip_paste(void)
{
    struct IFFHandle *iff;
    if (!IFFParseBase || sock < 0) return;
    if (!(iff = AllocIFF())) return;
    if ((iff->iff_Stream = (ULONG)OpenClipboard(0))) {
        InitIFFasClip(iff);
        if (!OpenIFF(iff, IFFF_READ)) {
            if (!StopChunk(iff, MAKE_ID('F','T','X','T'), MAKE_ID('C','H','R','S'))) {
                while (!ParseIFF(iff, IFFPARSE_SCAN)) {
                    UBYTE b[256];
                    LONG got, i;
                    while ((got = ReadChunkBytes(iff, b, sizeof(b))) > 0)
                        for (i = 0; i < got; i++) {
                            UBYTE c = b[i];
                            if (c == '\n') c = '\r';
                            else if (c >= 0xA0) { if (!amiga_mode) c = lat2cp[c - 0xA0]; }
                            else if (c < 0x20 && c != '\t' && c != '\r') continue;
                            else if (c >= 0x7F && c < 0xA0) continue;
                            ob_data(c);
                        }
                }
            }
            CloseIFF(iff);
        }
        CloseClipboard((struct ClipboardHandle *)iff->iff_Stream);
    }
    FreeIFF(iff);
    ob_flush();
}

/* ---- the dialing directory: ENVARC:NilTerm.lst ------------------------------------------------ */
#define DIR_FILE "ENVARC:NilTerm.lst"
#define DIR_MAX  100
struct DirEnt {
    char name[40];
    char host[64];                      /* host name / address, or a modem's phone number */
    LONG port;                          /* 0 = this Amiga's NilBBS, whatever port it's on */
    LONG calls;
    char last[20];                      /* "28-Sep-26 14:22" */
    char dl[128];                       /* its own download folder, "" = the Settings one */
    LONG type;                          /* CT_TELNET / CT_RAW / CT_RLOGIN / CT_MODEM */
    char user[40], pass[40];            /* the login: auto-login, Alt+L, rlogin */
    LONG autologin;                     /* answer the name / password prompts by ourselves */
    LONG amimode;                        /* Amiga mode: Topaz, Latin-1, 0x9B = CSI */
    LONG rate;                          /* emulated line speed, bps (0 = full speed) */
    LONG ice;                           /* iCE colours from the start */
};
static const char *const type_names[] = { "telnet", "raw", "rlogin", "modem", NULL };
static const LONG rates[] = { 0, 300, 1200, 2400, 9600, 14400, 19200, 28800, 38400, 57600, 115200 };
#define NRATES ((int)(sizeof(rates) / sizeof(rates[0])))
static struct DirEnt *cur_ent;          /* the entry being called (NULL: HOST=/PORT= direct mode) */
static void settings(void);
static struct DirEnt *dir;
static int ndir;

static void dir_save(void)
{
    BPTR fh;
    int i;
    char l[400];
    if (!(fh = Open((STRPTR)DIR_FILE, MODE_NEWFILE))) return;
    FPuts(fh, (STRPTR)"; NilTerm dialing directory - [name], then its settings (passwords are NOT encrypted)\n");
    for (i = 0; i < ndir; i++) {
        struct DirEnt *d = &dir[i];
        sprintf(l, "\n[%s]\nhost=%s\nport=%ld\ncalls=%ld\nlast=%s\ntype=%s\nscreen=%s\nrate=%ld\nice=%s\nautologin=%s\n",
                d->name, d->host, (long)d->port, (long)d->calls, d->last, type_names[d->type & 3],
                d->amimode ? "amiga" : "pc", (long)d->rate, d->ice ? "yes" : "no", d->autologin ? "yes" : "no");
        FPuts(fh, (STRPTR)l);
        if (d->dl[0]) { sprintf(l, "download=%s\n", d->dl); FPuts(fh, (STRPTR)l); }
        if (d->user[0]) { sprintf(l, "user=%s\n", d->user); FPuts(fh, (STRPTR)l); }
        if (d->pass[0]) { sprintf(l, "password=%s\n", d->pass); FPuts(fh, (STRPTR)l); }
    }
    Close(fh);
}

static void dir_load(void)
{
    BPTR fh;
    char l[200];
    ndir = 0;
    if ((fh = Open((STRPTR)DIR_FILE, MODE_OLDFILE))) {
        while (FGets(fh, (STRPTR)l, sizeof(l))) {
            char *e = l + strlen(l);
            while (e > l && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
            if (l[0] == '[' && e > l + 1 && e[-1] == ']' && ndir < DIR_MAX) {
                memset(&dir[ndir], 0, sizeof(dir[0]));
                dir[ndir].autologin = TRUE;
                e[-1] = 0;
                strncpy(dir[ndir].name, l + 1, sizeof(dir[0].name) - 1);
                ndir++;
            } else if (ndir) {
                struct DirEnt *d = &dir[ndir - 1];
                if (!strncmp(l, "password=", 9)) strncpy(d->pass, l + 9, sizeof(d->pass) - 1);  /* spaces kept */
                else {
                    while (e > l && e[-1] == ' ') *--e = 0;
                    if (!strncmp(l, "host=", 5)) strncpy(d->host, l + 5, sizeof(d->host) - 1);
                    else if (!strncmp(l, "port=", 5)) d->port = atol(l + 5);
                    else if (!strncmp(l, "calls=", 6)) d->calls = atol(l + 6);
                    else if (!strncmp(l, "last=", 5)) strncpy(d->last, l + 5, sizeof(d->last) - 1);
                    else if (!strncmp(l, "download=", 9)) strncpy(d->dl, l + 9, sizeof(d->dl) - 1);
                    else if (!strncmp(l, "user=", 5)) strncpy(d->user, l + 5, sizeof(d->user) - 1);
                    else if (!strncmp(l, "screen=", 7)) d->amimode = (l[7] == 'a' || l[7] == 'A');
                    else if (!strncmp(l, "rate=", 5)) d->rate = atol(l + 5);
                    else if (!strncmp(l, "ice=", 4)) d->ice = (l[4] == 'y' || l[4] == 'Y');
                    else if (!strncmp(l, "autologin=", 10)) d->autologin = (l[10] == 'y' || l[10] == 'Y');
                    else if (!strncmp(l, "type=", 5)) {
                        int t;
                        for (t = 0; type_names[t]; t++) if (!str_icmp(l + 5, type_names[t])) d->type = t;
                    }
                }
            }
        }
        Close(fh);
    }
    if (!ndir) {                        /* first run: the BBS on this Amiga */
        memset(&dir[0], 0, sizeof(dir[0]));
        strcpy(dir[0].name, "This Amiga's NilBBS");
        strcpy(dir[0].host, "127.0.0.1");
        dir[0].autologin = TRUE;
        ndir = 1;
        dir_save();
    }
}

static void dir_stamp(struct DirEnt *d)
{
    struct DateTime dt;
    char day[LEN_DATSTRING], tim[LEN_DATSTRING];
    DateStamp(&dt.dat_Stamp);
    dt.dat_Format = FORMAT_DOS; dt.dat_Flags = 0;
    dt.dat_StrDay = NULL; dt.dat_StrDate = (STRPTR)day; dt.dat_StrTime = (STRPTR)tim;
    DateToStr(&dt);
    tim[5] = 0;                         /* hh:mm */
    sprintf(d->last, "%s %s", day, tim);
    d->calls++;
}

static void dir_addr(const struct DirEnt *d, char *addr)
{
    if (d->type == CT_MODEM) {
        if (d->host[0]) sprintf(addr, "modem %.40s", d->host);
        else strcpy(addr, "serial line");
    } else if (!d->port) sprintf(addr, "%.40s (local)", d->host);
    else sprintf(addr, "%s%.40s:%ld", d->type == CT_RLOGIN ? "rlogin " : d->type == CT_RAW ? "raw " : "",
                 d->host, (long)d->port);
}

static void dir_draw(int sel, int first, int shown)
{
    char l[COLS + 8];
    int i, y;
    for (y = 0; y < ROWS; y++) put_at(y, 0, 0x07, "", COLS);
    put_at(0, 0, 0x4F, "", COLS);
    put_at(0, 2, 0x4F, "NilTerm  -  Dialing Directory", 40);
    put_at(0, 59, 0x4B, "Help = keys & about", 20);
    sprintf(l, " %-28s %-26s %5s  %s", "Name", "Address", "Calls", "Last call");
    put_at(2, 0, 0x0E, l, COLS);
    put_at(3, 0, 0x08, " -----------------------------------------------------------------------------", COLS);
    for (i = 0; i < shown; i++) {
        int n = first + i;
        char addr[80];
        if (n >= ndir) break;
        dir_addr(&dir[n], addr);
        sprintf(l, " %-28.28s %-26.26s %5ld  %.15s", dir[n].name, addr, (long)dir[n].calls, dir[n].last);
        put_at(4 + i, 0, n == sel ? 0x70 : 0x07, l, COLS);
    }
    if (!ndir) put_at(4, 1, 0x08, "(empty - A adds a system)", 40);
    put_at(ROWS - 2, 0, 0x08, " -----------------------------------------------------------------------------", COLS);
    put_at(ROWS - 1, 0, 0x0B,
           " Enter call  A add  E edit  D delete  C quick connect  S settings  Q quit", COLS);
    strcpy(st_name, "Dialing directory");
    st_since = 0;
    status_draw();
    cursor_hide();
    curvis = FALSE;
    flush();
}

/* ---- a form: one field per line; the entry editor and the Settings screen ---------------------- */
enum { F_TEXT, F_PASS, F_NUM, F_CHOICE, F_DIR, F_RATE };
struct Field {
    const char *label;
    int type;
    void *p;                            /* char[] (TEXT/PASS/DIR) or LONG (NUM/CHOICE/RATE) */
    int max;                            /* the text's size, or the number's digits */
    const char *const *ch;              /* F_CHOICE: the names, NULL-terminated */
    const char *blank;                  /* shown for an empty text */
    const char *help;
};

static void form_value(const struct Field *f, char *v)
{
    LONG n = (f->type == F_NUM || f->type == F_CHOICE || f->type == F_RATE) ? *(LONG *)f->p : 0;
    switch (f->type) {
    case F_TEXT: case F_DIR:
        strncpy(v, (char *)f->p, 70); v[70] = 0;
        if (!v[0] && f->blank) strcpy(v, f->blank);
        break;
    case F_PASS:                        /* set or not - never its length */
        strcpy(v, ((char *)f->p)[0] ? "********" : f->blank ? f->blank : "");
        break;
    case F_NUM: {
        char t[12];
        int i = 0;
        ULONG u = n < 0 ? -n : n;
        do { t[i++] = '0' + u % 10; u /= 10; } while (u);
        if (n < 0) t[i++] = '-';
        while (i) *v++ = t[--i];
        *v = 0;
        break;
    }
    case F_CHOICE: { int c = 0; while (f->ch[c]) c++; sprintf(v, "< %s >", n >= 0 && n < c ? f->ch[n] : "?"); break; }
    case F_RATE: if (n) sprintf(v, "< %ld bps >", (long)n); else strcpy(v, "< full speed >"); break;
    }
}

static int rate_index(LONG r)
{
    int i;
    for (i = NRATES - 1; i > 0; i--) if (r >= rates[i]) return i;
    return 0;
}

/* returns TRUE to keep the changes: the editor's S (or Yes to "save?"), always for Settings */
static BOOL form(const char *title, struct Field *f, int n, BOOL editor)
{
    const int top = 2;
    int sel = 0, k, i;
    BOOL changed = FALSE;
    char oldmode[sizeof(st_mode)];
    strcpy(oldmode, st_mode);
    cursor_hide();
    curvis = FALSE;
    for (;;) {
        char l[COLS + 8], v[80];
        struct Field *c = &f[sel];
        for (i = 0; i < ROWS; i++) put_at(i, 0, 0x07, "", COLS);
        put_at(0, 0, 0x4F, "", COLS);
        put_at(0, 2, 0x4F, title, 50);
        put_at(0, 56, 0x4B, editor ? "S save   Esc cancel" : "Esc = back (saved)", 23);
        for (i = 0; i < n; i++) {
            form_value(&f[i], v);
            v[54] = 0;                  /* (not "%.54s": libnix prints "%.Ns" of "0" as nothing) */
            sprintf(l, " %-20s %s", f[i].label, v);
            put_at(top + i, 1, i == sel ? 0x70 : 0x07, l, COLS - 2);
        }
        put_at(top + n + 1, 2, 0x0B, c->help ? c->help : "", COLS - 4);
        put_at(ROWS - 2, 0, 0x08, " -----------------------------------------------------------------------------", COLS);
        put_at(ROWS - 1, 0, 0x0B, editor ?
               " Up/Down pick  Enter change  Left/Right choose  Del clear  S save  Esc cancel" :
               " Up/Down pick  Enter change  Left/Right choose  Del clear  Esc back", COLS);
        strcpy(st_mode, title);
        status_draw();
        flush();
        k = ui_key();
        if (k == K_UP) sel = (sel + n - 1) % n;
        else if (k == K_DOWN) sel = (sel + 1) % n;
        else if (k == K_HOME) sel = 0;
        else if (k == K_END) sel = n - 1;
        else if (k == K_LEFT || k == K_RIGHT || k == K_ENTER || k == ' ') {
            int d = k == K_LEFT ? -1 : 1;
            if (c->type == F_CHOICE) {
                int cnt = 0;
                while (c->ch[cnt]) cnt++;
                *(LONG *)c->p = (*(LONG *)c->p + d + cnt) % cnt;
                changed = TRUE;
            } else if (c->type == F_RATE) {
                *(LONG *)c->p = rates[(rate_index(*(LONG *)c->p) + d + NRATES) % NRATES];
                changed = TRUE;
            } else if (k == K_ENTER || k == ' ') {
                char prompt[24];
                sprintf(prompt, "%-20s", c->label);
                if (c->type == F_DIR && AslBase) {
                    if (ask_drawer(c->label, (char *)c->p, c->max)) changed = TRUE;
                } else if (c->type == F_NUM) {
                    char num[12];
                    sprintf(num, "%ld", (long)*(LONG *)c->p);
                    if (edit_line(top + sel, prompt, num, c->max)) { *(LONG *)c->p = atol(num); changed = TRUE; if (sel < n - 1) sel++; }
                } else if (edit_line_x(top + sel, prompt, (char *)c->p, c->max - 1, c->type == F_PASS)) {
                    changed = TRUE;
                    if (sel < n - 1) sel++;
                }
                curvis = FALSE;
            }
        } else if (k == K_DEL && (c->type == F_TEXT || c->type == F_PASS || c->type == F_DIR)) {
            ((char *)c->p)[0] = 0;
            changed = TRUE;
        } else if (editor && (k == 's' || k == 'S')) { strcpy(st_mode, oldmode); return TRUE; }
        else if (k == K_ESC || k == K_QUIT || (!editor && (k == 'q' || k == 'Q'))) {
            char yn[4] = "y";
            if (!editor) { strcpy(st_mode, oldmode); return changed; }
            if (!changed) { strcpy(st_mode, oldmode); return FALSE; }
            if (!edit_line(ROWS - 3, "Save the changes? (Y/n)", yn, 1)) continue;   /* Esc: keep editing */
            strcpy(st_mode, oldmode);
            return yn[0] != 'n' && yn[0] != 'N';
        }
    }
}

static BOOL dir_edit(struct DirEnt *d)
{
    static const char *const onoff[] = { "off - Alt+L sends them", "on - answers the prompts", NULL };
    static const char *const scrn[] = { "PC (CP437, the VGA font)", "Amiga (Latin-1, Topaz)", NULL };
    static const char *const icec[] = { "when the BBS asks", "always", NULL };
    struct DirEnt e = *d;
    struct Field f[] = {
        { "Name", F_TEXT, e.name, sizeof(e.name), NULL, NULL, "What the directory calls it." },
        { "Connection", F_CHOICE, &e.type, 0, type_names, NULL,
          "telnet for most BBSes; raw TCP; rlogin; or a modem (set it up in Settings)." },
        { "Address / number", F_TEXT, e.host, sizeof(e.host), NULL, "(direct serial line)",
          "Host name or address.  Modem: the number to dial (blank = a direct line)." },
        { "Port", F_NUM, &e.port, 5, NULL, NULL,
          "Telnet 23, rlogin 513.  0 = this Amiga's own NilBBS.  (Not used by a modem.)" },
        { "User name", F_TEXT, e.user, sizeof(e.user), NULL, "(none)",
          "Sent when the BBS asks for your name / handle (and in the rlogin handshake)." },
        { "Password", F_PASS, e.pass, sizeof(e.pass), NULL, "(none)",
          "Sent at the password prompt.  Kept in ENVARC:NilTerm.lst - NOT encrypted." },
        { "Auto-login", F_CHOICE, &e.autologin, 0, onoff, NULL,
          "On: answer the name and password prompts by itself.  Off: press Alt+L." },
        { "Screen", F_CHOICE, &e.amimode, 0, scrn, NULL,
          "PC for most BBSes; Amiga for BBSes that send Amiga ANSI (Topaz, 0x9B)." },
        { "Line speed", F_RATE, &e.rate, 0, NULL, NULL,
          "Show text at a modem's pace, for ANSI animations.  Alt+Up/Down in a call." },
        { "iCE colours", F_CHOICE, &e.ice, 0, icec, NULL,
          "Always: blink attribute = bright background (for iCE art), from the start." },
        { "Download folder", F_DIR, e.dl, sizeof(e.dl), NULL, "(the Settings one)",
          "This system's downloads go here.  Enter picks a drawer, Del clears it." },
    };
    LONG oldtype = e.type;
    if (!form(d->name[0] ? "NilTerm  -  Edit a system" : "NilTerm  -  Add a system", f, sizeof(f) / sizeof(f[0]), TRUE))
        return FALSE;
    if (!e.host[0] && e.type != CT_MODEM) { DisplayBeep(scr); return FALSE; }
    if (!e.name[0]) strncpy(e.name, e.host[0] ? e.host : "Serial line", sizeof(e.name) - 1);
    if (e.type != oldtype) {            /* the usual port for the new kind, unless one was set by hand */
        if (e.type == CT_RLOGIN && (e.port == 23 || !e.port)) e.port = 513;
        else if (e.type == CT_TELNET && e.port == 513) e.port = 23;
    }
    *d = e;
    return TRUE;
}

/* the directory: returns the entry to call (its host/port are set up), -1 = quit */
static int directory(void)
{
    static int sel;
    int first = 0, shown = ROWS - 7, k, i;
    char q[80];
    for (;;) {
        if (sel >= ndir) sel = ndir - 1;
        if (sel < 0) sel = 0;
        if (sel < first) first = sel;
        if (sel >= first + shown) first = sel - shown + 1;
        dir_draw(sel, first, shown);
        k = ui_key();
        switch (k) {
        case K_UP: sel--; break;
        case K_DOWN: sel++; break;
        case K_PGUP: sel -= shown; break;
        case K_PGDN: sel += shown; break;
        case K_HOME: sel = 0; break;
        case K_END: sel = ndir - 1; break;
        case K_HELP: about(); break;
        case 's': case 'S': settings(); break;
        case K_QUIT: case K_ESC: case 'q': case 'Q': return -1;
        case K_ENTER:
            if (ndir) return sel;
            break;
        case 'a': case 'A':
            if (ndir < DIR_MAX) {
                struct DirEnt d;
                memset(&d, 0, sizeof(d));
                d.port = 23;
                d.autologin = TRUE;
                if (dir_edit(&d)) { dir[ndir] = d; sel = ndir++; dir_save(); }
            }
            break;
        case 'e': case 'E':
            if (ndir && dir_edit(&dir[sel])) dir_save();
            break;
        case 'd': case 'D': case K_DEL:
            if (ndir) {
                char yn[4] = "";
                sprintf(q, "Delete \"%.40s\"? (y/N)", dir[sel].name);
                if (edit_line(ROWS - 3, q, yn, 1) && (yn[0] == 'y' || yn[0] == 'Y')) {
                    for (i = sel; i + 1 < ndir; i++) dir[i] = dir[i + 1];
                    ndir--;
                    dir_save();
                }
            }
            break;
        case 'c': case 'C': {
            char hp[80] = "";
            if (edit_line(ROWS - 3, "Connect to host[:port]:", hp, 70) && hp[0] && ndir < DIR_MAX) {
                char *c = strrchr(hp, ':');
                struct DirEnt d;
                memset(&d, 0, sizeof(d));
                d.port = 23;
                d.autologin = TRUE;
                if (c) { *c = 0; d.port = atol(c + 1); }
                strncpy(d.host, hp, sizeof(d.host) - 1);
                strncpy(d.name, hp, sizeof(d.name) - 1);
                /* a quick connect is remembered, like SyncTERM does - delete it if not wanted */
                dir[ndir] = d; sel = ndir++;
                dir_save();
                return sel;
            }
            break;
        }
        }
    }
}

/* ---- the Settings screen (S in the directory, Alt+S during a call) ------------------------------ */
static void settings(void)
{
    static UBYTE sc[SROWS][COLS], sa[SROWS][COLS], sk[SROWS][COLS];   /* what was on screen, put back afterwards */
    static const char *const zm[] = { "Alt+R only", "start by themselves", NULL };
    static const char *const yn[] = { "off", "on (RTS/CTS)", NULL };
    struct Field f[] = {
        { "Download folder", F_DIR, prefs.dl, sizeof(prefs.dl), NULL, "(ask the first time)",
          "Where downloads are saved - a directory entry's own folder wins over this one." },
        { "Upload folder", F_DIR, prefs.ul, sizeof(prefs.ul), NULL, NULL, "Where the upload file requester opens." },
        { "Capture folder", F_DIR, prefs.cap, sizeof(prefs.cap), NULL, NULL, "Where Alt+C capture files go." },
        { "ZMODEM downloads", F_CHOICE, &prefs.autozm, 0, zm, NULL,
          "Start by themselves when the BBS sends one, or only with Alt+R." },
        { "Serial device", F_TEXT, prefs.serdev, sizeof(prefs.serdev), NULL, NULL,
          "For modem entries: serial.device, or your serial card's driver." },
        { "Serial unit", F_NUM, &prefs.serunit, 3, NULL, NULL, "The device's unit number (0 for the built-in port)." },
        { "Serial speed", F_NUM, &prefs.serbaud, 6, NULL, NULL,
          "Amiga <-> modem speed (bps): 19200, 38400, 57600, 115200..." },
        { "Hardware handshake", F_CHOICE, &prefs.rtscts, 0, yn, NULL, "RTS/CTS flow control - leave on for a modem." },
        { "Modem init", F_TEXT, prefs.init, sizeof(prefs.init), NULL, "(none)",
          "Sent before dialling, e.g. ATZ or AT&F1.  Blank = nothing." },
        { "Dial command", F_TEXT, prefs.dial, sizeof(prefs.dial), NULL, NULL,
          "Put before the number: ATDT (tone) or ATDP (pulse)." },
    };
    int i, oldx = cx, oldy = cy;
    BOOL oldvis = curvis;
    memcpy(sc, chr, sizeof(sc)); memcpy(sa, att, sizeof(sa)); memcpy(sk, blk, sizeof(sk));
    if (form("NilTerm  -  Settings", f, sizeof(f) / sizeof(f[0]), FALSE)) prefs_save();
    if (!prefs.ul[0]) strcpy(prefs.ul, "RAM:");
    if (!prefs.cap[0]) strcpy(prefs.cap, "RAM:");
    memcpy(chr, sc, sizeof(sc)); memcpy(att, sa, sizeof(sa)); memcpy(blk, sk, sizeof(sk));
    for (i = 0; i < SROWS; i++) mark(i, 0, COLS - 1);
    cx = oldx; cy = oldy; curvis = oldvis;
    status_draw();
    flush();
}

/* ---- file transfers: NilBBS's own ZMODEM / X/YMODEM (src/node, via ntzm.c / ntxy.c) -----------
 * They talk to "the node": here that's this connection.  While one runs, data_byte() puts the
 * incoming bytes in the ring below instead of on the screen; tn_wait() reads the socket,
 * shows progress on the status bar and turns Esc into a cancel (N.online = FALSE). */
#define ZIN_SIZE 16384                  /* a power of two */
struct NodeCtx N;
static UBYTE zin[ZIN_SIZE];
static struct ZStats *xst;              /* the running transfer's counters, for the status bar */
static const char *xwhat;
static ULONG xshown;

LONG in_avail(void) { return (LONG)((N.in_head - N.in_tail) & (ZIN_SIZE - 1)); }

static void zin_put(UBYTE c)
{
    UWORD next = (N.in_head + 1) & (ZIN_SIZE - 1);
    if (next == N.in_tail) return;      /* can't happen: tn_wait() only reads what fits */
    zin[N.in_head] = c;
    N.in_head = next;
}

LONG in_get(void)
{
    UBYTE c;
    if (N.in_head == N.in_tail) return -1;
    c = zin[N.in_tail];
    N.in_tail = (N.in_tail + 1) & (ZIN_SIZE - 1);
    return c;
}

void in_unget(UBYTE c)
{
    UWORD prev = (N.in_tail - 1) & (ZIN_SIZE - 1);
    if (prev == N.in_head) return;
    N.in_tail = prev;
    zin[prev] = c;
}

void tn_raw(const UBYTE *buf, LONG len) { while (len-- > 0) ob_data(*buf++); }
BOOL tn_flush(void) { ob_flush(); return !closed; }
void tn_rawflush(const UBYTE *buf, LONG len) { tn_raw(buf, len); ob_flush(); }
void tn_set_binary(BOOL on) { (void)on; }      /* the BBS asks for BINARY; tn_option() agrees */

static void xfer_status(BOOL force)
{
    ULONG s = now_secs();
    if (!xst || (!force && s == xshown)) return;
    xshown = s;
    sprintf(st_mode, "%s: file %ld, %lu KB  (Esc cancels)", xwhat, (long)(xst->file ? xst->file : 1),
            (unsigned long)((xst->total + xst->bytes) / 1024));
    cursor_hide();
    status_draw();
    flush();
}

LONG tn_wait(ULONG ms, ULONG extrasigs, ULONG *gotsigs)
{
    static UBYTE buf[4096];
    ULONG winsig = 1UL << win->UserPort->mp_SigBit, sigs;
    LONG n, room, i;
    (void)extrasigs;
    if (gotsigs) *gotsigs = 0;
    ob_flush();
    if (in_avail()) return in_avail();
    if (closed || (sock < 0 && !ser_open_ok)) { N.online = FALSE; return 0; }
    xfer_status(FALSE);
    room = ZIN_SIZE - 1 - in_avail();
    if (room > (LONG)sizeof(buf)) room = sizeof(buf);
    n = conn_wait(buf, room, ms ? ms : 1, winsig | SIGBREAKF_CTRL_C, &sigs);
    if (sigs & SIGBREAKF_CTRL_C) N.online = FALSE;
    if (sigs & winsig) {                /* Esc: cancel; other keys: ignored */
        struct IntuiMessage *m;
        while ((m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
            if (m->Class == IDCMP_RAWKEY && m->Code == 0x45) N.online = FALSE;
            if (m->Class == IDCMP_REFRESHWINDOW) { BeginRefresh(win); EndRefresh(win, TRUE); }
            ReplyMsg((struct Message *)m);
        }
    }
    if (n < 0) { closed = TRUE; N.online = FALSE; }
    else for (i = 0; i < n; i++) rx_byte(buf[i]);
    return in_avail();
}

/* the download drawer: ENV:NilTerm/DownloadDir, asked for (and saved) the first time */
static BOOL dl_dir(char *dir, int max)
{
    if (cur_ent && cur_ent->dl[0]) { strncpy(dir, cur_ent->dl, max - 1); dir[max - 1] = 0; return TRUE; }
    if (prefs.dl[0]) { strncpy(dir, prefs.dl, max - 1); dir[max - 1] = 0; return TRUE; }
    if (!AslBase) { strcpy(dir, "RAM:"); return TRUE; }
    dir[0] = 0;
    if (!ask_drawer("Where should downloads go?", dir, max)) return FALSE;
    strncpy(prefs.dl, dir, sizeof(prefs.dl) - 1);
    prefs_save();                       /* change it later in Settings */
    return TRUE;
}

static void xfer_begin(const char *what, struct ZStats *st)
{
    memset(st, 0, sizeof(*st));
    N.online = TRUE;
    N.in_head = N.in_tail = 0;
    xst = st; xwhat = what; xshown = 0;
    xfer = TRUE;
    xfer_status(TRUE);
}

static void xfer_end(LONG files, const char *verb, const char *where)
{
    char msg[160];
    xfer = FALSE;
    xst = NULL;
    N.in_head = N.in_tail = 0;
    if (closed) N.online = FALSE;
    if (files > 0) sprintf(msg, "%s: %ld file%s %s %s", xwhat, (long)files, files == 1 ? "" : "s", verb, where);
    else sprintf(msg, "%s: %s (%s)", xwhat, files == 0 ? "nothing transferred" : "cancelled / no answer", where);
    strncpy(st_mode, msg, sizeof(st_mode) - 1);
    cursor_hide();
    status_draw();
    flush();
    Delay(files > 0 ? 150 : 250);       /* read it (a failure a little longer), then back to the clock */
    st_mode[0] = 0;
    status_draw();
    flush();
}

/* ZMODEM download: started by the BBS ("**" ZDLE "B00" = ZRQINIT), zm_watch() spots it */
static BOOL zm_go;
static int zm_seen;
static void zm_watch(UBYTE c)
{
    static const UBYTE zrqinit[] = { '*', '*', 0x18, 'B', '0', '0' };
    if (xfer || !prefs.autozm) return;
    if (c == zrqinit[zm_seen]) { if (++zm_seen == (int)sizeof(zrqinit)) { zm_seen = 0; zm_go = TRUE; xfer = TRUE; } }
    else zm_seen = (c == '*') ? 1 : 0;
}

static void zm_download(void)
{
    static char names[16][32];
    struct ZStats st;
    char dir[200];
    LONG n;
    zm_go = FALSE;
    if (!dl_dir(dir, sizeof(dir))) {    /* no drawer: tell the sender to stop */
        static const UBYTE can[] = { 24,24,24,24,24,24,24,24,8,8,8,8,8,8,8,8 };
        xfer = FALSE;
        tn_rawflush(can, sizeof(can));
        return;
    }
    xfer_begin("ZMODEM download", &st);
    n = zm_receive(dir, names, 16, &st);
    xfer_end(n, "saved in", dir);
}

/* Alt+U: send a file - ZMODEM, YMODEM or XMODEM (start the BBS's upload first) */
static void upload(void)
{
    static char path[256] = "RAM:";
    struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"NilTerm upload",
        (UBYTE *)"Send a file with which protocol?\n(Start the upload on the BBS first.)",
        (UBYTE *)"ZMODEM|YMODEM|XMODEM-1K|XMODEM|Cancel" };
    struct ZStats st;
    const char *paths[1], *names[1];
    LONG r, n;
    int proto;
    r = EasyRequestArgs(win, &es, NULL, NULL);
    if (r == 0) return;
    proto = r == 1 ? PROTO_Z : r == 2 ? PROTO_Y : r == 3 ? PROTO_X1K : PROTO_X;
    if (!strcmp(path, "RAM:") && prefs.ul[0]) strcpy(path, prefs.ul);
    if (!ask_file("Upload which file?", path, sizeof(path), FALSE)) return;
    paths[0] = path;
    names[0] = (const char *)FilePart((STRPTR)path);
    xfer_begin(proto == PROTO_Z ? "ZMODEM upload" : proto_name(proto), &st);
    n = proto == PROTO_Z ? zm_send(paths, names, 1, &st) : xy_send(paths, names, 1, proto, &st);
    xfer_end(n, "sent from", path);
}

/* Alt+R: receive with YMODEM or XMODEM (ZMODEM downloads start by themselves) */
static void receive_xy(void)
{
    static char names[16][32];
    static char xpath[256];
    struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"NilTerm download",
        (UBYTE *)"Receive with which protocol?\n(ZMODEM downloads start by themselves unless Settings says not.)",
        (UBYTE *)"ZMODEM|YMODEM|XMODEM-1K|XMODEM|Cancel" };
    struct ZStats st;
    char dir[200];
    LONG r, n;
    int proto;
    r = EasyRequestArgs(win, &es, NULL, NULL);
    if (r == 0) return;
    if (r == 1) { xfer = TRUE; zm_download(); return; }
    proto = r == 2 ? PROTO_Y : r == 3 ? PROTO_X1K : PROTO_X;
    if (!dl_dir(dir, sizeof(dir))) return;
    if (proto != PROTO_Y) {             /* XMODEM carries no file name: ask for one */
        if (!xpath[0]) path_join(xpath, dir, "download");
        if (!ask_file("Save the XMODEM download as", xpath, sizeof(xpath), TRUE)) return;
        strncpy(dir, xpath, sizeof(dir) - 1); dir[sizeof(dir) - 1] = 0;
        *PathPart((STRPTR)dir) = 0;
        str_copy(names[0], (const char *)FilePart((STRPTR)xpath), 32);
    }
    xfer_begin(proto_name(proto), &st);
    n = xy_receive(dir, names, 16, proto, &st);
    xfer_end(n, "saved in", dir);
}

/* ---- a modem: init, dial, wait for CONNECT (what it says is shown as it comes) ----------------- */
#define M_FAIL 0
#define M_CONNECT 1
#define M_OK 2
#define M_ABORT 3
static int modem_result(int secs)
{
    ULONG winsig = 1UL << win->UserPort->mp_SigBit, until = now_secs() + secs;
    char line[80];
    int ln = 0;
    while (now_secs() < until) {
        UBYTE b[64];
        ULONG sigs;
        LONG n = conn_wait(b, sizeof(b), 200, winsig | SIGBREAKF_CTRL_C, &sigs), i;
        if (sigs & SIGBREAKF_CTRL_C) { quit = TRUE; return M_ABORT; }
        if (sigs & winsig) {
            struct IntuiMessage *m;
            BOOL esc = FALSE;
            while ((m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
                if (m->Class == IDCMP_RAWKEY && m->Code == 0x45) esc = TRUE;
                ReplyMsg((struct Message *)m);
            }
            if (esc) return M_ABORT;
        }
        if (n < 0) return M_FAIL;
        for (i = 0; i < n; i++) {
            UBYTE c = b[i];
            ansi_byte(c);
            if (c != '\r' && c != '\n') { if (ln < (int)sizeof(line) - 1) line[ln++] = c; continue; }
            line[ln] = 0;
            ln = 0;
            if (!strncmp(line, "CONNECT", 7)) { flush(); return M_CONNECT; }
            if (!strcmp(line, "OK")) { flush(); return M_OK; }
            if (!strncmp(line, "NO CARRIER", 10) || !strncmp(line, "BUSY", 4) || !strncmp(line, "NO DIAL", 7) ||
                !strncmp(line, "NO ANSWER", 9) || !strncmp(line, "ERROR", 5)) { flush(); return M_FAIL; }
        }
        flush();
    }
    return M_FAIL;
}

static void modem_cmd(const char *s) { ser_write((const UBYTE *)s, strlen(s)); ser_write((const UBYTE *)"\r", 1); }

static BOOL modem_connect(void)
{
    char msg[200];
    int r;
    if (!ser_open(prefs.serdev, prefs.serunit, prefs.serbaud, prefs.rtscts)) {
        sprintf(msg, "\x1b[1;31mNilTerm: can't open %s unit %ld (Settings: the serial device)\x1b[0m\r\n",
                prefs.serdev, (long)prefs.serunit);
        term_str(msg);
        return FALSE;
    }
    if (!host_name[0]) {
        term_str("\x1b[0;36mDirect serial line - you're on.\x1b[0m\r\n");
        return TRUE;
    }
    if (prefs.init[0]) {
        modem_cmd(prefs.init);
        if (!ser_stuck && modem_result(5) == M_ABORT) { ser_close(); return FALSE; }
    }
    if (!ser_stuck) {
        sprintf(msg, "%s%s", prefs.dial, host_name);
        modem_cmd(msg);
    }
    if (ser_stuck) {
        term_str("\x1b[1;31mNilTerm: the serial port won't send - the modem isn't giving CTS.\r\n"
                 "Is it on and connected?  (A cable without handshake lines: Settings, Hardware handshake off.)\x1b[0m\r\n");
        closed = FALSE;
        ser_close();
        return FALSE;
    }
    r = modem_result(90);
    if (r == M_CONNECT) return TRUE;
    if (r == M_ABORT) { modem_cmd(""); Delay(25); term_str("\r\n\x1b[1;31mDialling cancelled\x1b[0m\r\n"); }
    else term_str("\r\n\x1b[1;31mNo connection\x1b[0m\r\n");
    ser_close();
    return FALSE;
}

static void modem_hangup(void)
{
    if (!ser_open_ok) return;
    if (host_name[0] && !closed) {      /* dialled and still on: back to command mode, hang up */
        Delay(60); ser_write((const UBYTE *)"+++", 3); Delay(60); modem_cmd("ATH0"); Delay(25);
    }
    ser_close();                        /* DTR drops too */
}

/* ---- auto-login: the entry's name at a name prompt, its password at a password prompt --------- */
static void login_send(const char *s) { while (*s) ob_data((UBYTE)*s++); ob_put('\r'); ob_flush(); }

static void login_check(void)
{
    char t[80];
    int n = lg_n;
    if (!lg_auto || lg_stage >= 2 || (!lg_user[0] && !lg_pass[0])) return;
    if (now_secs() > lg_until) { lg_stage = 2; return; }
    memcpy(t, lg_line, n); t[n] = 0;
    while (n && t[n - 1] == ' ') t[--n] = 0;
    if (!n || (t[n - 1] != ':' && t[n - 1] != '?' && t[n - 1] != '>')) return;
    if (strstr(t, "password") || strstr(t, "passwd")) {
        if (lg_pass[0]) { login_send(lg_pass); lg_stage = 2; lg_n = 0; }
        return;
    }
    if (lg_stage == 0 && lg_user[0] &&
        (strstr(t, "name") || strstr(t, "handle") || strstr(t, "login") || strstr(t, "user") || strstr(t, "alias"))) {
        login_send(lg_user);
        lg_stage = lg_pass[0] ? 1 : 2;
        lg_n = 0;
    }
}

static void login_hotkey(void)          /* Alt+L */
{
    if (!lg_user[0] && !lg_pass[0]) { DisplayBeep(scr); return; }
    if (lg_stage == 0 && lg_user[0]) login_send(lg_user);
    if (lg_pass[0]) login_send(lg_pass);
    lg_stage = 2;
}

/* ---- the speed emulation: what arrives waits here, and comes out at rate/10 characters a second -- */
#define RQ_SIZE 8192
static UBYTE rq[RQ_SIZE];
static int rq_head, rq_tail, rq_n;
static ULONG rq_last;

static void rq_add(const UBYTE *p, LONG n)
{
    while (n-- > 0 && rq_n < RQ_SIZE) { rq[rq_head] = *p++; rq_head = (rq_head + 1) % RQ_SIZE; rq_n++; }
}
static UBYTE rq_get(void) { UBYTE c = rq[rq_tail]; rq_tail = (rq_tail + 1) % RQ_SIZE; rq_n--; return c; }

static void after_input(void)
{
    ob_flush();         /* telnet answers + cursor reports, in order */
    flush();
    login_check();
    if (zm_go) zm_download();
}

/* release n queued bytes (all of them if a download starts: the rest is its) */
static void rq_release(LONG n)
{
    cursor_hide();
    beeped = FALSE;
    while (n-- > 0 && rq_n) {
        rx_byte(rq_get());
        if (xfer) { while (rq_n) rx_byte(rq_get()); break; }
    }
    after_input();
}

static void blink_tick(void)
{
    int y, x;
    BOOL any = FALSE;
    for (y = 0; y < ROWS; y++)
        for (x = 0; x < COLS; x++)
            if (blk[y][x]) { any = TRUE; mark(y, x, x); }
    if (!any) { blink_off = FALSE; return; }
    blink_off = !blink_off;
    cursor_hide();
    flush();
}

static ULONG st_clear_at;               /* a passing status message goes at this time (seconds) */
static void rate_step(int d)
{
    while (d) {
        int i = rate_index(rate);
        if (d > 0) { i = rate ? i + 1 : 0; if (i >= NRATES) i = 0; d--; }   /* past the fastest: full */
        else { i = rate ? i - 1 : NRATES - 1; if (i < 1) i = 1; d++; }      /* slowest is 300 */
        rate = rates[i];
    }
    if (!rate && rq_n) rq_release(rq_n);
    rq_last = now_us();
    if (rate) sprintf(st_mode, "line speed %ld bps  (Alt+Up / Alt+Down)", (long)rate);
    else strcpy(st_mode, "line speed: full  (Alt+Up / Alt+Down)");
    st_clear_at = now_secs() + 2;
    cursor_hide(); status_draw(); flush();
}

/* ---- one call: connect, run the terminal until it ends ------------------------------------------ */
enum { END_CLOSED, END_HANGUP, END_DIR, END_QUIT };
static BOOL direct_mode;                /* HOST=/PORT= given: one call, no directory (BBSControl's Logon) */

static int session(void)
{
    static UBYTE buf[4096];
    ULONG shown_s = 0, blink_t, winsig = 1UL << win->UserPort->mp_SigBit;
    int why = END_CLOSED, y;
    char msg[200];
    BOOL ok;

    memset(us, 0, sizeof(us)); memset(them, 0, sizeof(them));
    tstate = T_DATA; pstate = S_NORM; closed = FALSE; obn = 0; action = ACT_NONE;
    ctype = cur_ent ? (int)cur_ent->type & 3 : CT_TELNET;
    amiga_mode = cur_ent && cur_ent->amimode;
    ice_force = cur_ent && cur_ent->ice;
    rate = cur_ent ? cur_ent->rate : 0;
    lg_user[0] = lg_pass[0] = 0;
    if (cur_ent) {
        strncpy(lg_user, cur_ent->user, sizeof(lg_user) - 1);
        strncpy(lg_pass, cur_ent->pass, sizeof(lg_pass) - 1);
    }
    lg_auto = cur_ent && cur_ent->autologin;
    lg_stage = 0; lg_n = 0; lg_esc = 0;
    rl_first = FALSE;
    rq_n = rq_head = rq_tail = 0;
    blink_off = FALSE;
    if (ctype != CT_TELNET) them[O_ECHO] = 1;       /* no telnet talk: the other end echoes */
    font_glyphs(amiga_mode);
    for (y = 0; y < SROWS; y++) mark(y, 0, COLS - 1);
    cursor_hide();
    reset_term();
    curvis = TRUE;
    st_since = 0; st_mode[0] = 0;
    if (ctype == CT_MODEM)
        sprintf(msg, "\x1b[0;36mNilTerm - %s%s on %s unit %ld...\x1b[0m\r\n", host_name[0] ? "dialling " : "opening the line",
                host_name, prefs.serdev, (long)prefs.serunit);
    else
        sprintf(msg, "\x1b[0;36mNilTerm - connecting to %s port %ld%s...\x1b[0m\r\n", host_name, (long)host_port,
                ctype == CT_RLOGIN ? " (rlogin)" : ctype == CT_RAW ? " (raw)" : "");
    term_str(msg);
    status_draw();
    flush();
    ok = ctype == CT_MODEM ? modem_connect() : connect_host();
    if (!ok) {
        status_draw();
        if (!quit) wait_any_key(10);
        if (amiga_mode) { amiga_mode = FALSE; font_glyphs(FALSE); }
        ctype = CT_TELNET;
        return quit ? END_QUIT : END_CLOSED;
    }
    st_since = now_secs();
    lg_until = st_since + 120;
    rq_last = blink_t = now_us();

    while (!quit && (!closed || rq_n)) {
        ULONG sigs = 0, s, now;
        LONG got = 0, room = sizeof(buf);
        ULONG ms = (rate && rq_n) ? 20 : 250;
        if (rate && room > RQ_SIZE - rq_n) room = RQ_SIZE - rq_n;
        if (closed || room <= 0) sigs = sig_wait(ms, winsig | SIGBREAKF_CTRL_C);
        else got = conn_wait(buf, room, ms, winsig | SIGBREAKF_CTRL_C, &sigs);
        if (got < 0) { closed = TRUE; got = 0; }
        if (sigs & SIGBREAKF_CTRL_C) { quit = TRUE; break; }
        if (sigs & winsig) handle_idcmp();
        if (got > 0) {
            if (rate) rq_add(buf, got);
            else {
                /* a burst: everything that's waiting (up to 32 KB) into the cells, then one redraw */
                LONG total = 0, i;
                cursor_hide();
                beeped = FALSE;
                for (;;) {
                    for (i = 0; i < got; i++) rx_byte(buf[i]);
                    total += got;
                    if (total >= 32768 || xfer || closed) break;     /* a download starting: the rest is its */
                    got = conn_wait(buf, sizeof(buf), 0, 0, &s);
                    if (got <= 0) { if (got < 0) closed = TRUE; break; }
                }
                after_input();
            }
        }
        now = now_us();
        if (rate && rq_n) {
            LONG cps = rate / 10, allow = (LONG)((now - rq_last) / 1000) * cps / 1000;
            if (allow > 0) { rq_last += (ULONG)allow * (1000000UL / cps); rq_release(allow); }
        } else rq_last = now;
        if (now - blink_t >= 500000) { blink_t = now; blink_tick(); }
        if ((s = now_secs()) != shown_s) { shown_s = s; cursor_hide(); status_draw(); flush(); }
        if (action) {
            int a = action;
            action = ACT_NONE;
            if (a == ACT_HANGUP) { why = END_HANGUP; break; }
            if (a == ACT_DIR) { why = END_DIR; break; }
            if (a == ACT_BACK) scrollback();
            else if (a == ACT_LOG) capture_toggle();
            else if (a == ACT_COPY) clip_copy();
            else if (a == ACT_PASTE) clip_paste();
            else if (a == ACT_UPLOAD) upload();
            else if (a == ACT_RECV) receive_xy();
            else if (a == ACT_SET) settings();
            else if (a == ACT_LOGIN) login_hotkey();
        }
        if (rate_steps) { int d = rate_steps; rate_steps = 0; rate_step(d); }
        if (st_clear_at && now_secs() >= st_clear_at && !xfer) {
            st_clear_at = 0; st_mode[0] = 0; cursor_hide(); status_draw(); flush();
        }
    }
    if (quit) why = END_QUIT;
    if (ctype == CT_MODEM) modem_hangup();
    if (sock >= 0) { CloseSocket(sock); sock = -1; }
    if (cap_fh) { cap_flush(); Close(cap_fh); cap_fh = 0; }
    st_since = 0;
    cursor_hide();
    if (closed) {
        term_str("\r\n\x1b[0;36m-- disconnected --\x1b[0m");
        status_draw();
        flush();
        Delay(60);              /* the goodbye screen, a moment */
    }
    if (amiga_mode) { amiga_mode = FALSE; font_glyphs(FALSE); for (y = 0; y < SROWS; y++) mark(y, 0, COLS - 1); }
    blink_off = FALSE;
    ctype = CT_TELNET;
    rate = 0;
    return why;
}


static int real_main(void)
{
    int rc = RETURN_FAIL, y;
    char msg[160];

    if (!(IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39)) ||
        !(GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 39)) ||
        !(KeymapBase = OpenLibrary((STRPTR)"keymap.library", 37))) {
        say("NilTerm: needs AmigaOS 3.0 or newer\n");
        goto out;
    }
    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);   /* optional: a modem needs none */
    if ((tm_port = CreateMsgPort()) &&
        (tm_io = (struct timerequest *)CreateIORequest(tm_port, sizeof(struct timerequest)))) {
        if (OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)tm_io, 0)) {
            DeleteIORequest((struct IORequest *)tm_io); tm_io = NULL;
        } else TimerBase = tm_io->tr_node.io_Device;
    }
    AslBase = OpenLibrary((STRPTR)"asl.library", 38);
    IFFParseBase = OpenLibrary((STRPTR)"iffparse.library", 39);
    if (!open_screen(arg_modeid, arg_native, arg_small)) {
        say("NilTerm: can't open a 640x400 or 640x200 16-colour screen\n");
        goto out;
    }
    if (from_cli) {
        sprintf(msg, "NilTerm: screen mode 0x%08lx (%s), 640x%d, %s glyphs\n",
                modeid, modename, SROWS * CH, CH == 16 ? "8x16" : "8x8 half-height");
        say(msg);
    }
    if (!make_font()) { say("NilTerm: out of chip memory\n"); goto out; }
    if (!(win = OpenWindowTags(NULL,
            WA_CustomScreen, (ULONG)scr,
            WA_Left, 0, WA_Top, 0, WA_Width, scr->Width, WA_Height, scr->Height,
            WA_Backdrop, TRUE, WA_Borderless, TRUE, WA_Activate, TRUE, WA_RMBTrap, TRUE,
            WA_SmartRefresh, TRUE,
            WA_IDCMP, IDCMP_RAWKEY | IDCMP_REFRESHWINDOW,
            TAG_END))) {
        say("NilTerm: can't open its window\n");
        goto out;
    }
    rp = win->RPort;
    SetFont(rp, &vfont);
    SetRast(rp, 0);
    for (y = 0; y < SROWS; y++) { dmin[y] = COLS; dmax[y] = -1; }
    make_cp2lat();
    prefs_load();
    ScreenToFront(scr);

    if (watch_node) {                   /* the caller's own 80x25, no status bar */
        ROWS = SROWS; statusbar = FALSE;
        reset_term();
        rc = watch_loop();
        goto out;
    }
    ROWS = SROWS - 1; statusbar = TRUE;
    sb_chr = AllocVec(SBMAX * COLS, MEMF_ANY);
    sb_att = AllocVec(SBMAX * COLS, MEMF_ANY);
    if (!sb_chr || !sb_att) { if (sb_chr) FreeVec(sb_chr); sb_chr = NULL; }
    reset_term();
    rc = RETURN_OK;
    if (direct_mode) {                  /* HOST=/PORT=: one call (BBSControl's Logon) */
        default_port();
        sprintf(st_name, "%s:%ld", host_name, (long)host_port);
        session();
    } else {
        if (!(dir = AllocVec(DIR_MAX * sizeof(struct DirEnt), MEMF_CLEAR))) goto out;
        dir_load();
        while (!quit) {
            int i = directory();
            if (i < 0) break;
            strncpy(host_name, dir[i].host, sizeof(host_name) - 1);
            host_port = dir[i].port;
            if (!host_port && dir[i].type != CT_MODEM) default_port();
            strncpy(st_name, dir[i].name, sizeof(st_name) - 1);
            cur_ent = &dir[i];
            dir_stamp(&dir[i]);
            dir_save();
            if (session() == END_QUIT) break;
        }
    }

out:
    if (sock >= 0) CloseSocket(sock);
    ser_close();
    if (tm_io) { CloseDevice((struct IORequest *)tm_io); DeleteIORequest((struct IORequest *)tm_io); }
    if (tm_port) DeleteMsgPort(tm_port);
    if (cap_fh) { cap_flush(); Close(cap_fh); }
    if (dir) FreeVec(dir);
    if (sb_chr) FreeVec(sb_chr);
    if (sb_att) FreeVec(sb_att);
    if (AslBase) CloseLibrary(AslBase);
    if (IFFParseBase) CloseLibrary(IFFParseBase);
    if (win) CloseWindow(win);
    if (scr) CloseScreen(scr);
    if (strip) FreeVec(strip);
    if (SocketBase) CloseLibrary(SocketBase);
    if (KeymapBase) CloseLibrary(KeymapBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}

/* Started from its icon: the same options as the Shell arguments, read from the
 * icon's Tool Types (PORT=n, HOST=name, MODEID=hex, NATIVE, SMALL); anything
 * missing keeps its default. */
static void wb_tooltypes(struct WBStartup *wb)
{
    struct Library *IconBase;
    struct DiskObject *dob;
    BPTR old;
    char *v;
    if (!wb || wb->sm_NumArgs < 1) return;
    if (!(IconBase = OpenLibrary((STRPTR)"icon.library", 37))) return;
    old = CurrentDir(wb->sm_ArgList[0].wa_Lock);
    if ((dob = GetDiskObject(wb->sm_ArgList[0].wa_Name))) {
        CONST_STRPTR *tt = (CONST_STRPTR *)dob->do_ToolTypes;
        if ((v = (char *)FindToolType(tt, (STRPTR)"PORT")) && atol(v) > 0) { host_port = atol(v); direct_mode = TRUE; }
        if ((v = (char *)FindToolType(tt, (STRPTR)"HOST")) && *v) {
            strncpy(host_name, v, sizeof(host_name) - 1); direct_mode = TRUE;
        }
        if ((v = (char *)FindToolType(tt, (STRPTR)"MODEID")) && *v) arg_modeid = strtoul(v, NULL, 16);
        if (FindToolType(tt, (STRPTR)"NATIVE")) arg_native = TRUE;
        if (FindToolType(tt, (STRPTR)"SMALL")) arg_small = TRUE;
        if ((v = (char *)FindToolType(tt, (STRPTR)"WATCH")) && atol(v) > 0) watch_node = (int)atol(v);
        FreeDiskObject(dob);
    }
    CurrentDir(old);
    CloseLibrary(IconBase);
}

/* No PORT= given: the port the running NilBBS listens on, else port= in its config, else 2323 */
static void default_port(void)
{
    struct BBSShared *S;
    struct Cfg *c;
    if (host_port > 0) return;
    if ((S = shared_find()) && S->port) { host_port = S->port; return; }
    if ((c = cfg_load(bbs_config()))) { host_port = cfg_int(c, "port", 0); cfg_free(c); }
    if (host_port <= 0) host_port = 2323;
}

int main(int argc, char **argv)
{
    if (argc == 0) wb_tooltypes((struct WBStartup *)argv);   /* from its icon */
    if (argc > 0) {             /* from a Shell */
        static LONG a[6];
        struct RDArgs *rd;
        from_cli = TRUE;
        if (!(rd = ReadArgs((STRPTR)"PORT/N,HOST/K,MODEID/K,NATIVE/S,SMALL/S,WATCH/K/N", a, NULL))) {
            PrintFault(IoErr(), (STRPTR)"NilTerm");
            return RETURN_FAIL;
        }
        if (a[0]) { host_port = *(LONG *)a[0]; direct_mode = TRUE; }
        if (a[1]) { strncpy(host_name, (char *)a[1], sizeof(host_name) - 1); direct_mode = TRUE; }
        if (a[2]) arg_modeid = strtoul((char *)a[2], NULL, 16);
        arg_native = a[3] != 0;
        arg_small = a[4] != 0;
        if (a[5]) watch_node = (int)*(LONG *)a[5];
        FreeArgs(rd);
    }
    return run_with_stack(16384, real_main);
}
