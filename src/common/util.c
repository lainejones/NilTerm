/*
 * util.c - time, strings, logging, a buffered line reader.
 * Only dos.library I/O, so it is safe in every NilBBS executable.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/datetime.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "bbs.h"

/* ---- time --------------------------------------------------------------- */

ULONG bbs_now(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return (ULONG)ds.ds_Days * 86400UL + (ULONG)ds.ds_Minute * 60UL
         + (ULONG)ds.ds_Tick / TICKS_PER_SECOND;
}

ULONG bbs_daynum(ULONG t) { return t / 86400UL; }

static const char *mon_name[12] = {
    "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};

/* days since 1-Jan-1978 -> y/m/d (m 0-11).  Straight arithmetic over the
 * 400/100/4-year cycles on a March-based year (H. Hinnant's civil_from_days)
 * instead of a loop per year since 1978 - it runs for every listing line. */
static void civil(ULONG days, int *y, int *m, int *d)
{
    ULONG z = days + 2922UL + 719468UL;     /* 1978-01-01 -> days since 0000-03-01 */
    ULONG era = z / 146097UL;
    ULONG doe = z - era * 146097UL;                                 /* [0, 146096] */
    ULONG yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  /* [0, 399] */
    ULONG doy = doe - (365 * yoe + yoe / 4 - yoe / 100);            /* [0, 365] */
    ULONG mp = (5 * doy + 2) / 153;                                 /* [0, 11], 0 = Mar */
    *d = (int)(doy - (153 * mp + 2) / 5) + 1;
    *m = (int)(mp < 10 ? mp + 2 : mp - 10);                         /* 0 = Jan */
    *y = (int)(yoe + era * 400) + (mp >= 10);
}

void bbs_datestr(ULONG t, char *buf)
{
    int y, m, d;
    civil(t / 86400UL, &y, &m, &d);
    sprintf(buf, "%02d-%s-%02d", d, mon_name[m], y % 100);
}

void bbs_timestr(ULONG t, char *buf)
{
    ULONG s = t % 86400UL;
    sprintf(buf, "%02ld:%02ld", s / 3600, (s / 60) % 60);
}

void bbs_datetimestr(ULONG t, char *buf)
{
    bbs_datestr(t, buf);
    buf[9] = ' ';
    bbs_timestr(t, buf + 10);
}

/* ---- IP ---------------------------------------------------------------------- */

void ip_tostr(ULONG ip, char *buf)
{
    sprintf(buf, "%lu.%lu.%lu.%lu", (ip >> 24) & 255, (ip >> 16) & 255,
            (ip >> 8) & 255, ip & 255);
}

BOOL ip_parse(const char *s, ULONG *ip)
{
    ULONG v = 0;
    int part;
    for (part = 0; part < 4; part++) {
        ULONG n = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') { n = n * 10 + (*s++ - '0'); digits++; }
        if (!digits || n > 255 || digits > 3) return FALSE;
        v = (v << 8) | n;
        if (part < 3) { if (*s != '.') return FALSE; s++; }
    }
    if (*s && *s != '/' && *s != ' ' && *s != '\t') return FALSE;
    *ip = v;
    return TRUE;
}

/* ---- strings -------------------------------------------------------------- */

void str_copy(char *dst, const char *src, LONG size)
{
    if (size <= 0) return;
    while (--size > 0 && *src) *dst++ = *src++;
    *dst = 0;
}

/* Characters an AmigaDOS Shell treats specially, even inside double quotes:
 * ` runs a command, $ expands a variable, * is the quote escape, " ends the
 * quote, < > redirect, ; starts a comment.  Anything a caller typed (handle,
 * real name, an uploaded file's name) must pass through here before it is
 * written into a command line or a script. */
BOOL shell_char_ok(int c)
{
    return c >= 32 && c != 127 && !strchr("`$*\"<>;", c);
}

/* Turn the file name a caller's ZMODEM/XMODEM program sent into a safe local
 * name: path stripped, at most 30 chars (the FFS limit), and only letters,
 * digits and . _ - + ! , = @ & kept.  Uploaded names later end up quoted
 * inside LhA command lines, so shell and pattern characters must not survive. */
void upload_name(const char *in, char *out)
{
    const char *p, *base = in;
    int n = 0;
    for (p = in; *p; p++) if (*p == '/' || *p == '\\' || *p == ':') base = p + 1;
    for (p = base; *p && n < 30; p++) {
        UBYTE c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              strchr("._-+!,=@&", c)))
            c = '_';
        out[n++] = c;
    }
    out[n] = 0;
    if (!n || (n == 1 && out[0] == '.')) strcpy(out, "upload.bin");
}

/* caller-typed text shown to other callers must not carry |XX colour/MCI codes */
void str_nopipe(char *s)
{
    for (; *s; s++) if (*s == '|') *s = '!';
}

void shell_safe(char *dst, const char *src, LONG size)
{
    if (size <= 0) return;
    while (--size > 0 && *src) {
        *dst++ = shell_char_ok((UBYTE)*src) ? *src : '_';
        src++;
    }
    *dst = 0;
}

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int str_icmp(const char *a, const char *b)
{
    while (*a && lc((UBYTE)*a) == lc((UBYTE)*b)) { a++; b++; }
    return lc((UBYTE)*a) - lc((UBYTE)*b);
}

