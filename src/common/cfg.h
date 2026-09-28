#ifndef NILBBS_CFG_H
#define NILBBS_CFG_H
/* section helpers for cfg.c (the flat API is in bbs.h) */
LONG        cfg_sections(struct Cfg *c);
const char *cfg_section(struct Cfg *c, LONG i);
const char *cfg_sget(struct Cfg *c, const char *sect, const char *key, const char *def);
LONG        cfg_sint(struct Cfg *c, const char *sect, const char *key, LONG def);
BOOL        cfg_sbool(struct Cfg *c, const char *sect, const char *key, BOOL def);
#endif
