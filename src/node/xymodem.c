/*
 * xymodem.c - XMODEM (checksum / CRC), XMODEM-1K and YMODEM batch, both ways.
 *
 * For callers whose terminal has no ZMODEM (or a flaky one).  Same wire as
 * ZMODEM: the telnet stream in binary mode (tn_set_binary), bytes through
 * tn_raw()/in_get().
 *
 * Send:    the receiver starts with 'C' (CRC-16) or NAK (8-bit checksum).
 *          YMODEM adds block 0 (name, size, date) per file and an empty
 *          block 0 to end the batch.  The last block is padded with ^Z.
 * Receive: we start with 'C' (NAK after a few tries, for checksum-only
 *          senders), take 128- or 1024-byte blocks, ACK good ones, NAK bad
 *          ones, ACK duplicates of the previous block.  YMODEM files are cut
 *          to the size in block 0; XMODEM files lose the trailing ^Z padding.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "node.h"
#include "zmodem.h"

#define SOH 0x01
#define STX 0x02
#define EOT 0x04
#define ACK 0x06
#define NAK 0x15
#define CAN 0x18
#define SUB 0x1A

#define R_TIMEOUT (-1)
#define R_HANGUP  (-2)

const char *proto_name(int p)
{
    return p == PROTO_Y ? "YMODEM" : p == PROTO_X1K ? "XMODEM-1K" : p == PROTO_X ? "XMODEM" : "ZMODEM";
}

static UWORD crctab[256];

static void mk_crc(void)
{
    int i, j;
    if (crctab[1]) return;
    for (i = 0; i < 256; i++) {
        UWORD c = (UWORD)(i << 8);
        for (j = 0; j < 8; j++) c = (c & 0x8000) ? (UWORD)((c << 1) ^ 0x1021) : (UWORD)(c << 1);
        crctab[i] = c;
    }
}

static UWORD crc16(const UBYTE *p, LONG n)
{
    UWORD crc = 0;
    while (n-- > 0) crc = (UWORD)(crctab[((crc >> 8) ^ *p++) & 0xFF] ^ (crc << 8));
    return crc;
}

static LONG getc_t(LONG secs)
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

static void put1(UBYTE c) { tn_rawflush(&c, 1); }

static void cancel(void)
{
    static const UBYTE can[10] = { CAN, CAN, CAN, CAN, CAN, 8, 8, 8, 8, 8 };
    tn_rawflush(can, 10);
}

static void purge(void)
{
    /* swallow whatever is still arriving (e.g. a block we're rejecting) */
    while (getc_t(1) >= 0) ;
}

static void begin(void)
{
    mk_crc();
    tn_set_binary(TRUE);
    N.in_head = N.in_tail = 0;
}

static void end(void)
{
    tn_set_binary(FALSE);
    tn_wait(500, 0, NULL);
    N.in_head = N.in_tail = 0;
}

/* ---- send ---------------------------------------------------------------------------- */

/* wait for the receiver's go: 'C' = CRC, NAK = checksum.  0 = cancelled */
static int wait_start(LONG secs, BOOL *crc)
{
    ULONG start = bbs_now();
    int cans = 0;
    while ((LONG)(bbs_now() - start) < secs) {
        LONG c = getc_t(2);
        if (c == R_HANGUP) return 0;
        if (c == 'C' || c == 'G') { *crc = TRUE; return 1; }
        if (c == NAK) { *crc = FALSE; return 1; }
        if (c == CAN) { if (++cans >= 2) return 0; } else cans = 0;
    }
    return 0;
}

/* one block, retried until ACKed; FALSE = give up */
static BOOL send_block(UBYTE num, const UBYTE *data, LONG size, BOOL crc, struct ZStats *st)
{
    UBYTE hdr[3], tail[2];
    int tries;
    hdr[0] = size == 1024 ? STX : SOH;
    hdr[1] = num;
    hdr[2] = (UBYTE)~num;
    if (crc) { UWORD c = crc16(data, size); tail[0] = (UBYTE)(c >> 8); tail[1] = (UBYTE)c; }
    else { UBYTE s = 0; LONG i; for (i = 0; i < size; i++) s += data[i]; tail[0] = s; }
    for (tries = 0; tries < 10; tries++) {
        LONG c;
        int cans = 0;
        tn_raw(hdr, 3);
        tn_raw(data, size);
        tn_raw(tail, crc ? 2 : 1);
        tn_flush();
        for (;;) {
            c = getc_t(10);
            if (c == ACK) return TRUE;
            if (c == NAK || c == R_TIMEOUT || c == 'C') break;
            if (c == R_HANGUP) return FALSE;
            if (c == CAN) { if (++cans >= 2) return FALSE; }
        }
        st->errors++;
    }
    return FALSE;
}

static BOOL send_eot(void)
{
    int tries;
    for (tries = 0; tries < 10; tries++) {
        LONG c;
        put1(EOT);
        c = getc_t(10);
        if (c == ACK) return TRUE;
        if (c == R_HANGUP || c == CAN) return FALSE;
    }
    return FALSE;
}