/* does `s` match `pat`?  Case doesn't matter; * in pat matches any run (even none) */
BOOL glob_icmp(const char *pat, const char *s)
{
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return TRUE;
            for (; *s; s++) if (glob_icmp(pat, s)) return TRUE;
            return FALSE;
        }
        if (!*s || lc((UBYTE)*pat) != lc((UBYTE)*s)) return FALSE;
        pat++; s++;
    }
    return !*s;
}

int str_nicmp(const char *a, const char *b, LONG n)
{
    while (n-- > 0) {
        int d = lc((UBYTE)*a) - lc((UBYTE)*b);
        if (d || !*a) return d;
        a++; b++;
    }
    return 0;
}

char *str_trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

char *str_istr(const char *hay, const char *needle)
{
    LONG n = strlen(needle);
    if (!n) return (char *)hay;
    for (; *hay; hay++)
        if (str_nicmp(hay, needle, n) == 0) return (char *)hay;
    return NULL;
}

LONG str_split(char *s, char sep, char **fields, LONG max)
{
    LONG n = 0;
    if (!max) return 0;
    fields[n++] = s;
    for (; *s; s++) {
        if (*s == sep) {
            *s = 0;
            if (n < max) fields[n++] = s + 1;
            else break;
        }
    }
    {
        LONG i;
        for (i = 0; i < n; i++) fields[i] = str_trim(fields[i]);
    }
    return n;
}

/* ---- files ------------------------------------------------------------------ */

BOOL file_exists(const char *path)
{
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    if (!l) return FALSE;
    UnLock(l);
    return TRUE;
}

LONG file_size(const char *path)
{
    struct FileInfoBlock *fib;
    BPTR l;
    LONG size = -1;
    if (!(l = Lock((STRPTR)path, ACCESS_READ))) return -1;
    if ((fib = AllocDosObject(DOS_FIB, NULL))) {
        if (Examine(l, fib) && fib->fib_DirEntryType < 0) size = fib->fib_Size;
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(l);
    return size;
}

void bbs_log(const char *file, const char *fmt, ...)
{
    char line[400];
    char ts[20];
    va_list ap;
    BPTR fh;
    LONG n;

    bbs_datetimestr(bbs_now(), ts);
    n = sprintf(line, "%s ", ts);
    va_start(ap, fmt);
    /* bounded: callers log remote-supplied paths and commands, and vsnprintf
     * returns the untruncated length, so clamp before appending the newline */
    n += vsnprintf(line + n, sizeof(line) - 1 - n, fmt, ap);
    va_end(ap);
    if (n > (LONG)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';

    fh = Open((STRPTR)file, MODE_READWRITE);        /* creates if missing */
    if (!fh) return;
    Seek(fh, 0, OFFSET_END);
    Write(fh, line, n);
    Close(fh);
}

/* ---- line reader -------------------------------------------------------------- */

BOOL lr_open(struct LineReader *lr, const char *path)
{
    lr->pos = lr->len = 0;
    lr->fh = Open((STRPTR)path, MODE_OLDFILE);
    return lr->fh != 0;
}

LONG lr_gets(struct LineReader *lr, char *line, LONG size)
{
    LONG n = 0;
    BOOL any = FALSE;
    for (;;) {
        UBYTE c;
        if (lr->pos >= lr->len) {
            lr->len = Read(lr->fh, lr->buf, sizeof(lr->buf));
            lr->pos = 0;
            if (lr->len <= 0) { lr->len = 0; break; }
        }
        c = lr->buf[lr->pos++];
        any = TRUE;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (n < size - 1) line[n++] = c;
    }
    line[n] = 0;
    return any ? n : -1;
}

void lr_close(struct LineReader *lr)
{
    if (lr->fh) Close(lr->fh);
    lr->fh = 0;
}

/* Run fn() on a stack of at least `need` bytes.  Programs started from a Shell
 * get the Shell's stack (often 4K) - libnix ignores __stack - and overflowing
 * it corrupts memory (guru 81000005), so swap to our own when it's too small. */
static struct StackSwapStruct rws_sss;
static int rws_rc;
static int (*rws_fn)(void);

int run_with_stack(ULONG need, int (*fn)(void))
{
    struct Task *me = FindTask(NULL);
    APTR stack;
    if ((ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower >= need) return fn();
    if (!(stack = AllocVec(need, MEMF_ANY))) return fn();      /* try anyway */
    rws_fn = fn;
    rws_sss.stk_Lower   = stack;
    rws_sss.stk_Upper   = (ULONG)stack + need;
    rws_sss.stk_Pointer = (APTR)rws_sss.stk_Upper;
    StackSwap(&rws_sss);
    rws_rc = rws_fn();              /* only statics from here: the old frame is elsewhere */
    StackSwap(&rws_sss);
    FreeVec(stack);
    return rws_rc;
}

/* dir + name: AmigaDOS reads "CD0:/x" as the root's parent, so no '/' after a ':' (or a '/') */
char *path_join(char *out, const char *dir, const char *name)
{
    LONG n = strlen(dir);
    if (!n || dir[n - 1] == ':' || dir[n - 1] == '/') sprintf(out, "%s%s", dir, name);
    else sprintf(out, "%s/%s", dir, name);
    return out;
}

/* the main config: NilBBS.cfg, or - on a board set up before the rename - NuzBBS.cfg */
const char *bbs_config(void)
{
    if (file_exists(BBS_CONFIG)) return BBS_CONFIG;
    if (file_exists(BBS_CONFIG_OLD)) return BBS_CONFIG_OLD;
    return BBS_CONFIG;
}
