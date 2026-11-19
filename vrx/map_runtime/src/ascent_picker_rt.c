/* ascent_picker_rt.c — the headless VRX Picker daemon entry.
 *
 * Thin orchestration ONLY: it loads the deployed config + this VRX's T2, binds the proven
 * switch engine to the real loopback transports, then loops {poll RC -> controller, service the
 * IPC command/status socket, write the OSD bar}. ALL activation/validity/PWM/switching decisions
 * come from the proven core (ascent_cells + switch engine + controller); this file holds none of
 * that logic. Both the RC path and a manual IPC SELECT feed the ONE controller/engine.
 *
 * BENCH-GATED: the live socket behaviour (50000/2767), the IPC server, the ttyS2 0x16 presence,
 * reboot survival and GlassesUI OSD compositing are confirmed on hardware, not here. The pure
 * parts it calls (cfg/ipc/osd/cells/engine/controller) are host-unit-tested.
 */
#include "ascent_sys.h"
#include "ascent_picker_ctl.h"
#include "ascent_picker_cfg.h"
#include "ascent_picker_ipc.h"
#include "ascent_picker_osd.h"
#include "ascent_rc_source.h"
#include "ascent_sw_rpc.h"

#ifndef CONFIG_PATH
#define CONFIG_PATH "/usrdata/ascent/widgets/picker/config.txt"
#endif
#ifndef USERCFG_PATH
#define USERCFG_PATH "/factory/user_cfg.json"
#endif
#ifndef RX_RING_PATH
#define RX_RING_PATH "/tmp/ascent_ttyS2_rx.ring"
#endif
#ifndef OSD_PATH
#define OSD_PATH "/tmp/ascent_picker_bar"
#endif
#ifndef A_PICKER_IPC_PORT
#define A_PICKER_IPC_PORT 50126
#endif
#define OSD_BUF 8192          /* matches A_PBAR_MAX_FILE; local to avoid the render include chain */

/* commit context: run the proven engine with a per-switch a_sw_cfg built from the controller's
   current target + active-valid whitelist. */
typedef struct {
    a_pctl    *ctl;
    a_sw_ops   rpc;
    a_sw_rpc_ctx rpc_ctx;
    int park_enabled, rearm_enabled;
} commit_ctx;

static int do_commit(void *vc, const au8 target[4], a_sw_result *out)
{
    commit_ctx *cc = (commit_ctx *)vc;
    a_sw_cfg sc; a_memset(&sc, 0, sizeof sc);
    a_memcpy(sc.target, target, 4);
    /* Recovery widens once to this receiver's full T2, never a selector. */
    sc.whitelist_n = cc->ctl->recovery_n;
    a_memcpy(sc.whitelist, cc->ctl->recovery, (ausize)sc.whitelist_n*4);
    sc.park_enabled = cc->park_enabled;
    sc.rearm_enabled = cc->rearm_enabled;
    sc.drop_wait_ms = 1500; sc.connect_stable_ms = 2000;
    sc.acquire_window_ms = A_SW_ACQUIRE_WINDOW_MS; sc.poll_ms = 50; sc.work_id = cc->rpc_ctx.work_id;
    a_switch_execute(&sc, &cc->rpc, out);
    /* Reject a waiting second SELECT before the controller releases busy. */
    if (cc->rpc_ctx.service) cc->rpc_ctx.service(cc->rpc_ctx.service_ctx);
    return 0;   /* an outcome (incl. no_reacquire) is a result, not a transport failure */
}

/* --- small file read (bounded) --- */
static int read_file(const char *path, char *buf, int cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    assize n = a_read_full(fd, buf, (ausize)(cap - 1));
    close(fd);
    if (n < 0) n = 0;
    if (n == cap-1) return -1; /* truncated config/T2 fails closed */
    buf[n] = 0;
    return (int)n;
}

