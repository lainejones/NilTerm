/*
 * NilBBS - a native multi-node telnet BBS for AmigaOS 3.1+ (68020+).
 *
 * bbs.h - definitions shared by the daemon (NilBBS), the per-caller node
 * process (BBSNode) and the control tool (BBSCtl).
 *
 * Process model:
 *   NilBBS   listens on the telnet port, screens the caller's IP, picks a free
 *            node, hands the socket over with ReleaseSocket() and starts
 *            BBSNode for it.  It owns the shared state below.
 *   BBSNode  one process per caller: ObtainSocket(), telnet, terminal
 *            detection, login, menus, doors, messages, files.
 *   BBSCtl   CLI sysop tool: who / kick / ban / unban / shutdown.
 *
 * The three executables are separate programs, so each has its own globals
 * and its own bsdsocket.library base (socket descriptors are per-opener).
 * They meet in one public block found with FindSemaphore(BBS_SEMNAME); every
 * field of it is only touched while holding that semaphore.
 */
#ifndef NILBBS_BBS_H
#define NILBBS_BBS_H

#include <exec/types.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <dos/dos.h>

/* the Makefile passes these in: BBS_VERSION from the VERSION file, BBS_VERDATE = the build date */
#ifndef BBS_VERSION
#define BBS_VERSION     "dev"
#endif
#ifndef BBS_VERDATE
#define BBS_VERDATE     "?"
#endif
#define BBS_SEMNAME     "NilBBS.shared"
#define BBS_SHARED_MAGIC 0x414D4242UL      /* 'AMBB' */
#define BBS_SHARED_VER   5                 /* bump when BBSShared changes */

#define MAX_NODES        32
#define MAX_IPRULES      128
#define MAX_BANS         256
#define MAX_RECENT       64                /* connect-flood tracker slots */
#define NODE_MSGQ        8                 /* inter-node messages queued per node */

/* ---- sizes -------------------------------------------------------------- */
#define NAMELEN     32
#define LONGNAME    48
#define PATHLEN     256
#define LINELEN     256

/* ---- time: seconds since 1 Jan 1978 (the Amiga epoch, local time) ------- */
ULONG bbs_now(void);
void  bbs_datestr(ULONG t, char *buf);          /* "22-Sep-26"   */
void  bbs_timestr(ULONG t, char *buf);          /* "14:05"       */
void  bbs_datetimestr(ULONG t, char *buf);      /* "22-Sep-26 14:05" */
ULONG bbs_daynum(ULONG t);                      /* days since epoch */

/* ---- IP addresses: host-order ULONG, a.b.c.d = a<<24|b<<16|c<<8|d ------- */
void  ip_tostr(ULONG ip, char *buf);            /* buf >= 16 */
BOOL  ip_parse(const char *s, ULONG *ip);

/* ---- IP filter ------------------------------------------------------------ */
#define IPR_ALLOW 1
#define IPR_DENY  2

struct IPRule {
    UBYTE type;             /* IPR_ALLOW / IPR_DENY */
    UBYTE bits;             /* prefix length 0..32  */
    UWORD pad;
    ULONG net;              /* network, already masked */
};

struct IPBan {
    ULONG ip;               /* 0 = free slot */
    ULONG since;
    ULONG expires;          /* 0 = permanent */
    char  reason[40];
};

struct IPRecent {
    ULONG ip;
    ULONG first;            /* start of the counting window */
    UWORD conns;            /* connects inside the window   */
    UWORD fails;            /* failed logins (reset on success) */
    ULONG lastfail;
};

/* ---- nodes ---------------------------------------------------------------- */
#define NS_FREE      0
#define NS_STARTING  1      /* socket released, BBSNode being launched */
#define NS_LOGIN     2
#define NS_ONLINE    3
#define NS_DOOR      4

struct NodeMsg {
    UBYTE from;             /* sending node (1-based), 0 = sysop */
    UBYTE type;             /* NM_PAGE / NM_TEXT / NM_CHATREQ */
    char  fromname[NAMELEN];
    char  text[120];
};
#define NM_TEXT     1
#define NM_CHATREQ  2
#define NM_SYSOP    3       /* broadcast from the sysop */

