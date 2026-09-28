/*
 * node.h - BBSNode internals.  One BBSNode process serves one caller, so the
 * node's state is a single global context, N.
 */
#ifndef NILBBS_NODE_H
#define NILBBS_NODE_H

#include <exec/types.h>
#include "../common/bbs.h"
#include "../common/cfg.h"
#include "../common/lang.h"

/* ---- keys returned by tgetkey() ---------------------------------------- */
#define KEY_NONE    (-1)
#define KEY_HANGUP  (-2)        /* carrier lost / kicked / time up */
#define KEY_TIMEOUT (-3)
#define KEY_UP      0x101
#define KEY_DOWN    0x102
#define KEY_RIGHT   0x103
#define KEY_LEFT    0x104
#define KEY_HOME    0x105
#define KEY_END     0x106
#define KEY_PGUP    0x107
#define KEY_PGDN    0x108
#define KEY_INS     0x109
#define KEY_DEL     0x10A

/* tgetline() flags */
#define GL_UPPER    0x01        /* force upper case */
#define GL_MASK     0x02        /* echo '*' (passwords) */
#define GL_DIGITS   0x04        /* digits only */
#define GL_NAME     0x08        /* capitalise words, no leading spaces */
#define GL_EDIT     0x10        /* start from buf's current contents */
#define GL_NOCR     0x20        /* don't print a newline when done */

#define INBUF_SIZE  2048
#define OUTBUF_SIZE 4096

struct NodeCtx {
    int    node;                /* 1-based node number */
    LONG   sock;
    BOOL   local;               /* a console session (BBSNode LOCAL), no socket */
    BPTR   lcon;                /* ... its CON: window, raw mode */
    struct BBSShared *S;
    struct NodeInfo  *ni;
    struct Cfg *cfg;
    BOOL   online;              /* FALSE once the caller is gone */
    BOOL   kicked;
    ULONG  ip;
    char   ipstr[16];

    /* telnet protocol */
    UBYTE  tstate, sbopt;
    UWORD  sblen;
    UBYTE  sbbuf[80];
    UBYTE  cr_last;             /* previous data byte was CR (collapse CR LF / CR NUL) */
    UBYTE  remote_naws, remote_ttype, remote_binary, local_binary;
    UBYTE  ttype_rounds;
    char   ttype[32];           /* first terminal type the client reported */
    char   ttypes[96];          /* every name it cycled through, space-separated */

    /* decoded input bytes (telnet stripped) */
    UBYTE  in[INBUF_SIZE];
    UWORD  in_head, in_tail;

    /* output */
    UBYTE  out[OUTBUF_SIZE];
    UWORD  outlen;
    UBYTE  binary_raw;          /* TRUE while a file transfer owns the stream */
    UBYTE  wait_new;            /* tn_wait() ignores already-buffered input */

    /* terminal */
    UBYTE  term;                /* TT_* */
    UBYTE  charset;             /* CS_* */
    UWORD  cols, rows;
    UBYTE  fg, bg;              /* current pipe-code colour */
    UBYTE  dec_gfx;             /* VT100: DEC line drawing set is active */
    /* ANSI output filter state */
    UBYTE  esc_state;
    UBYTE  esc_len;
    char   esc_buf[40];
    UBYTE  utf8_pending;        /* bytes still expected for a UTF-8 sequence */
    UBYTE  door_csi8;           /* current door emits Amiga 0x9B CSI */

    /* more-prompt paging */
    BOOL   paging;
    BOOL   page_abort;
    UWORD  lines_out;

    /* session */
    struct UserRec user;
    BOOL   loggedin;
    BOOL   sysop;               /* level >= sysop_level */
    ULONG  logon;               /* bbs_now() at login */
    ULONG  prevcall;            /* the user's previous call (0 = first call) */
    LONG   limit_mins;          /* 0 = unlimited */
    ULONG  last_input;
    UWORD  idle_warned;
    BOOL   msg_waiting;         /* CTRL-D arrived: node message queued */
    const char *reprompt;       /* redrawn after node messages interrupt a menu prompt */
    UBYTE  cur_msgarea;         /* index into the message area table */
    UBYTE  cur_filearea;
};

extern struct NodeCtx N;

/* telnet.c */
void tn_start(void);
LONG tn_wait(ULONG ms, ULONG extrasigs, ULONG *gotsigs);  /* >0 = input available */
BOOL tn_flush(void);
void tn_raw(const UBYTE *buf, LONG len);          /* queue bytes, IAC-escaped */
void tn_rawflush(const UBYTE *buf, LONG len);     /* queue and flush */
LONG in_avail(void);
LONG in_get(void);                                 /* -1 if empty */
void in_unget(UBYTE c);
void tn_set_binary(BOOL on);
void node_heartbeat(void);
void node_hangup(const char *why);
void spy_feed(const UBYTE *p, LONG len);          /* spy.c */
void spy_poll(void);                                /* spy.c: history to a new watcher while idle */
BOOL node_reset_wanted(void);   /* the sysop asked for a RESET of this node */
void node_release_slot(void);   /* give the slot back now; this process carries on outside it */
void node_check_slot(void);     /* drop the slot pointer if the daemon took the slot back */

