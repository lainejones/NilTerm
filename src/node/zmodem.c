/*
 * zmodem.c - ZMODEM send and receive over the telnet connection.
 *
 * Written from Chuck Forsberg's protocol description.  Streaming send with
 * ZCRCG subpackets (ZCRCQ every few KB so the receiver can object), CRC-32
 * when the receiver offers it, ZRPOS error recovery and crash resume.  We
 * escape every control character when sending and ask the sender to do the
 * same (ESCCTL), so a transfer survives telnet clients that never agreed to
 * BINARY mode and mangle CR/NUL.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "node.h"
#include "zmodem.h"

#define ZPAD   '*'
#define ZDLE   0x18
#define ZDLEE  (ZDLE ^ 0x40)
#define ZBIN   'A'
#define ZHEX   'B'
#define ZBIN32 'C'

#define ZRQINIT 0
#define ZRINIT  1
#define ZSINIT  2
#define ZACK    3
#define ZFILE   4
#define ZSKIP   5
#define ZNAK    6
#define ZABORT  7
#define ZFIN    8
#define ZRPOS   9
#define ZDATA   10
#define ZEOF    11
#define ZFERR   12
#define ZCRC    13
#define ZCHALLENGE 14
#define ZCOMPL  15
#define ZCAN    16
#define ZFREECNT 17
#define ZCOMMAND 18

#define ZCRCE 'h'
#define ZCRCG 'i'
#define ZCRCQ 'j'
#define ZCRCW 'k'
#define ZRUB0 'l'
#define ZRUB1 'm'

/* ZRINIT flags (ZF0) */
#define CANFDX  0x01
#define CANOVIO 0x02
#define CANBRK  0x04
#define CANFC32 0x20
#define ESCCTL  0x40

/* results of the low-level readers */
#define R_TIMEOUT  (-1)
#define R_HANGUP   (-2)
#define R_CAN      (-3)
#define R_ERROR    (-4)
#define GOTCRCE    (0x100 | ZCRCE)
#define GOTCRCG    (0x100 | ZCRCG)
#define GOTCRCQ    (0x100 | ZCRCQ)
#define GOTCRCW    (0x100 | ZCRCW)

#define SUBPKT 1024

static UWORD crc16tab[256];
static ULONG crc32tab[256];
static BOOL  tables;
static BOOL  use32;           /* sending: receiver can do CRC-32 */
static BOOL  esc_all = TRUE;  /* sending: escape every control character */
static UBYTE rxhdr[4];        /* last header's four data bytes */
static int   rxtype_crc32;    /* last received frame used CRC-32 */
static int   cancount;

static void mk_tables(void)
{
    int i, j;
    if (tables) return;
    for (i = 0; i < 256; i++) {
        UWORD c = (UWORD)(i << 8);
        ULONG d = i;
        for (j = 0; j < 8; j++) {
            c = (c & 0x8000) ? (UWORD)((c << 1) ^ 0x1021) : (UWORD)(c << 1);
            d = (d & 1) ? (d >> 1) ^ 0xEDB88320UL : d >> 1;
        }
        crc16tab[i] = c;
        crc32tab[i] = d;
    }
    tables = TRUE;
}
/* table-driven, non-augmented CRC-16/XMODEM: the value is final as-is, and
 * running it over data + the two CRC bytes gives 0 */
static UWORD crc16_byte(UWORD crc, UBYTE b) { return (UWORD)(crc16tab[((crc >> 8) ^ b) & 0xFF] ^ (crc << 8)); }
static ULONG crc32_byte(ULONG crc, UBYTE b) { return crc32tab[(crc ^ b) & 0xFF] ^ (crc >> 8); }

/* ---- raw I/O ------------------------------------------------------------------ */

static void put(UBYTE c) { tn_raw(&c, 1); }

static LONG getb(LONG secs)
{
    ULONG start;
    LONG c = in_get();          /* buffered byte: no clock read on the per-byte path */
    if (c >= 0) return c;
    start = bbs_now();
    for (;;) {
        if (!N.online) return R_HANGUP;
        if ((LONG)(bbs_now() - start) >= secs) return R_TIMEOUT;
        tn_wait(1000, 0, NULL);
        c = in_get();
        if (c >= 0) return c;
    }
}

/* next byte with XON/XOFF noise dropped */
static LONG getb_noxon(LONG secs)
{
    for (;;) {
        LONG c = getb(secs);
        if (c < 0) return c;
        if ((c & 0x7F) == 0x11 || (c & 0x7F) == 0x13) continue;
        return c;
    }
}