struct NodeInfo {
    UBYTE state;
    UBYTE termtype;         /* TT_* */
    UWORD cols, rows;
    struct Task *task;      /* the BBSNode process while running */
    ULONG ip;
    ULONG connected;        /* bbs_now() at accept */
    ULONG beat;             /* heartbeat from the node */
    LONG  sockid;           /* ReleaseSocket() key while NS_STARTING */
    LONG  userid;           /* -1 = not logged in yet */
    char  user[NAMELEN];
    char  location[LONGNAME];
    char  activity[LONGNAME];
    UBYTE available;        /* accepts pages/chat */
    UBYTE msg_head, msg_tail, spy;  /* spy: the sysop is watching this node (BBSCtl WATCH) - the caller isn't told */
    struct NodeMsg msgq[NODE_MSGQ];
    /* chat link: node number we're in split-screen chat with, 0 = none */
    UBYTE chat_with;
    UBYTE tele;             /* teleconference channel the node is in, 0 = none */
    UBYTE local;            /* a local (console) session, not a telnet caller */
    UBYTE reset;            /* a sysop RESET: let go of the slot, even from a door that won't end */
};

/* teleconference: one ring of lines shared by every node */
#define TELE_RING   64
#define TK_SAY      0
#define TK_ACTION   1       /* /me */
#define TK_SYSTEM   2       /* joins, leaves */
#define TK_WHISPER  3       /* to one node only */
#define TK_LEAVE    4       /* someone left (ends a private chat) */
struct TeleLine {
    ULONG seq;
    UBYTE chan, fromnode, kind, tonode;
    char  from[NAMELEN];
    char  text[160];
};

struct BBSShared {
    struct SignalSemaphore sem;     /* public, named BBS_SEMNAME */
    char   semname[24];
    ULONG  magic;
    UWORD  version;
    UWORD  nodes;                   /* configured node count */
    struct Task *daemon;            /* signal CTRL_E = node freed / bans changed */
    ULONG  started;
    UWORD  port;
    UBYTE  shutdown;                /* set by BBSCtl SHUTDOWN */
    UBYTE  bans_dirty;
    ULONG  total_calls;             /* since start-up */

    /* IP policy (copied from the config by the daemon) */
    UWORD  flood_conns;             /* this many connects ...        */
    UWORD  flood_secs;              /* ... inside this window = ban  */
    UWORD  flood_ban_mins;
    UWORD  fail_logins;             /* failed logins before a ban    */
    UWORD  fail_ban_mins;
    UWORD  default_deny;            /* 1 = only "allow" rules get in */

    UWORD  nrules;
    UWORD  nbans_hint;
    struct IPRule   rules[MAX_IPRULES];
    struct IPBan    bans[MAX_BANS];
    struct IPRecent recent[MAX_RECENT];
    struct NodeInfo node[MAX_NODES];

    /* one lock for the user database and one for the message bases, so
     * nodes serialise their writes without holding the main semaphore */
    struct SignalSemaphore userlock;
    struct SignalSemaphore msglock;
    struct SignalSemaphore filelock;

    /* teleconference (under the main lock) */
    ULONG  tele_seq;                /* next sequence number */
    struct TeleLine tele[TELE_RING];

    /* BBSControl on the Amiga's screen: pages it can answer, and its side of a private chat */
    struct Task *ctl_task;          /* BBSControl while it runs; CTRL_F = a page or a chat line */
    ULONG  page_seq;                /* bumped for every page */
    UBYTE  page_node;               /* the node paging (0 = nobody waiting) */
    UBYTE  page_answer;             /* 0 waiting, 1 chat, 2 not now, 3 the caller gave up / timed out */
    UBYTE  ctl_chat;                /* the private channel BBSControl chats on (0 = none) */
    UBYTE  pad3;
    char   page_from[NAMELEN];
    char   page_text[80];
};

/* shared.c */
struct BBSShared *shared_find(void);   /* NULL if the daemon isn't running */
void shared_lock(struct BBSShared *s);
void shared_unlock(struct BBSShared *s);
BOOL task_alive(struct Task *t);
BOOL node_reset_start(struct BBSShared *s, int n);  /* 1-based; FALSE if the node is free */
#define NODE_RESET_FORCE 15     /* the daemon frees a slot by force this long after a RESET */

