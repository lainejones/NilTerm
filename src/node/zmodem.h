#ifndef NILBBS_ZMODEM_H
#define NILBBS_ZMODEM_H
#include <exec/types.h>

struct ZStats {
    LONG  file;         /* current file number (1-based) */
    ULONG bytes;        /* position in the current file */
    ULONG total;        /* bytes of completed files */
    LONG  errors;
    LONG  skipped;
    ULONG ok_mask;      /* send: bit i set = file i went through */
};

/* returns files sent, or -1 if the session never started */
LONG zm_send(const char **paths, const char **names, LONG count, struct ZStats *st);
/* files land in `dir`; their names in names[]; returns files received or -1 */
LONG zm_receive(const char *dir, char names[][32], LONG maxfiles, struct ZStats *st);

/* xymodem.c - protocols a caller can pick (UserRec.proto) */
#define PROTO_Z    0        /* ZMODEM */
#define PROTO_Y    1        /* YMODEM batch */
#define PROTO_X1K  2        /* XMODEM-1K */
#define PROTO_X    3        /* XMODEM (128-byte blocks, CRC or checksum) */
#define PROTO_MAX  3
const char *proto_name(int p);
/* XMODEM sends/receives one file (names[0] must be set for receive) */
LONG xy_send(const char **paths, const char **names, LONG count, int proto, struct ZStats *st);
LONG xy_receive(const char *dir, char names[][32], LONG maxfiles, int proto, struct ZStats *st);

#endif