/* ZDLE-decoded read.  Returns a byte, GOTCRCx, or R_* */
static LONG zdlread(LONG secs)
{
    LONG c;
again:
    c = getb(secs);
    if (c < 0) return c;
    if (c != ZDLE) {
        if ((c & 0x7F) == 0x11 || (c & 0x7F) == 0x13) goto again;
        cancount = 0;
        return c;
    }
    /* ZDLE is also CAN: five in a row abort the session */
    if (++cancount >= 5) return R_CAN;
again2:
    c = getb(secs);
    if (c < 0) return c;
    switch (c) {
    case ZDLE:
        if (++cancount >= 5) return R_CAN;
        goto again2;
    case ZCRCE: case ZCRCG: case ZCRCQ: case ZCRCW:
        cancount = 0;
        return 0x100 | c;
    case ZRUB0: cancount = 0; return 0x7F;
    case ZRUB1: cancount = 0; return 0xFF;
    case 0x11: case 0x13: case 0x91: case 0x93:
        goto again2;
    default:
        cancount = 0;
        if ((c & 0x60) == 0x40) return c ^ 0x40;
        return R_ERROR;
    }
}

static void zsendline(UBYTE c)
{
    switch (c) {
    case ZDLE:
        put(ZDLE); put(ZDLEE); return;
    case 0x11: case 0x13: case 0x91: case 0x93:
    case 0x0D: case 0x8D: case 0x10: case 0x90:
        put(ZDLE); put(c ^ 0x40); return;
    case 0x7F: put(ZDLE); put(ZRUB0); return;
    case 0xFF: put(ZDLE); put(ZRUB1); return;
    }
    if (esc_all && ((c & 0x7F) < 0x20)) { put(ZDLE); put(c ^ 0x40); return; }
    put(c);
}

/* ---- headers -------------------------------------------------------------------- */

static void puthex(UBYTE b)
{
    static const char *hx = "0123456789abcdef";
    put((UBYTE)hx[b >> 4]);
    put((UBYTE)hx[b & 15]);
}

static void stohdr(UBYTE *h, ULONG pos)
{
    h[0] = (UBYTE)pos; h[1] = (UBYTE)(pos >> 8); h[2] = (UBYTE)(pos >> 16); h[3] = (UBYTE)(pos >> 24);
}

static ULONG rclhdr(const UBYTE *h)
{
    return (ULONG)h[0] | ((ULONG)h[1] << 8) | ((ULONG)h[2] << 16) | ((ULONG)h[3] << 24);
}

static void zshhdr(UBYTE type, const UBYTE *h)
{
    UWORD crc = 0;
    int i;
    put(ZPAD); put(ZPAD); put(ZDLE); put(ZHEX);
    puthex(type);
    crc = crc16_byte(crc, type);
    for (i = 0; i < 4; i++) { puthex(h[i]); crc = crc16_byte(crc, h[i]); }
    puthex((UBYTE)(crc >> 8));
    puthex((UBYTE)crc);
    put('\r'); put(0x8A);
    if (type != ZFIN && type != ZACK) put(0x11);    /* XON */
    tn_flush();
}

static void zsbhdr(UBYTE type, const UBYTE *h)
{
    int i;
    put(ZPAD); put(ZDLE);
    if (use32) {
        ULONG crc = 0xFFFFFFFFUL;
        put(ZBIN32);
        zsendline(type); crc = crc32_byte(crc, type);
        for (i = 0; i < 4; i++) { zsendline(h[i]); crc = crc32_byte(crc, h[i]); }
        crc = ~crc;
        for (i = 0; i < 4; i++) { zsendline((UBYTE)crc); crc >>= 8; }
    } else {
        UWORD crc = 0;
        put(ZBIN);
        zsendline(type); crc = crc16_byte(crc, type);
        for (i = 0; i < 4; i++) { zsendline(h[i]); crc = crc16_byte(crc, h[i]); }
        zsendline((UBYTE)(crc >> 8));
        zsendline((UBYTE)crc);
    }
}