/* ipfilter.c - all of these expect the caller to hold the shared lock */
#define IPV_ALLOW    0
#define IPV_DENY     1      /* static deny rule */
#define IPV_BANNED   2      /* dynamic ban      */
#define IPV_FLOOD    3      /* this connect tripped the flood ban */
int   ipf_check(struct BBSShared *s, ULONG ip, ULONG now);   /* IPV_* */
int   ipf_screen(struct BBSShared *s, ULONG ip, ULONG now);  /* same, without flood counting */
BOOL  ipf_whitelisted(struct BBSShared *s, ULONG ip);
/* reason NULL/"" = keep the reason of an existing ban (re-ban), else "sysop" */
BOOL  ipf_ban(struct BBSShared *s, ULONG ip, ULONG mins, const char *reason);
BOOL  ipf_unban(struct BBSShared *s, ULONG ip);
struct IPBan *ipf_findban(struct BBSShared *s, ULONG ip);
BOOL  ipf_login_failed(struct BBSShared *s, ULONG ip, ULONG now); /* TRUE = now banned */
void  ipf_login_ok(struct BBSShared *s, ULONG ip);
int   ipf_expire(struct BBSShared *s, ULONG now);                 /* # removed */
BOOL  ipf_parse_rule(const char *line, struct IPRule *r);
void  ipf_rule_str(const struct IPRule *r, char *buf);
LONG  ipf_load_rules(struct BBSShared *s, const char *path);
LONG  ipf_load_bans(struct BBSShared *s, const char *path);
BOOL  ipf_save_bans(struct BBSShared *s, const char *path);

/* ---- terminal types / charsets ------------------------------------------ */
#define TT_ASCII   0
#define TT_ANSI    1
#define TT_VT100   2

#define CS_CP437   0        /* IBM PC - what ANSI art is drawn in */
#define CS_UTF8    1
#define CS_LATIN1  2        /* Amiga / ISO-8859-1 */
#define CS_ASCII   3        /* 7-bit, box drawing approximated */

/* ---- user database -------------------------------------------------------- */
#define MAX_MSGAREAS  64
#define MAX_FILEAREAS 32

#define UF_DELETED    0x0001
#define UF_HOTKEYS    0x0002
#define UF_EXPERT     0x0004    /* don't redraw menus */
#define UF_NOPAGE     0x0008
#define UF_TERMSET    0x0010    /* terminal chosen by hand - skip autodetect prompt */
#define UF_LOCKED     0x0020
#define UF_LINEEDIT   0x0040    /* prefers the line editor over the full-screen one */
#define UF_HIDDEN     0x0080    /* never listed in last callers (sysop-set) */
#define UF_NEWUSER    0x0100    /* signed up, waiting for the sysop to validate (validated_level) */

struct UserRec {
    ULONG id;                   /* record number + 1; 0 = unused */
    UWORD flags;
    UBYTE level;                /* security level 0..255 */
    UBYTE termtype;
    UBYTE charset;
    UBYTE cols, rows;
    UBYTE pad;
    char  name[NAMELEN];        /* login name / handle, unique, case-insensitive */
    char  realname[LONGNAME];
    char  location[LONGNAME];
    char  email[64];
    UBYTE salt[16];
    UBYTE pwhash[32];           /* iterated salted SHA-256 */
    ULONG firstcall, lastcall;
    ULONG calls;
    ULONG posts;
    ULONG uploads, downloads;
    ULONG ulkb, dlkb;
    ULONG today;                /* bbs_daynum() the counters below belong to */
    UWORD mins_today;           /* minutes used today */
    UWORD calls_today;
    ULONG lastip;
    ULONG doors;
    ULONG lastread[MAX_MSGAREAS];   /* highest message number read, per area */
    ULONG lastfscan;                /* newest file date seen at last new-files scan */
    ULONG aflags;                   /* access flags A..Z = bits 0..25 (sysop-set) */
    char  group[16];                /* access group name (sysop-set), "" = none */
    LONG  credits;                  /* extra download allowance in KB (sysop-set) */
    UBYTE conf;                     /* conference the user was last in */
    UBYTE proto;                    /* file transfer protocol, PROTO_* (zmodem.h) */
    UBYTE pad2[2];
    char  lang[16];                 /* language file (BBS:Text/Language/<lang>.lng), "" = board default */
    UBYTE reserved[164];
};                                  /* on-disk record: exactly 768 bytes */
#define USERREC_SIZE 768
typedef char userrec_size_check[(sizeof(struct UserRec) == USERREC_SIZE) ? 1 : -1];

/* userdb.c - callers must hold shared->userlock */
extern const char *userdb_path;             /* default BBS_USERS */
LONG userdb_count(void);
BOOL userdb_read(ULONG id, struct UserRec *u);
BOOL userdb_write(struct UserRec *u);               /* u->id must be set */
LONG userdb_find(const char *name, struct UserRec *u); /* id or 0 */
LONG userdb_add(struct UserRec *u);                 /* assigns u->id */
void user_setpass(struct UserRec *u, const char *pw);
BOOL user_checkpass(const struct UserRec *u, const char *pw);