/* YMODEM block 0: "name\0size mtime mode\0", zero padded.  name NULL = end of batch */
static BOOL send_header(const char *name, LONG size, ULONG mtime, BOOL crc, struct ZStats *st)
{
    UBYTE blk[128];
    memset(blk, 0, sizeof(blk));
    if (name) {
        LONG n = strlen(name);
        strcpy((char *)blk, name);
        /* Unix time for the date: our epoch is 1 Jan 1978, theirs 1 Jan 1970 */
        sprintf((char *)blk + n + 1, "%ld %lo 100644 0", size, mtime + 252460800UL);
    }
    return send_block(0, blk, 128, crc, st);
}

static int send_one(const char *path, int proto, BOOL crc, struct ZStats *st)
{
    UBYTE *buf = AllocVec(1024, 0);
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    UBYTE num = 1;
    int ok = 0;
    LONG bsize = proto == PROTO_X ? 128 : 1024;
    if (!buf || !fh) goto out;
    st->bytes = 0;
    for (;;) {
        LONG n = Read(fh, buf, bsize), size = bsize;
        if (n < 0) goto out;
        if (n == 0) break;
        /* a short tail fits a 128-byte block: less padding on the wire */
        if (bsize == 1024 && n <= 128) size = 128;
        if (n < size) memset(buf + n, SUB, size - n);
        if (!send_block(num++, buf, size, crc, st)) goto out;
        st->bytes += n;
        if (n < bsize) break;
    }
    ok = send_eot();
out:
    if (fh) Close(fh);
    if (buf) FreeVec(buf);
    return ok;
}

LONG xy_send(const char **paths, const char **names, LONG count, int proto, struct ZStats *st)
{
    LONG i, sent = 0;
    BOOL crc = TRUE;
    memset(st, 0, sizeof(*st));
    begin();
    if (proto != PROTO_Y) count = count > 0 ? 1 : 0;     /* XMODEM: one file */
    for (i = 0; i < count && N.online; i++) {
        LONG size = file_size(paths[i]);
        ULONG mtime = 0;
        st->file = i + 1;
        if (!wait_start(i == 0 ? 60 : 20, &crc)) { cancel(); goto done; }
        if (proto == PROTO_Y) {
            struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
            BPTR l = Lock((STRPTR)paths[i], ACCESS_READ);
            if (fib && l && Examine(l, fib))
                mtime = fib->fib_Date.ds_Days * 86400UL + fib->fib_Date.ds_Minute * 60UL;
            if (l) UnLock(l);
            if (fib) FreeDosObject(DOS_FIB, fib);
            if (!send_header(names[i], size, mtime, crc, st)) { cancel(); goto done; }
            if (!wait_start(20, &crc)) { cancel(); goto done; }
        }
        if (!send_one(paths[i], proto, crc, st)) { cancel(); goto done; }
        st->total += st->bytes;
        st->ok_mask |= 1UL << (i & 31);
        sent++;
    }
    if (proto == PROTO_Y && N.online && wait_start(20, &crc)) send_header(NULL, 0, 0, crc, st);
done:
    end();
    return sent;
}

/* ---- receive ------------------------------------------------------------------------ */

/* read one block after its SOH/STX: returns its number (0..255) with the data
 * in buf, -1 for a bad block (NAK it), R_HANGUP */
static LONG read_block(LONG size, BOOL crc, UBYTE *buf)
{
    LONG n, i, c;
    UBYTE num, inv;
    if ((c = getc_t(5)) < 0) return c == R_HANGUP ? R_HANGUP : -1;
    num = (UBYTE)c;
    if ((c = getc_t(5)) < 0) return c == R_HANGUP ? R_HANGUP : -1;
    inv = (UBYTE)c;
    for (i = 0; i < size; i++) {
        if ((c = getc_t(5)) < 0) return c == R_HANGUP ? R_HANGUP : -1;
        buf[i] = (UBYTE)c;
    }
    if (crc) {
        LONG hi = getc_t(5), lo;
        if (hi < 0) return hi == R_HANGUP ? R_HANGUP : -1;
        if ((lo = getc_t(5)) < 0) return lo == R_HANGUP ? R_HANGUP : -1;
        if (crc16(buf, size) != (UWORD)((hi << 8) | lo)) return -1;
    } else {
        UBYTE s = 0;
        LONG ck = getc_t(5);
        if (ck < 0) return ck == R_HANGUP ? R_HANGUP : -1;
        for (n = 0; n < size; n++) s += buf[n];
        if (s != (UBYTE)ck) return -1;
    }
    if ((UBYTE)~num != inv) return -1;
    return num;
}

/* receive one file's blocks into fh (block 1 onwards).  `want` = the size from
 * YMODEM's header, or -1 for XMODEM.  Returns bytes written or -1. */