static void zsdata(const UBYTE *buf, LONG len, UBYTE frameend)
{
    LONG i;
    if (use32) {
        ULONG crc = 0xFFFFFFFFUL;
        for (i = 0; i < len; i++) { zsendline(buf[i]); crc = crc32_byte(crc, buf[i]); }
        put(ZDLE); put(frameend);
        crc = ~crc32_byte(crc, frameend);
        for (i = 0; i < 4; i++) { zsendline((UBYTE)crc); crc >>= 8; }
    } else {
        UWORD crc = 0;
        for (i = 0; i < len; i++) { zsendline(buf[i]); crc = crc16_byte(crc, buf[i]); }
        put(ZDLE); put(frameend);
        crc = crc16_byte(crc, frameend);
        zsendline((UBYTE)(crc >> 8));
        zsendline((UBYTE)crc);
    }
    if (frameend == ZCRCW) { put(0x11); tn_flush(); }
}

static LONG hexnib(LONG secs)
{
    LONG c = getb_noxon(secs);
    if (c < 0) return c;
    c &= 0x7F;
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return R_ERROR;
}

static LONG hexbyte(LONG secs)
{
    LONG a = hexnib(secs), b;
    if (a < 0) return a;
    if ((b = hexnib(secs)) < 0) return b;
    return (a << 4) | b;
}

/* Wait for a header.  Returns the frame type (>= 0) or R_*.  The header's
 * data bytes land in rxhdr. */
static LONG zgethdr(LONG secs)
{
    LONG c, garbage = 0;
    int i;
    for (;;) {
        c = getb_noxon(secs);
        if (c < 0) return c;
        if (c == 0x18) {                     /* CAN */
            if (++cancount >= 5) return R_CAN;
            continue;
        }
        cancount = 0;
        if (c != ZPAD) {
            if (++garbage > 2400) return R_ERROR;
            continue;
        }
        /* one or more ZPADs, then ZDLE */
        do c = getb_noxon(secs); while (c == ZPAD);
        if (c < 0) return c;
        if (c != ZDLE) continue;
        c = getb_noxon(secs);
        if (c < 0) return c;

        if (c == ZHEX) {
            UWORD crc = 0;
            LONG t = hexbyte(secs), b;
            if (t < 0) continue;
            crc = crc16_byte(crc, (UBYTE)t);
            for (i = 0; i < 4; i++) {
                if ((b = hexbyte(secs)) < 0) break;
                rxhdr[i] = (UBYTE)b;
                crc = crc16_byte(crc, (UBYTE)b);
            }
            if (i < 4) continue;
            if ((b = hexbyte(secs)) < 0) continue;
            crc = crc16_byte(crc, (UBYTE)b);
            if ((b = hexbyte(secs)) < 0) continue;
            crc = crc16_byte(crc, (UBYTE)b);
            if (crc) continue;              /* bad CRC: wait for the next one */
            /* swallow the CR LF (and XON) that follow a hex header */
            c = getb(1);
            if (c == '\r' || c == 0x8D) { c = getb(1); if (c >= 0 && c != '\n' && c != 0x8A) in_unget((UBYTE)c); }
            else if (c >= 0) in_unget((UBYTE)c);
            rxtype_crc32 = 0;
            return t;
        }
        if (c == ZBIN || c == ZBIN32) {
            LONG t, b;
            if (c == ZBIN32) {
                ULONG crc = 0xFFFFFFFFUL;
                if ((t = zdlread(secs)) < 0 || t > 0xFF) continue;
                crc = crc32_byte(crc, (UBYTE)t);
                for (i = 0; i < 4; i++) {
                    if ((b = zdlread(secs)) < 0 || b > 0xFF) break;
                    rxhdr[i] = (UBYTE)b; crc = crc32_byte(crc, (UBYTE)b);
                }
                if (i < 4) continue;
                for (i = 0; i < 4; i++) {
                    if ((b = zdlread(secs)) < 0 || b > 0xFF) break;
                    crc = crc32_byte(crc, (UBYTE)b);
                }
                if (i < 4 || crc != 0xDEBB20E3UL) continue;
                rxtype_crc32 = 1;
            } else {
                UWORD crc = 0;
                if ((t = zdlread(secs)) < 0 || t > 0xFF) continue;
                crc = crc16_byte(crc, (UBYTE)t);
                for (i = 0; i < 4; i++) {
                    if ((b = zdlread(secs)) < 0 || b > 0xFF) break;
                    rxhdr[i] = (UBYTE)b; crc = crc16_byte(crc, (UBYTE)b);
                }
                if (i < 4) continue;
                for (i = 0; i < 2; i++) {
                    if ((b = zdlread(secs)) < 0 || b > 0xFF) break;
                    crc = crc16_byte(crc, (UBYTE)b);
                }
                if (i < 2 || crc) continue;
                rxtype_crc32 = 0;
            }
            return t;
        }
    }
}

