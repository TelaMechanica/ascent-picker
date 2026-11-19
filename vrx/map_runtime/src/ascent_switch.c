/* ascent_switch.c — the one VRX-local Picker switch engine. See ascent_switch.h. */
#include "ascent_switch.h"

/* ---------- frame builders (byte-exact ports of the proven host builders) ---------- */

au8 a_sw_checksum17(const au8 *h)
{
    au8 x = 0;
    for (int i = 0; i < 17; i++) x ^= h[i];
    return (au8)(~x & 0xFF);        /* caddx_relay_qos._checksum17 */
}

/* caddx_relay_qos.build_frame: 18B header + payload + 0xBB.
   [0]=0xAA [1:5]=len(payload) LE [5:9]=cmd BE [9:13]=meta1 BE [13:17]=meta2 BE [17]=cksum17 */
int a_usbpack_build(au32 cmd, au32 meta1, au32 meta2,
                    const au8 *payload, au32 paylen, au8 *out, au32 outcap)
{
    au32 total = A_SW_USBPACK_HDR + paylen + 1;
    if (outcap < total) return -1;
    au8 *h = out;
    h[0] = 0xAA;
    h[1] = (au8)(paylen);       h[2] = (au8)(paylen >> 8);
    h[3] = (au8)(paylen >> 16); h[4] = (au8)(paylen >> 24);          /* len LE */
    h[5] = (au8)(cmd >> 24);  h[6] = (au8)(cmd >> 16);  h[7] = (au8)(cmd >> 8);  h[8] = (au8)cmd;
    h[9] = (au8)(meta1 >> 24); h[10]=(au8)(meta1 >> 16); h[11]=(au8)(meta1 >> 8); h[12]=(au8)meta1;
    h[13]=(au8)(meta2 >> 24); h[14]=(au8)(meta2 >> 16); h[15]=(au8)(meta2 >> 8); h[16]=(au8)meta2;
    h[17] = a_sw_checksum17(h);
    if (paylen) a_memcpy(out + A_SW_USBPACK_HDR, payload, paylen);
    out[A_SW_USBPACK_HDR + paylen] = 0xBB;
    return (int)total;
}

/* candidate_set_frame: payload = 402B [flags=0][count][macs 4B...], cmd 0x02000004. */
int a_sw_candidate_frame(const au8 macs[][4], int count, au8 *out, au32 outcap)
{
    if (count < 0) count = 0;
    if (count > A_SW_CANDIDATE_MAX) count = A_SW_CANDIDATE_MAX;
    au8 body[A_SW_CANDIDATE_PAYLEN];
    a_memset(body, 0, sizeof(body));
    body[0] = 0x00;                      /* flags (unused) */
    body[1] = (au8)(count & 0xFF);       /* count */
    for (int i = 0; i < count; i++) a_memcpy(body + 2 + i * 4, macs[i], 4);
    return a_usbpack_build(A_SW_CMD_SET_CANDIDATE, 0, 0, body, sizeof(body), out, outcap);
}

/* pair re-arm: 0x02000002, 14B payload. ENTER = 01 01 + 12x00, EXIT = 00 01 + 12x00. */
int a_sw_pair_frame(int enter, au8 *out, au32 outcap)
{
    au8 body[14];
    a_memset(body, 0, sizeof(body));
    body[0] = (au8)(enter ? 0x01 : 0x00);
    body[1] = 0x01;
    return a_usbpack_build(A_SW_CMD_PAIR_MODE, 0, 0, body, sizeof(body), out, outcap);
}

/* STATUS read: 0x01000000, 2B payload 00 00, meta1 = work_id (matches _rpc). */
int a_sw_status_frame(au32 work_id, au8 *out, au32 outcap)
{
    au8 body[2] = {0, 0};
    return a_usbpack_build(A_SW_CMD_GET_STATUS, work_id, 0, body, sizeof(body), out, outcap);
}