static LONG recv_data(BPTR fh, LONG want, BOOL *crc, BOOL started, struct ZStats *st)
{
    UBYTE *buf = AllocVec(1024, 0);
    UBYTE expect = 1;
    LONG written = 0, tries = 0, lastlen = 0;
    int cans = 0;
    if (!buf) return -1;
    for (;;) {
        LONG c, num, size;
        if (!started) {
            /* invite the sender: CRC first, then checksum for old senders */
            put1(tries < 4 ? 'C' : NAK);
            *crc = tries < 4;
        }
        c = getc_t(started ? 10 : 3);
        if (c == R_HANGUP) { written = -1; break; }
        if (c == R_TIMEOUT) {
            if (++tries > 10) { cancel(); written = -1; break; }
            if (started) put1(NAK);
            continue;
        }
        if (c == CAN) { if (++cans >= 2) { written = -1; break; } continue; }
        cans = 0;
        if (c == EOT) {
            put1(ACK);
            /* XMODEM: drop the ^Z padding of the last block */
            if (want < 0 && lastlen) {
                LONG cut = 0;
                while (cut < lastlen && buf[lastlen - 1 - cut] == SUB) cut++;
                if (cut) {
                    written -= cut;
                    SetFileSize(fh, written, OFFSET_BEGINNING);
                }
            }
            break;
        }
        if (c != SOH && c != STX) continue;             /* line noise */
        started = TRUE;
        size = c == STX ? 1024 : 128;
        num = read_block(size, *crc, buf);
        if (num == R_HANGUP) { written = -1; break; }
        if (num < 0) { purge(); st->errors++; put1(NAK); continue; }
        if ((UBYTE)num == (UBYTE)(expect - 1)) { put1(ACK); continue; }      /* a repeat */
        if ((UBYTE)num != expect) { cancel(); written = -1; break; }
        {
            LONG keep = size;
            if (want >= 0 && written + keep > want) keep = want - written;
            if (keep > 0 && Write(fh, buf, keep) != keep) { cancel(); written = -1; break; }
            written += keep;
            lastlen = keep;
            st->bytes = written;
        }
        expect++;
        tries = 0;
        put1(ACK);
    }
    FreeVec(buf);
    return written;
}

/* a free name in dir: name, then name.1, name.2 ... */
static void unique_path(const char *dir, char *name, char *path)
{
    int i;
    char base[32];
    str_copy(base, name, 26);
    path_join(path, dir, name);
    for (i = 1; file_exists(path) && i < 100; i++) {
        sprintf(name, "%s.%d", base, i);
        path_join(path, dir, name);
    }
}

LONG xy_receive(const char *dir, char names[][32], LONG maxfiles, int proto, struct ZStats *st)
{
    char path[PATHLEN];
    LONG got = 0;
    BOOL crc = TRUE;
    memset(st, 0, sizeof(*st));
    begin();
    if (proto != PROTO_Y) {
        /* XMODEM carries no name: the caller supplied it in names[0] */
        BPTR fh;
        LONG n;
        unique_path(dir, names[0], path);
        if (!(fh = Open((STRPTR)path, MODE_NEWFILE))) { cancel(); end(); return 0; }
        n = recv_data(fh, -1, &crc, FALSE, st);
        Close(fh);
        if (n > 0) { st->total = n; got = 1; }
        else DeleteFile((STRPTR)path);
        end();
        return got;
    }
    while (N.online && got < maxfiles) {
        UBYTE *hdr = AllocVec(1024, 0);
        LONG c, num = -1, tries = 0, size;
        char name[40];
        BPTR fh;
        if (!hdr) break;
        /* block 0: the file's name and size, or an empty name = batch over */
        while (num < 0 && tries++ < 10) {
            put1('C');
            c = getc_t(3);
            if (c == R_HANGUP) break;
            if (c == SOH || c == STX) {
                num = read_block(c == STX ? 1024 : 128, TRUE, hdr);
                if (num == R_HANGUP) break;
                if (num != 0) { purge(); num = -1; }
            } else if (c == EOT) put1(ACK);            /* a repeated EOT from the last file */
        }
        if (num != 0) { FreeVec(hdr); if (!got) cancel(); break; }
        put1(ACK);
        if (!hdr[0]) { FreeVec(hdr); break; }            /* end of the batch */
        upload_name((char *)hdr, name);
        size = atol((char *)hdr + strlen((char *)hdr) + 1);
        FreeVec(hdr);
        str_copy(names[got], name, 32);
        unique_path(dir, names[got], path);
        if (!(fh = Open((STRPTR)path, MODE_NEWFILE))) { cancel(); break; }
        st->file = got + 1;
        {
            LONG n = recv_data(fh, size > 0 ? size : -1, &crc, FALSE, st);
            Close(fh);
            if (n < 0) { DeleteFile((STRPTR)path); break; }
            st->total += n;
        }
        got++;
    }
    end();
    return got;
}