/* read a data subpacket into buf.  Returns GOTCRCx with *len set, or R_* */
static LONG zrdata(UBYTE *buf, LONG max, LONG *len)
{
    LONG c, n = 0;
    int i;
    *len = 0;
    if (rxtype_crc32) {
        ULONG crc = 0xFFFFFFFFUL;
        for (;;) {
            if ((c = zdlread(10)) < 0) return c;
            if (c & 0x100) break;
            if (n >= max) return R_ERROR;
            buf[n++] = (UBYTE)c;
            crc = crc32_byte(crc, (UBYTE)c);
        }
        crc = crc32_byte(crc, (UBYTE)c);
        for (i = 0; i < 4; i++) {
            LONG b = zdlread(10);
            if (b < 0 || b > 0xFF) return R_ERROR;
            crc = crc32_byte(crc, (UBYTE)b);
        }
        if (crc != 0xDEBB20E3UL) return R_ERROR;
    } else {
        UWORD crc = 0;
        for (;;) {
            if ((c = zdlread(10)) < 0) return c;
            if (c & 0x100) break;
            if (n >= max) return R_ERROR;
            buf[n++] = (UBYTE)c;
            crc = crc16_byte(crc, (UBYTE)c);
        }
        crc = crc16_byte(crc, (UBYTE)c);
        for (i = 0; i < 2; i++) {
            LONG b = zdlread(10);
            if (b < 0 || b > 0xFF) return R_ERROR;
            crc = crc16_byte(crc, (UBYTE)b);
        }
        if (crc) return R_ERROR;
    }
    *len = n;
    return c;
}

static void send_cancel(void)
{
    static const UBYTE can[18] = { 24,24,24,24,24,24,24,24,24,24, 8,8,8,8,8,8,8,8 };
    tn_rawflush(can, 18);
}

static void begin(void)
{
    mk_tables();
    tn_set_binary(TRUE);
    cancount = 0;
    N.in_head = N.in_tail = 0;
}

static void end(void)
{
    tn_set_binary(FALSE);
    /* let any trailing protocol bytes arrive, then throw them away */
    tn_wait(500, 0, NULL);
    N.in_head = N.in_tail = 0;
}

/* ---- send ------------------------------------------------------------------------ */

/* send one file; returns 1 = sent, 0 = skipped by receiver, -1 = session failed */
static int send_file(const char *path, const char *name, LONG size, ULONG mtime,
                     LONG files_left, LONG bytes_left, struct ZStats *st)
{
    UBYTE hdr[4], *buf;
    LONG n, t, tries;
    ULONG pos = 0;
    BPTR fh;
    int result = -1;

    if (!(buf = AllocVec(SUBPKT + 256, 0))) return -1;
    if (!(fh = Open((STRPTR)path, MODE_OLDFILE))) { FreeVec(buf); return 0; }

    /* ZFILE: name \0 "size mtime mode serial files_left bytes_left" \0 */
    for (tries = 0; tries < 10; tries++) {
        LONG l;
        stohdr(hdr, 0);
        hdr[3] = 1;                     /* ZF0 = ZCBIN: binary transfer */
        zsbhdr(ZFILE, hdr);
        l = strlen(name) + 1;
        memcpy(buf, name, l);
        /* mtime in Unix seconds, octal; Amiga epoch is 8 years + 2 days later */
        l += sprintf((char *)buf + l, "%ld %lo 100644 0 %ld %ld", size,
                     mtime + 252460800UL, files_left, bytes_left) + 1;
        zsdata(buf, l, ZCRCW);
        t = zgethdr(20);
        if (t == ZRPOS) { pos = rclhdr(rxhdr); break; }
        if (t == ZSKIP) { result = 0; goto out; }
        if (t == ZCRC) {
            /* receiver asks for the file CRC (crash recovery check) */
            ULONG crc = 0xFFFFFFFFUL;
            LONG r;
            Seek(fh, 0, OFFSET_BEGINNING);
            while ((r = Read(fh, buf, SUBPKT)) > 0) { LONG i; for (i = 0; i < r; i++) crc = crc32_byte(crc, buf[i]); }
            stohdr(hdr, ~crc);
            zsbhdr(ZCRC, hdr);
            t = zgethdr(20);
            if (t == ZRPOS) { pos = rclhdr(rxhdr); break; }
            if (t == ZSKIP) { result = 0; goto out; }
        }
        if (t == R_CAN || t == R_HANGUP) goto out;
    }
    if (tries >= 10) goto out;

resend:
    if (Seek(fh, pos, OFFSET_BEGINNING) < 0) goto out;
    stohdr(hdr, pos);
    zsbhdr(ZDATA, hdr);
    for (;;) {
        UBYTE fe;
        n = Read(fh, buf, SUBPKT);
        if (n < 0) n = 0;
        fe = (n < SUBPKT) ? ZCRCE : ((pos / SUBPKT) % 8 == 7 ? ZCRCQ : ZCRCG);
        zsdata(buf, n, fe);
        pos += n;
        st->bytes = pos;
        if (fe == ZCRCQ) tn_flush();
        /* did the receiver say something? (non-blocking) */
        if (fe != ZCRCE) {
            tn_wait(0, 0, NULL);
            if (in_avail()) {
                LONG c = in_get();
                if (c == ZPAD || c == 0x18) {
                    in_unget((UBYTE)c);
                    t = zgethdr(10);
                    if (t == ZRPOS) {
                        pos = rclhdr(rxhdr);
                        st->errors++;
                        goto resend;
                    }
                    if (t == ZSKIP) { result = 0; goto out; }
                    if (t == R_CAN || t == ZABORT || t == ZFERR || t == R_HANGUP) goto out;
                    /* ZACK etc: ignore and carry on */
                }
            }
        }
        if (!N.online) goto out;
        if (fe == ZCRCE) break;
    }
    tn_flush();

    /* ZEOF and wait for the receiver to confirm */
    for (tries = 0; tries < 10; tries++) {
        stohdr(hdr, pos);
        zsbhdr(ZEOF, hdr);
        tn_flush();
        t = zgethdr(20);
        if (t == ZRINIT) { result = 1; break; }
        if (t == ZRPOS) { pos = rclhdr(rxhdr); st->errors++; goto resend; }
        if (t == ZSKIP) { result = 0; break; }
        if (t == R_CAN || t == R_HANGUP) break;
    }
out:
    Close(fh);
    FreeVec(buf);
    return result;
}

