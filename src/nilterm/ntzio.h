/*
 * ntzio.h - what NilBBS's transfer code (src/node/zmodem.c, xymodem.c) needs from a node,
 * supplied by NilTerm instead: the connection's input ring and the telnet output.
 * ntzm.c / ntxy.c include it and then the BBS's own source, unchanged; this header
 * defines node.h's guard so the real node.h (the whole BBS node) stays out.
 */
#ifndef NILTERM_ZIO_H
#define NILTERM_ZIO_H
#include <exec/types.h>
#include "../common/bbs.h"

#define NILBBS_NODE_H

struct NodeCtx {
    BOOL  online;           /* FALSE: the call dropped, or the sysop pressed Esc */
    UWORD in_head, in_tail; /* the input ring (NilTerm's ZIN_SIZE) */
};
extern struct NodeCtx N;

LONG tn_wait(ULONG ms, ULONG extrasigs, ULONG *gotsigs);   /* >0 = input available */
BOOL tn_flush(void);
void tn_raw(const UBYTE *buf, LONG len);                     /* IAC-escaped */
void tn_rawflush(const UBYTE *buf, LONG len);
LONG in_avail(void);
LONG in_get(void);                                           /* -1 if empty */
void in_unget(UBYTE c);
void tn_set_binary(BOOL on);
#endif