/* standby_frame: 8B header + [cmd=56][mode]. See caddx_picker.standby_frame. */
int a_sw_standby_frame(int mode, int sub_chn, int seq, au8 *out, au32 outcap)
{
    if (mode != 0 && mode != 1) return -1;
    if (outcap < 10) return -1;
    au8 pay[2] = {(au8)A_SW_GUI_SET_STANDBY, (au8)mode};
    out[0] = 0x8E; out[1] = 0x25;
    out[2] = 0x00;                     /* checksum, filled below */
    out[3] = (au8)(seq & 0xFF);
    out[4] = (au8)(sub_chn & 0xFF);
    out[5] = 0x00;                     /* flags */
    out[6] = (au8)(sizeof(pay) & 0xFF);
    out[7] = (au8)((sizeof(pay) >> 8) & 0xFF);
    out[8] = pay[0]; out[9] = pay[1];
    au8 cks = 0;                        /* xor over bytes [3 .. 8+len) */
    for (int i = 3; i < 8 + (int)sizeof(pay); i++) cks ^= out[i];
    out[2] = cks;
    return 10;
}

/* ---------------------------- the state machine ---------------------------- */

int a_sw_target_valid(const au8 mac[4])
{
    int zero = 1, ff = 1;
    for (int i = 0; i < 4; i++) { if (mac[i] != 0x00) zero = 0; if (mac[i] != 0xFF) ff = 0; }
    return !zero && !ff;
}

/* One-shot recovery (invariant #5): re-widen T1 to the full whitelist, then restore power to
   whatever is still linked (standby-OFF). Never looped. Best-effort: a transport error here is
   recorded but does not change the already-decided outcome. */
static void recover_once(const a_sw_cfg *cfg, const a_sw_ops *ops, a_sw_result *r)
{
    if (r->recovered) return;
    r->recovered = 1;
    int ok = cfg->whitelist_n > 0;
    if (cfg->whitelist_n > 0 && ops->set_candidates(ops->ctx, cfg->whitelist, cfg->whitelist_n) < 0)
        ok = 0;
    if (cfg->park_enabled && r->standby_on_sent && !r->standby_off_sent) {
        if (ops->send_standby(ops->ctx, 0) == 0) r->standby_off_sent = 1;
        else ok = 0;
    }
    r->recovery_ok = ok;
}

/* Poll the link until it leaves CONNECT (the measured drop) within drop_wait_ms.
   Returns 1 if a drop was observed, 0 if CONNECT persisted the whole budget (NO_DROP),
   <0 on transport error. */
static int await_drop(const a_sw_cfg *cfg, const a_sw_ops *ops, a_sw_result *r)
{
    au64 start = ops->now_ms(ops->ctx);
    for (;;) {
        int st = -1;
        if (ops->read_link(ops->ctx, &st) < 0) return -1;
        r->polls++;
        if (st != A_SW_LINK_CONNECT) return 1;            /* dropped */
        if (ops->now_ms(ops->ctx) - start >= cfg->drop_wait_ms) return 0;  /* NO_DROP */
        ops->sleep_ms(ops->ctx, cfg->poll_ms);
    }
}

/* Poll until a CONNECT has held continuously for connect_stable_ms, within acquire_window_ms.
   Returns 1 acquired (fills r->acquire_ms), 0 window elapsed, <0 transport error. */
static int await_connect(const a_sw_cfg *cfg, const a_sw_ops *ops, a_sw_result *r)
{
    au64 start = ops->now_ms(ops->ctx);
    au64 connect_since = 0;
    int  holding = 0;
    for (;;) {
        int st = -1;
        if (ops->read_link(ops->ctx, &st) < 0) return -1;
        r->polls++;
        au64 now = ops->now_ms(ops->ctx);
        if (st == A_SW_LINK_CONNECT) {
            if (!holding) { holding = 1; connect_since = now; }
            if (now - connect_since >= cfg->connect_stable_ms) {
                r->acquire_ms = (au32)(now - start);
                return 1;
            }
        } else {
            holding = 0;                                   /* reset the dwell */
        }
        if (now - start >= cfg->acquire_window_ms) return 0;
        ops->sleep_ms(ops->ctx, cfg->poll_ms);
    }
}