LONG zm_send(const char **paths, const char **names, LONG count, struct ZStats *st)
{
    UBYTE hdr[4];
    LONG t, i, tries, bytes_left = 0, sent = 0;
    memset(st, 0, sizeof(*st));
    begin();
    for (i = 0; i < count; i++) { LONG s = file_size(paths[i]); if (s > 0) bytes_left += s; }

    tn_raw((const UBYTE *)"rz\r", 3);
    stohdr(hdr, 0);
    zshhdr(ZRQINIT, hdr);

    /* wait for the receiver's ZRINIT */
    for (tries = 0; ; tries++) {
        t = zgethdr(10);
        if (t == ZRINIT) break;
        if (t == ZCHALLENGE) { zshhdr(ZACK, rxhdr); continue; }
        if (t == ZCOMMAND) { stohdr(hdr, 0); zshhdr(ZCOMPL, hdr); continue; }
        if (t == R_CAN || t == R_HANGUP || tries >= 6) { end(); return -1; }
        if (t == ZRQINIT) continue;         /* our own echo */
        stohdr(hdr, 0);
        zshhdr(ZRQINIT, hdr);
    }
    use32 = (rxhdr[3] & CANFC32) != 0;
    esc_all = TRUE;

    for (i = 0; i < count && N.online; i++) {
        struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
        BPTR l;
        LONG size = 0;
        ULONG mtime = bbs_now();
        int r;
        if (fib && (l = Lock((STRPTR)paths[i], ACCESS_READ))) {
            if (Examine(l, fib)) {
                size = fib->fib_Size;
                mtime = fib->fib_Date.ds_Days * 86400UL + fib->fib_Date.ds_Minute * 60UL;
            }
            UnLock(l);
        }
        if (fib) FreeDosObject(DOS_FIB, fib);
        st->file = i + 1;
        r = send_file(paths[i], names[i], size, mtime, count - i, bytes_left, st);
        if (r < 0) break;
        if (r > 0) { sent++; st->ok_mask |= 1UL << (i & 31); st->total += size; }
        bytes_left -= size;
    }

    /* ZFIN handshake, then "OO" */
    if (N.online) {
        for (tries = 0; tries < 4; tries++) {
            stohdr(hdr, 0);
            zshhdr(ZFIN, hdr);
            t = zgethdr(10);
            if (t == ZFIN) { tn_rawflush((const UBYTE *)"OO", 2); break; }
            if (t == R_CAN || t == R_HANGUP) break;
        }
    }
    end();
    return sent;
}

/* ---- receive ------------------------------------------------------------------------ */