/* --- load/reload config + T2 into the controller (fail closed) --- */
static void load_config(a_pctl *ctl, a_pcfg *cfg)
{
    static char ctext[8192], jtext[16384];
    int cl = read_file(CONFIG_PATH, ctext, sizeof ctext);
    static au64 identity; static int loaded;
    au64 next = 1469598103934665603ull;
    for (int i=0; i<cl; i++) next = (next ^ (au8)ctext[i]) * 1099511628211ull;
    a_pcfg_parse(cl > 0 ? ctext : "", cl > 0 ? (ausize)cl : 0, cfg);

    au8 t2[A_SW_CANDIDATE_MAX][4]; int t2n = 0;
    int jl = read_file(USERCFG_PATH, jtext, sizeof jtext);
    if (jl > 0) t2n = a_pcfg_parse_t2(jtext, (ausize)jl, t2, A_SW_CANDIDATE_MAX);

    for (int i=0; i<jl; i++) next = (next ^ (au8)jtext[i]) * 1099511628211ull;
    if (loaded && identity == next) return;
    loaded = 1; identity = next;
    if (!cfg->ok) { a_pctl_set_config(ctl, 0, 0, 0, 0); return; }  /* malformed => disabled */
    au8 bbids[A_PCFG_MAX_CELLS][4];
    for (int i = 0; i < cfg->n; i++) a_memcpy(bbids[i], cfg->cells[i].bbid, 4);
    a_pctl_set_config(ctl, bbids, cfg->n, t2, t2n);
    if (ctl->hold_stable_ms != cfg->hold_ms || ctl->rc_enabled != cfg->rc_enabled || ctl->rc_channel != cfg->rc_channel) {
        ctl->pending_cell = A_CELL_INVALID;
        /* Changing RC input must establish a new stable position without RF. */
        ctl->cur_cell = A_CELL_INVALID;
    }
    if(ctl->cooldown_ms!=cfg->cooldown_ms) {ctl->cooldown_until_ms=0;ctl->cooldown_remaining_ms=0;}
    ctl->cooldown_ms = cfg->cooldown_ms;
    ctl->hold_stable_ms = cfg->hold_ms;
    ctl->rc_enabled = cfg->rc_enabled;
    ctl->rc_channel = cfg->rc_channel;
}