a_sw_outcome a_switch_execute(const a_sw_cfg *cfg, const a_sw_ops *ops, a_sw_result *result)
{
    a_sw_result local;
    a_sw_result *r = result ? result : &local;
    a_memset(r, 0, sizeof(*r));
    r->phase = A_SW_IDLE;
    r->outcome = A_SW_OUT_PENDING;

    if (!cfg || !ops) { r->outcome = A_SW_OUT_REFUSED; r->phase = A_SW_FAILED; return r->outcome; }
    if (!a_sw_target_valid(cfg->target)) {           /* no RF issued for a bad target */
        r->outcome = A_SW_OUT_REFUSED; r->phase = A_SW_FAILED; return r->outcome;
    }

    r->phase = A_SW_COMMAND;

    /* S2 PARK leaving (standby-ON) BEFORE the narrow. */
    if (cfg->park_enabled) {
        if (ops->send_standby(ops->ctx, 1) < 0) {
            r->outcome = A_SW_OUT_EXEC_ERR; r->phase = A_SW_FAILED;
            /* nothing narrowed yet; best-effort un-park */
            (void)ops->send_standby(ops->ctx, 0);
            return r->outcome;
        }
        r->standby_on_sent = 1;
    }

    /* S3 RELEASE+SELECT: narrow T1 to the target only (count=1). */
    r->phase = A_SW_RELEASING;
    {
        au8 one[1][4];
        a_memcpy(one[0], cfg->target, 4);
        if (ops->set_candidates(ops->ctx, one, 1) < 0) {
            r->outcome = A_SW_OUT_EXEC_ERR; r->phase = A_SW_FAILED;
            recover_once(cfg, ops, r);
            return r->outcome;
        }
    }

    /* S4 MEASURED DROP GATE (invariant #1). */
    {
        int d = await_drop(cfg, ops, r);
        if (d < 0) {
            r->outcome = A_SW_OUT_EXEC_ERR; r->phase = A_SW_FAILED;
            recover_once(cfg, ops, r); return r->outcome;
        }
        r->dropped = d;
        if (!d) {                               /* NO_DROP: never continue to re-arm/acquire */
            r->outcome = A_SW_OUT_NO_DROP; r->phase = A_SW_FAILED;
            recover_once(cfg, ops, r); return r->outcome;
        }
    }

    /* S5-S7 transient re-arm, ISOLATED behind one op + flag (invariant #2). */
    r->phase = A_SW_REARM;
    if (cfg->rearm_enabled && ops->pair_rearm) {
        if (ops->pair_rearm(ops->ctx) < 0) {
            r->outcome = A_SW_OUT_EXEC_ERR; r->phase = A_SW_FAILED;
            recover_once(cfg, ops, r); return r->outcome;
        }
        r->rearmed = 1;
    }

    /* S8 ACQUIRE to stable CONNECT (the identity proof on the narrowed T1). */
    r->phase = A_SW_ACQUIRING;
    {
        int a = await_connect(cfg, ops, r);
        if (a < 0) {
            r->outcome = A_SW_OUT_EXEC_ERR; r->phase = A_SW_FAILED;
            recover_once(cfg, ops, r); return r->outcome;
        }
        if (!a) {                               /* no stable CONNECT within the window */
            r->outcome = A_SW_OUT_NO_REACQUIRE; r->phase = A_SW_FAILED;
            recover_once(cfg, ops, r); return r->outcome;   /* one-shot widen + restore */
        }
    }

    /* S10 UN-PARK joined (standby-OFF) ONLY after verified CONNECT (invariant #4). */
    r->phase = A_SW_VERIFIED;
    if (cfg->park_enabled) {
        if (ops->send_standby(ops->ctx, 0) == 0) r->standby_off_sent = 1;
        /* a failed un-park does not fail the switch; the link is verified. */
    }
    r->outcome = A_SW_OUT_SWITCHED;
    return r->outcome;
}