static void send_zrinit(void)
{
    UBYTE hdr[4];
    stohdr(hdr, 0);
    hdr[3] = CANFDX | CANOVIO | CANFC32 | ESCCTL;
    hdr[0] = 0; hdr[1] = 0;                 /* buffer size 0: full streaming */
    zshhdr(ZRINIT, hdr);
}

LONG zm_receive(const char *dir, char names[][32], LONG maxfiles, struct ZStats *st)
{
    UBYTE hdr[4], *buf;
    LONG t, got = 0, tries = 0;
    BPTR fh = 0;
    ULONG pos = 0;
    LONG fsize = 0;
    char path[PATHLEN], name[32];

    memset(st, 0, sizeof(*st));
    if (!(buf = AllocVec(8192 + 16, 0))) return -1;
    begin();
    use32 = TRUE;
    esc_all = TRUE;
    send_zrinit();

    while (N.online) {
        t = zgethdr(10);
        switch (t) {
        case ZRQINIT:
            send_zrinit();
            break;
        case ZSINIT: {
            LONG len;
            if (zrdata(buf, 8192, &len) == GOTCRCW) { stohdr(hdr, 1); zshhdr(ZACK, hdr); }
            else { stohdr(hdr, 0); zshhdr(ZNAK, hdr); }
            break;
        }
        case ZFILE: {
            LONG len, r = zrdata(buf, 8192, &len);
            if (r != GOTCRCW) { send_zrinit(); st->errors++; break; }
            buf[len] = 0;
            upload_name((char *)buf, name);
            fsize = atol((char *)buf + strlen((char *)buf) + 1);
            path_join(path, dir, name);
            if (got >= maxfiles || file_exists(path)) {
                stohdr(hdr, 0);
                zshhdr(ZSKIP, hdr);
                st->skipped++;
                break;
            }
            if (!(fh = Open((STRPTR)path, MODE_NEWFILE))) {
                stohdr(hdr, 0);
                zshhdr(ZSKIP, hdr);
                break;
            }
            pos = 0;
            st->file++;
            stohdr(hdr, pos);
            zshhdr(ZRPOS, hdr);
            break;
        }
        case ZDATA: {
            if (!fh) { stohdr(hdr, 0); zshhdr(ZSKIP, hdr); break; }
            if (rclhdr(rxhdr) != pos) {
                stohdr(hdr, pos); zshhdr(ZRPOS, hdr);
                st->errors++;
                break;
            }
            for (;;) {
                LONG len, r = zrdata(buf, 8192, &len);
                if (r < 0) {
                    if (r == R_CAN || r == R_HANGUP) goto fail;
                    st->errors++;
                    /* drain and ask for a resend from where we are */
                    tn_wait(200, 0, NULL);
                    N.in_head = N.in_tail = 0;
                    stohdr(hdr, pos); zshhdr(ZRPOS, hdr);
                    break;
                }
                if (len && Write(fh, buf, len) != len) goto fail;
                pos += len;
                st->bytes = pos;
                if (r == GOTCRCW) { stohdr(hdr, pos); zshhdr(ZACK, hdr); break; }
                if (r == GOTCRCQ) { stohdr(hdr, pos); zshhdr(ZACK, hdr); }
                if (r == GOTCRCE) break;
            }
            break;
        }
        case ZEOF:
            if (!fh || rclhdr(rxhdr) != pos) break;     /* stale: ignore */
            Close(fh);
            fh = 0;
            if (fsize && (LONG)pos != fsize) st->errors++;
            str_copy(names[got], name, 32);
            got++;
            st->total += pos;
            send_zrinit();
            break;
        case ZFIN:
            stohdr(hdr, 0);
            zshhdr(ZFIN, hdr);
            getb(2); getb(1);                            /* "OO" */
            goto done;
        case ZSKIP:
        case ZNAK:
            send_zrinit();
            break;
        case R_TIMEOUT:
            if (++tries > 6) goto fail;
            if (fh) { stohdr(hdr, pos); zshhdr(ZRPOS, hdr); }
            else send_zrinit();
            break;
        case R_CAN:
        case R_HANGUP:
            goto fail;
        default:
            if (++tries > 30) goto fail;
            break;
        }
    }
fail:
    if (fh) {
        Close(fh);
        DeleteFile((STRPTR)path);           /* partial upload */
    }
    send_cancel();
    FreeVec(buf);
    end();
    return got ? got : -1;
done:
    if (fh) Close(fh);
    FreeVec(buf);
    end();
    return got;
}
