/*
 * cfg.c - tiny INI-style config reader.
 *
 *   ; comment            # comment
 *   key = value
 *   [SECTION]
 *   key = value          -> looked up as "SECTION.key"
 *
 * Keys are case-insensitive.  Values keep their inner spaces; surrounding
 * double quotes are stripped.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdlib.h>

#include "bbs.h"
#include "cfg.h"

struct CfgEnt {
    struct CfgEnt *next;
    char *key;
    char *val;
};

struct Cfg {
    struct CfgEnt *first, *last;
    LONG nsect;
    char *sect[64];
};

static char *dupstr(const char *s)
{
    LONG n = strlen(s) + 1;
    char *d = AllocVec(n, MEMF_ANY);
    if (d) memcpy(d, s, n);
    return d;
}

struct Cfg *cfg_load(const char *path)
{
    struct LineReader *lr;
    struct Cfg *c;
    char line[LINELEN], section[NAMELEN], key[NAMELEN * 2 + 2];

    if (!(c = AllocVec(sizeof(*c), MEMF_CLEAR))) return NULL;
    if (!(lr = AllocVec(sizeof(*lr), MEMF_ANY))) { FreeVec(c); return NULL; }
    section[0] = 0;

    if (!lr_open(lr, path)) { FreeVec(lr); return c; }  /* empty config is fine */

    while (lr_gets(lr, line, sizeof(line)) >= 0) {
        char *p = str_trim(line), *eq, *v;
        struct CfgEnt *e;
        if (!*p || *p == ';' || *p == '#') continue;
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end) *end = 0;
            str_copy(section, str_trim(p + 1), sizeof(section));
            if (c->nsect < 64) c->sect[c->nsect++] = dupstr(section);
            continue;
        }
        if (!(eq = strchr(p, '='))) continue;
        *eq = 0;
        v = str_trim(eq + 1);
        /* inline comment: whitespace then ';' (outside double quotes) */
        {
            char *q;
            BOOL inq = FALSE;
            for (q = v; *q; q++) {
                if (*q == '"') inq = !inq;
                else if (!inq && *q == ';' && (q == v || q[-1] == ' ' || q[-1] == '\t')) {
                    *q = 0;
                    v = str_trim(v);
                    break;
                }
            }
        }
        p = str_trim(p);
        if (*v == '"') {
            LONG n = strlen(v);
            if (n >= 2 && v[n - 1] == '"') { v[n - 1] = 0; v++; }
        }
        if (section[0]) {
            str_copy(key, section, sizeof(key));
            strcat(key, ".");
            strncat(key, p, sizeof(key) - strlen(key) - 1);
        } else {
            str_copy(key, p, sizeof(key));
        }
        if (!(e = AllocVec(sizeof(*e), MEMF_CLEAR))) break;
        e->key = dupstr(key);
        e->val = dupstr(v);
        if (!e->key || !e->val) break;
        if (c->last) c->last->next = e; else c->first = e;
        c->last = e;
    }
    lr_close(lr);
    FreeVec(lr);
    return c;
}

void cfg_free(struct Cfg *c)
{
    struct CfgEnt *e, *n;
    LONG i;
    if (!c) return;
    for (e = c->first; e; e = n) {
        n = e->next;
        if (e->key) FreeVec(e->key);
        if (e->val) FreeVec(e->val);
        FreeVec(e);
    }
    for (i = 0; i < c->nsect; i++) if (c->sect[i]) FreeVec(c->sect[i]);
    FreeVec(c);
}

const char *cfg_str(struct Cfg *c, const char *key, const char *def)
{
    struct CfgEnt *e;
    const char *hit = NULL;
    if (!c) return def;
    for (e = c->first; e; e = e->next)          /* last one wins */
        if (!str_icmp(e->key, key)) hit = e->val;
    return hit ? hit : def;
}

LONG cfg_int(struct Cfg *c, const char *key, LONG def)
{
    const char *s = cfg_str(c, key, NULL);
    if (!s || !*s) return def;
    return strtol(s, NULL, 10);
}

BOOL cfg_bool(struct Cfg *c, const char *key, BOOL def)
{
    const char *s = cfg_str(c, key, NULL);
    if (!s || !*s) return def;
    return (*s == 'y' || *s == 'Y' || *s == '1' || *s == 't' || *s == 'T' ||
            !str_icmp(s, "on"));
}

LONG cfg_sections(struct Cfg *c) { return c ? c->nsect : 0; }
const char *cfg_section(struct Cfg *c, LONG i)
{
    return (c && i >= 0 && i < c->nsect) ? c->sect[i] : NULL;
}

/* section-relative lookup: cfg_sget(c, "LORD", "command", "") */
const char *cfg_sget(struct Cfg *c, const char *sect, const char *key, const char *def)
{
    char k[NAMELEN * 2 + 2];
    str_copy(k, sect, sizeof(k));
    strcat(k, ".");
    strncat(k, key, sizeof(k) - strlen(k) - 1);
    return cfg_str(c, k, def);
}

LONG cfg_sint(struct Cfg *c, const char *sect, const char *key, LONG def)
{
    const char *s = cfg_sget(c, sect, key, NULL);
    if (!s || !*s) return def;
    return strtol(s, NULL, 10);
}

BOOL cfg_sbool(struct Cfg *c, const char *sect, const char *key, BOOL def)
{
    const char *s = cfg_sget(c, sect, key, NULL);
    if (!s || !*s) return def;
    return (*s == 'y' || *s == 'Y' || *s == '1' || *s == 't' || *s == 'T' ||
            !str_icmp(s, "on"));
}