static void write_osd(a_pctl *ctl, const a_pcfg *cfg)
{
    a_pctl_update_time(ctl,a_now_ms());
    static char osd[OSD_BUF];
    /* The renderer hides this state if the RF owner stops refreshing it. */
    const char *hex="0123456789ABCDEF", *prefix="owner_tick "; int n=0;
    while(*prefix) osd[n++]=*prefix++;
    au32 tick=(au32)a_now_ms();
    for(int shift=28;shift>=0;shift-=4) osd[n++]=hex[(tick>>shift)&15];
    osd[n++]='\n';
    n += a_posd_format(ctl, cfg, osd+n, (int)sizeof osd-n);
    int fd = open(OSD_PATH ".new", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if(write(fd, osd, (ausize)n)==n) rename(OSD_PATH ".new", OSD_PATH);
    close(fd);
    char status[512]; int sn = a_pipc_format_status(ctl, status, sizeof status);
    fd = open("/tmp/ascent_picker_status.new", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { if(write(fd,status,(ausize)sn)==sn) rename("/tmp/ascent_picker_status.new","/tmp/ascent_picker_status"); close(fd); }
}

struct a_in { au16 fam; au16 port; au32 addr; au8 z[8]; };
static int ipc_listen(void)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct a_in a; a_memset(&a, 0, sizeof a);
    a.fam = AF_INET;
    a.port = (au16)(((A_PICKER_IPC_PORT & 0xFF) << 8) | ((A_PICKER_IPC_PORT >> 8) & 0xFF));
    a.addr = 0x0100007Fu;                     /* 127.0.0.1 */
    if (bind(fd, &a, sizeof a) != 0) { close(fd); return -1; }
    if (listen(fd, 4) != 0) { close(fd); return -1; }
    return fd;
}

static void serve_ipc(int lfd, a_pctl *ctl, const a_pctl_ops *ops)
{
    int cfd = accept(lfd, 0, 0);
    if (cfd < 0) return;                        /* nothing pending */
    struct timeval tv = {0, 200000};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char line[256]; int r = 0;
    au64 deadline = a_now_ms() + 200;
    while (r < (int)sizeof line - 1 && a_now_ms() < deadline) {
        struct pollfd p={cfd,POLLIN,0};
        au64 now=a_now_ms();
        if(now>=deadline || poll(&p,1,(int)(deadline-now))<=0) break;
        assize n = recv(cfd, line+r, 1, MSG_DONTWAIT);
        if (n <= 0) break;
        if (line[r++] == '\n') break;
    }
    if (r > 0) {
        line[r] = 0;
        a_pipc_cmd cmd; a_sw_result res;
        const char *result = "refused";
        if (line[r-1] == '\n' && a_pipc_parse(line, (ausize)r, &cmd)) {
            if (cmd.kind == A_PIPC_STATUS) result = "status";
            else if (ctl->busy) result = "busy";
            else if (cmd.kind == A_PIPC_QUIESCE) {
                result=a_pctl_quiesce(ctl) ? "quiesced" : "busy";
                if(ctl->quiesced) write_osd(ctl,0);
            }
            else if (ctl->quiesced) result="refused";
            else {
                int committed = cmd.kind == A_PIPC_SELECT_BBID ? a_pctl_select_bbid(ctl, ops, cmd.bbid, &res) :
                    a_pctl_select_cell(ctl, ops, cmd.cell, &res);
                if (committed) result = ctl->has_target ? "verified" : "failed";
                else if (ctl->enabled && !ctl->faulted && ctl->has_target && cmd.kind == A_PIPC_SELECT_BBID &&
                    cmd.bbid[0]==ctl->cur_target[0] && cmd.bbid[1]==ctl->cur_target[1] &&
                    cmd.bbid[2]==ctl->cur_target[2] && cmd.bbid[3]==ctl->cur_target[3]) result = "verified";
            }
        }
        char out[512]; int n = a_pipc_format_status(ctl, out, sizeof out);
        const char *prefix = " result=";
        while (*prefix && n < 490) out[n++] = *prefix++;
        while (*result && n < 510) out[n++] = *result++;
        out[n++] = '\n';
        int sent = 0;
        au64 send_deadline=a_now_ms()+200;
        while (sent < n) {
            struct pollfd p={cfd,POLLOUT,0}; au64 now=a_now_ms();
            if(now>=send_deadline || poll(&p,1,(int)(send_deadline-now))<=0) break;
            assize w = send(cfd,out+sent,(ausize)(n-sent),MSG_NOSIGNAL|MSG_DONTWAIT);
            if(w<=0) break;
            sent += (int)w;
        }
    }
    close(cfd);
}

typedef struct { int fd; a_pctl *ctl; const a_pctl_ops *ops; const a_pcfg *cfg; } service_ctx;
static void service_busy(void *v) {
    service_ctx *s = v;
    if (s->cfg) write_osd(s->ctl, s->cfg);
    if (s->fd >= 0) serve_ipc(s->fd, s->ctl, s->ops);
}

static au64 picker_now(void *ctx) { (void)ctx; return a_now_ms(); }

int a_picker_rt_main(void)
{
    a_pctl ctl; a_pcfg cfg;
    commit_ctx cc; a_memset(&cc, 0, sizeof cc);
    cc.ctl = &ctl; cc.park_enabled = 1; cc.rearm_enabled = 1;
    a_sw_rpc_bind(&cc.rpc, &cc.rpc_ctx, 0 /* primary Ground radio: matches the proven host STATUS reader */);

    a_pctl_init(&ctl, 120);
    load_config(&ctl, &cfg);

    /* One ops bundle for BOTH the RC path and manual IPC SELECT -> the one engine. */
    a_pctl_ops ops; ops.ctx = &cc; ops.commit = do_commit; ops.now_ms = picker_now;

    a_rc_source rc; a_rc_source_init(&rc, RX_RING_PATH); a_rc_source_attach(&rc);
    int lfd = ipc_listen();
    if (lfd < 0) return 1; /* never run an uncontrollable RF owner */
    service_ctx svc = {lfd, &ctl, &ops, &cfg};
    cc.rpc_ctx.service = service_busy; cc.rpc_ctx.service_ctx = &svc;
    au64 last_reload = a_now_ms();

    for (;;) {
        int pwm_us=0; au32 age=0;
        ctl.rc_fresh=a_rc_source_poll(&rc,cfg.rc_channel,&pwm_us,&age);
        ctl.rc_protocol=rc.protocol; ctl.rc_pwm_us=ctl.rc_fresh ? pwm_us : 0;
        if (cfg.rc_enabled && cfg.rc_channel >= 1 && cfg.rc_channel <= 16) {
            a_sw_result res;
            a_pctl_on_source(&ctl, &ops, pwm_us, ctl.rc_fresh, rc.epoch, &res);
        }
        if (lfd >= 0) serve_ipc(lfd, &ctl, &ops);
        write_osd(&ctl, &cfg);

        au64 now = a_now_ms();
        if (now - last_reload > 2000) {        /* periodic T2/config reconcile (§11) */
            load_config(&ctl, &cfg);
            last_reload = now;
        }
        a_sleep_ms(20);
    }
    return 0;
}

#ifndef ASCENT_NO_START
/* hand-written entry (-nostdlib), mirrors the other runtime executables. */
void _start(void) { a_picker_rt_main(); _exit(0); }
#endif