/* charset.c */
UWORD cp437_to_uni(UBYTE c);
UBYTE uni_to_cp437(UWORD u);
void  emit_char(UBYTE c, UBYTE srccs);    /* translate one printable byte */

/* term.c */
void  tputraw(const UBYTE *buf, LONG len, UBYTE srccs);  /* ANSI-filtered + charset */
void  tputs(const char *s);                 /* pipe/MCI codes, \n -> CRLF */
void  tprintf(const char *fmt, ...);
void  tnl(void);
void  tcls(void);
void  tcolor(int fg);
void  tgotoxy(int x, int y);
void  tcleol(void);
LONG  tgetkey(LONG timeout_secs);           /* 0 = no timeout */
LONG  tgetline(char *buf, LONG max, UWORD flags);
LONG  tgethot(const char *valid);           /* returns the (upper-case) key */
BOOL  tyesno(const char *prompt, BOOL def);
void  tpause(void);
BOOL  tmore(void);                          /* call per line when paging: FALSE = user quit */
void  tpage_start(void);
void  tpage_end(void);
BOOL  tshowfile(const char *name);          /* Text/<name>.ans|.asc|.txt */
BOOL  tshowpath(const char *path, UBYTE srccs);
void  tdetect(void);
void  term_choose(void);
const char *term_name(UBYTE t);
const char *charset_name(UBYTE c);
void  expand_mci(const char *code, char *out);
void  tcheck_messages(void);

/* login.c */
BOOL  do_login(void);
void  do_logoff(void);
LONG  time_left_mins(void);                 /* -1 = unlimited */
void  user_save(void);
void  user_refresh(void);

/* menu.c */
void  menu_run(const char *start);

/* door.c */
void  door_run(const char *tag);
void  door_list(void);
ULONG door_launch_sync(const char *cmd, const char *dir, LONG stack);
BOOL  door_launch_done(void);
LONG  door_launch_finish(void);
void  cnet_mci_write(const UBYTE *s, LONG len, UBYTE srccs);

/* cnetc.c */
void  run_cnetc(const char *tag, const char *cmd, const char *dir, LONG stack,
                UBYTE ack, UBYTE srccs, LONG grace);

/* misc.c */
void  oneliners(void);
void  lastcallers_show(void);
void  lastcallers_add(void);
void  whos_online(void);
void  userlist(void);
void  page_node(void);
void  sysinfo(void);
void  user_settings(void);
void  lang_board(void);                     /* load the board's default language */
void  lang_user(void);                      /* load the caller's own (N.user.lang) */
BOOL  lang_pick(char *out);                 /* numbered pick list; out = LANG_NAMELEN */
void  set_activity(const char *act);
void  set_menu_activity(const char *title, BOOL with_area);  /* the menu we're in (with the file area?) */
void  back_to_menu(void);                                    /* a command is done: say the menu again */
const char *file_area_name(void);                            /* fileui.c */
void  node_msg_poll(void);

/* acs.c - access condition strings and conferences */
#define MAX_CONFS 16
BOOL  acs_check(const char *acs);
void  conf_load(void);
int   conf_count(void);
int   conf_find(const char *tag);
const char *conf_name(void);
const char *conf_tag(void);
BOOL  conf_visible(const char *conf);
void  conf_join(const char *arg);

/* community.c */
void  bulletins(void);
int   bulletins_new(void);
void  voting_booth(void);
int   voting_waiting(void);
void  finger(void);
void  plan_edit(void);
void  plan_path(ULONG id, char *buf);

/* qwk.c */
void  qwk_download(void);
void  qwk_upload(void);

/* tele.c */
void  teleconference(const char *arg);
void  page_sysop(void);
void  sysop_chat(void);
void  caller_chat(void);                     /* tele.c: invite a caller to a private chat */
BOOL  tele_chat_request(struct NodeMsg *m);

/* sysop.c */
void  sysop_menu(void);

/* a message being written (msgui.c line editor, fse.c full-screen editor) */
#define ED_MAXLINES 200
#define ED_WIDTH    76
struct Editor {
    char line[ED_MAXLINES][ED_WIDTH + 2];
    int  n;
};
int   ed_quote(struct Editor *ed, int at, const char *quote, const char *qfrom);

/* fse.c: 1 = save, 0 = abort, -1 = hung up */
int   fse_edit(struct Editor *ed, const char *quote, const char *qfrom, const char *to, const char *subj);

/* msgui.c */
void  msg_select_area(void);
void  msg_read_area(BOOL newonly);
void  msg_post(const char *to, const char *subj, ULONG replyto);
void  msg_scan_all(void);
void  msg_email(void);
void  msg_areas_load(void);

/* fileui.c */
void  file_select_area(void);
void  file_list(BOOL newonly);
void  file_download(void);
void  file_upload(void);
void  file_search(void);
void  file_areas_load(void);
void  file_sysop_diz(void);
void  file_subop(void);
void  file_review_uploads(void);           /* sysop: keep / move / delete new uploads */
void  file_stats(void);

/* diz.c (the DIZ machinery itself is common/dizcore.c) */
#include "../common/dizcore.h"
BOOL  diz_create_interactive(const char *filename, const char *areaname, char *out, LONG max);

#endif