/* sha256.c */
void sha256(const UBYTE *data, ULONG len, UBYTE out[32]);

/* ---- config --------------------------------------------------------------- */
struct Cfg;
struct Cfg *cfg_load(const char *path);
void        cfg_free(struct Cfg *c);
const char *cfg_str(struct Cfg *c, const char *key, const char *def);
LONG        cfg_int(struct Cfg *c, const char *key, LONG def);
BOOL        cfg_bool(struct Cfg *c, const char *key, BOOL def);

/* ---- util ------------------------------------------------------------------ */
void  str_copy(char *dst, const char *src, LONG size);
BOOL  shell_char_ok(int c);                    /* FALSE for ` $ * " < > ; and controls */
void  shell_safe(char *dst, const char *src, LONG size);   /* str_copy, unsafe chars -> _ */
void  str_nopipe(char *s);                     /* | -> ! : no colour codes in user text */
void  upload_name(const char *in, char *out);  /* uploaded file name -> safe local name (out >= 31) */
char *path_join(char *out, const char *dir, const char *name);   /* dir/name - no '/' after a "CD0:" */
int   str_icmp(const char *a, const char *b);
int   str_nicmp(const char *a, const char *b, LONG n);
BOOL  glob_icmp(const char *pat, const char *s);        /* * = anything, any case */
char *str_trim(char *s);
char *str_istr(const char *hay, const char *needle);
LONG  str_split(char *s, char sep, char **fields, LONG max);  /* in place */
void  bbs_log(const char *file, const char *fmt, ...);      /* timestamped line */
BOOL  file_exists(const char *path);
int   run_with_stack(ULONG need, int (*fn)(void));             /* see util.c */
LONG  file_size(const char *path);

/* buffered line reader over a dos.library handle */
struct LineReader {
    BPTR  fh;
    LONG  pos, len;
    UBYTE buf[1024];
};
BOOL lr_open(struct LineReader *lr, const char *path);
LONG lr_gets(struct LineReader *lr, char *line, LONG size);  /* -1 = EOF */
void lr_close(struct LineReader *lr);

/* ---- paths ----------------------------------------------------------------- */
#define BBS_CONFIG      "BBS:Config/NilBBS.cfg"

/* watching a node: the watcher's public port; the node posts copies of its output there (node/spy.c) */
#define SPY_PORTFMT     "NILBBS.WATCH.%d"
struct SpyMsg { struct Message msg; LONG len; UBYTE data[4]; };   /* data: len bytes; FreeVec() when read */
#define BBS_CONFIG_OLD  "BBS:Config/" "NuzBBS.cfg"    /* before the rename (2026-09-26) - still read */
const char *bbs_config(void);                 /* the one that exists (new name first) */
#define BBS_IPRULES     "BBS:Config/IPFilter.cfg"
#define BBS_BANS        "BBS:Data/Bans.dat"
#define BBS_USERS       "BBS:Data/Users.dat"
#define BBS_SYSLOG      "BBS:Logs/System.log"
#define BBS_CALLERLOG   "BBS:Logs/Callers.log"
#define BBS_BANLOG      "BBS:Logs/Bans.log"     /* every ban/unban/expiry, kept for good (never rotated) */
#define BBS_LASTCALLERS "BBS:Data/LastCallers.dat"
#define BBS_ONELINERS   "BBS:Data/OneLiners.dat"

/* ---- one-liners + last callers (lists.c; hold shared->msglock) -------------- */
struct LastCall {
    char  name[NAMELEN];
    char  location[LONGNAME];
    ULONG when;
    UWORD node;
    UBYTE term, pad;
};
#define MAX_LASTCALL 20
#define OL_LINE      128                    /* one "name|date|text" line */
#define OL_MAX       1000                   /* most one-liners loaded at once */
typedef char OneLine[OL_LINE];
LONG oneliners_load(OneLine **out);         /* FreeVec(*out) after */
BOOL oneliners_save(OneLine *lines, LONG n);
LONG oneliners_trim(LONG keep);
LONG oneliners_delete(const UBYTE *del, LONG ndel);   /* del[number] set = delete it */
LONG oneliners_clear(void);
LONG lastcallers_load(struct LastCall *all);          /* all[MAX_LASTCALL] */
BOOL lastcallers_save(struct LastCall *all, LONG n);
LONG lastcallers_remove(const char *name);
LONG lastcallers_clear(void);
BOOL lastcall_hidden(struct Cfg *c, const struct UserRec *u);

#endif
