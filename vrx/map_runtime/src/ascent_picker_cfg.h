/* ascent_picker_cfg.h — deployed Picker config + T2 parse (PURE, host-testable).
 *
 * The deployed device config is a tiny, schema-versioned, line-based text (no JSON parser on
 * the freestanding target). ASCENT Control deploys it; the daemon parses it, reads this VRX's
 * T2 candidate macs from /factory/user_cfg.json, and hands both to a_pctl_set_config -- which
 * computes active-valid + activation. This module only PARSES; it holds no activation/PWM logic.
 *
 * Config format (v1):
 *     ascent_picker_rt v1
 *     hold_ms 120
 *     cell <four-byte-hex-id> <label...>
 *     cell <four-byte-hex-id> <label...>
 *     ...
 * Lines may be \n-terminated; unknown lines are ignored; a missing/garbled header or zero
 * cells => parse fails (caller fails closed => Picker disabled).
 */
#ifndef ASCENT_PICKER_CFG_H
#define ASCENT_PICKER_CFG_H
#include "ascent_types.h"
#include "ascent_cells.h"

#define A_PCFG_MAX_CELLS A_CELLS_MAX_CFG
#define A_PCFG_LABEL     24

typedef struct {
    au8  bbid[4];
    char glyph[A_PCFG_LABEL];
    char label[A_PCFG_LABEL];   /* NUL-terminated, for the OSD bar */
} a_pcfg_cell;

typedef struct {
    a_pcfg_cell cells[A_PCFG_MAX_CELLS];
    int  n;
    au32 hold_ms;               /* debounce; default 120 if absent */
    au32 cooldown_ms;           /* advisory after success; 0 disables */
    au32 iconscale;
    int theme;
    char iconstyle[A_PCFG_LABEL];
    int rc_enabled;
    int display_hidden;        /* presentation only; never disables switching */
    int  rc_channel;            /* 1-based RC channel for headless selection; default 5 */
    int  ok;                    /* 0 => malformed, fail closed */
} a_pcfg;

/* Parse one BbId "<four-byte-hex-id>" (uppercase or lower) into 4 bytes. 1 on success. Rejects the
 * two sentinels (all-00 / all-FF). */
int a_pcfg_bbid(const char *s, au8 out[4]);

/* Parse the deployed config text. ok=0 and n=0 on any malformation (fail closed). */
void a_pcfg_parse(const char *text, ausize len, a_pcfg *out);

/* Extract T2 candidate macs from a /factory/user_cfg.json buffer: every bb_mac_addr* array of
 * four "0xNN" entries becomes one mac. Returns the count (<= cap). Sentinels dropped. */
int a_pcfg_parse_t2(const char *json, ausize len, au8 (*macs)[4], int cap);

#endif
