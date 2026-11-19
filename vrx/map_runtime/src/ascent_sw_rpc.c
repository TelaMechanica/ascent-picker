/* ascent_sw_rpc.c — see ascent_sw_rpc.h. BENCH-GATED device glue (loopback sockets). */
#include "ascent_sw_rpc.h"
#include "ascent_sys.h"

struct a_sockaddr_in { au16 sin_family; au16 sin_port; au32 sin_addr; au8 sin_zero[8]; };

/* All socket operations share an absolute deadline, including connect and slow peers. */
static int ready(int fd, short events, au64 deadline)
{
    au64 now=a_now_ms();
    if(now>=deadline) return 0;
    struct pollfd p={fd,events,0};
    return poll(&p,1,(int)(deadline-now))>0 && (p.revents & events);
}
static int tcp_connect(au16 port, au64 deadline)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct a_sockaddr_in a;
    a_memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = (au16)(((port & 0xFF) << 8) | ((port >> 8) & 0xFF));
    a.sin_addr = 0x0100007Fu;
    if (connect(fd, (const void *)&a, sizeof a) != 0) {
        int error=0; unsigned int len=sizeof error;
        if (!ready(fd,POLLOUT,deadline) || getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&len)!=0 || error) {
            close(fd); return -1;
        }
    }
    return fd;
}

/* Fire a frame to a loopback port; optionally read a reply. Returns bytes read (>=0) or -1. */
static int rpc_call(au16 port, const au8 *frame, int flen, au8 *resp, int rcap)
{
    au64 deadline = a_now_ms() + 1500;
    int fd = tcp_connect(port, deadline);
    if (fd < 0) return -1;
    int sent = 0, n = 0, ok = 1;
    while (sent < flen && a_now_ms() < deadline) {
        if (!ready(fd,POLLOUT,deadline)) {ok=0;break;}
        assize w = send(fd, frame + sent, (ausize)(flen - sent), MSG_NOSIGNAL);
        if (w <= 0) { ok = 0; break; } sent += (int)w;
    }
    if (sent != flen) ok = 0;
    if (ok && resp && rcap > 0) {
        int want = A_SW_USBPACK_HDR;
        while (n < want && a_now_ms() < deadline) {
            if (!ready(fd,POLLIN,deadline)) {ok=0;break;}
            assize z = recv(fd, resp + n, (ausize)(want - n), 0);
            if (z <= 0) { ok = 0; break; } n += (int)z;
            if (n == A_SW_USBPACK_HDR) {
                au32 len = (au32)resp[1] | ((au32)resp[2]<<8) | ((au32)resp[3]<<16) | ((au32)resp[4]<<24);
                if (resp[0] != 0xAA || resp[17] != a_sw_checksum17(resp) || len > (au32)(rcap - A_SW_USBPACK_HDR - 1)) { ok = 0; break; }
                want = A_SW_USBPACK_HDR + (int)len + 1;
            }
        }
        if (n != want || n < A_SW_USBPACK_HDR+1 || resp[n-1] != 0xBB) ok = 0;
    }
    close(fd);
    return ok ? n : -1;
}

static int op_standby(void *ctx, int mode)
{
    (void)ctx;
    au8 f[16]; int n = a_sw_standby_frame(mode, 0, 0, f, sizeof f);
    if (n < 0) return -1;
    return rpc_call(A_SW_STANDBY_PORT, f, n, 0, 0) >= 0 ? 0 : -1;
}

/* Ground SETs return a framed, empty reply. The earlier working host RPC
 * waits for it even when expect=0. Sending bytes and closing is not command
 * completion; in particular it must not start the measured-drop budget.
 * GUI standby is a different protocol and retains its existing send-only path. */
static int ground_set(au16 port, const au8 *frame, int flen)
{
    au8 reply[A_SW_USBPACK_HDR + 1];
    int r = rpc_call(port, frame, flen, reply, sizeof reply);
    if (r != (int)sizeof reply) return -1;
    for (int i=5; i<9; i++) if (reply[i] != frame[i]) return -1;
    return 0;
}

/* Diagnostic observations only: no extra RPC, state owner, retry or RF action. */
static void trace_rpc(const char *op, au32 value, int result)
{
    char line[128]; int n=0;
    const char *s="picker_rpc ms=";
    while (*s) line[n++]=*s++;
    au64 stamp=a_now_ms();
    const char *hex="0123456789abcdef";
    for (int shift=60; shift>=0; shift-=4) line[n++]=hex[(stamp>>shift)&15];
    s=" op="; while (*s) line[n++]=*s++;
    while (*op && n<90) line[n++]=*op++;
    s=" value="; while (*s) line[n++]=*s++;
    for (int shift=28; shift>=0; shift-=4) line[n++]=hex[(value>>shift)&15];
    s=result<0 ? " result=error\n" : " result=complete\n";
    while (*s) line[n++]=*s++;
    (void)write(2,line,(ausize)n);
}
static int op_read_link(void *ctx, int *state);
#ifdef ASCENT_CLASSIC_PAIR_RELEASE
static int classic_pair_set(a_sw_rpc_ctx *c, int enter)
{
    au8 f[64]; int n=a_sw_pair_frame(enter,f,sizeof f);
    if(enter)c->pair_entered=1; /* unknown ENTER must still receive one cleanup EXIT */
    int r=n<0 ? -1 : ground_set(A_SW_RPC_PORT,f,n);
    trace_rpc(enter ? "classic_release_enter" : "classic_release_exit",(au32)enter,r);
    if(!enter && r>=0)c->pair_entered=0;
    return r;
}
#endif
static int op_setcand(void *ctx, const au8 macs[][4], int count)
{
    (void)ctx;
#ifdef ASCENT_CLASSIC_PAIR_RELEASE
    a_sw_rpc_ctx *c=ctx;
    /* Recovery closes an early transient release before widening membership. */
    if(count!=1 && c->pair_entered && classic_pair_set(c,0)<0)return -1;
    c->verify_peer=0;
    c->has_requested_target=count==1;
    if(count==1)a_memcpy(c->requested_target,macs[0],4);
#endif
    if (count == 1)
        trace_rpc("set_candidate_target", ((au32)macs[0][0]<<24) | ((au32)macs[0][1]<<16) |
                  ((au32)macs[0][2]<<8) | macs[0][3], 0);
    au8 f[A_SW_USBPACK_HDR + A_SW_CANDIDATE_PAYLEN + 1];
    int n = a_sw_candidate_frame(macs, count, f, sizeof f);
    if (n < 0) return -1;
    int r=ground_set(A_SW_RPC_PORT,f,n);
    trace_rpc("set_candidates_reply",(au32)count,r);
#ifdef ASCENT_CLASSIC_PAIR_RELEASE
    if(r>=0 && count==1) {
        int state=-1;
        if(op_read_link(ctx,&state)<0)return -1;
        /* This profile acknowledges narrowing without releasing a healthy link.
         * Ask its normal transient pairing control to release AFTER target-only
         * narrowing. The engine still measures a real drop before acquisition. */
        if(state==A_SW_LINK_CONNECT && classic_pair_set(c,1)<0)return -1;
    }
#endif
    return r;
}
static int op_pair(void *ctx)
{
    (void)ctx;
    au8 f[64]; int n;
#ifdef ASCENT_CLASSIC_PAIR_RELEASE
    a_sw_rpc_ctx *classic=ctx;
    int r=0;
    if(!classic->pair_entered) {
        n=a_sw_pair_frame(1,f,sizeof f);
        classic->pair_entered=1;
        r=n<0 ? -1 : ground_set(A_SW_RPC_PORT,f,n);
        trace_rpc("pair_enter_reply",1,r);
    }
#else
    n = a_sw_pair_frame(1, f, sizeof f);
    int r=n<0 ? -1 : ground_set(A_SW_RPC_PORT,f,n);
    trace_rpc("pair_enter_reply",1,r);
#endif
    if(r<0) {
        /* ENTER may have reached Ground even if its reply was lost. Close the
         * transient mode once; never retry ENTER or claim a completed switch. */
        n=a_sw_pair_frame(0,f,sizeof f);
        int cleanup=n<0 ? -1 : ground_set(A_SW_RPC_PORT,f,n);
        trace_rpc("pair_exit_cleanup_reply",0,cleanup);
#ifdef ASCENT_CLASSIC_PAIR_RELEASE
        if(cleanup>=0)classic->pair_entered=0;
#endif
        return -1;
    }
    a_sw_rpc_ctx *c=ctx;
    if(c->service) c->service(c->service_ctx);
    /* Match the operator's v22 SOP exactly: two GET_PAIR polls at 50ms,
     * followed by its 50ms dwell, then EXIT. Ignore candidate identities as
     * the SOP does; these polls are not normal-link peer identity evidence. */
    for(int i=0;i<2;i++) {
        a_sleep_ms(50);
        au8 req[2]={0,0},reply[A_SW_USBPACK_HDR+164+1];
        n=a_usbpack_build(A_SW_CMD_GET_PAIR,c->work_id,0,req,2,f,sizeof f);
        int got=n<0 ? -1 : rpc_call(A_SW_RPC_PORT,f,n,reply,sizeof reply);
        int valid=got==(int)sizeof reply && reply[5]==1 && reply[6]==0 && reply[7]==0 && reply[8]==1;
        trace_rpc("pair_poll_reply",(au32)(i+1),valid ? 0 : -1);
        if(c->service)c->service(c->service_ctx);
        if(!valid)break; /* canonical failed-poll path still emits EXIT once */
    }
    a_sleep_ms(A_SW_PAIR_DWELL_MS);
    n = a_sw_pair_frame(0, f, sizeof f);
    r=n<0 ? -1 : ground_set(A_SW_RPC_PORT,f,n);
    trace_rpc("pair_exit_reply",0,r);
#ifdef ASCENT_CLASSIC_PAIR_RELEASE
    if(r>=0) {
        classic->pair_entered=0;
        if(classic->has_requested_target) {
            au8 one[1][4], target_frame[A_SW_USBPACK_HDR+A_SW_CANDIDATE_PAYLEN+1];
            a_memcpy(one[0],classic->requested_target,4);
            int tn=a_usbpack_build(0x02000003u,0,0,classic->requested_target,4,target_frame,sizeof target_frame);
            r=tn<0 ? -1 : ground_set(A_SW_RPC_PORT,target_frame,tn);
            trace_rpc("classic_explicit_peer",1,r);
            if(r>=0) {
                tn=a_sw_candidate_frame(one,1,target_frame,sizeof target_frame);
                r=tn<0 ? -1 : ground_set(A_SW_RPC_PORT,target_frame,tn);
                trace_rpc("classic_target_after_pair",1,r);
            }
            if(r>=0)classic->verify_peer=1;
        }
    }
#endif
    if(r<0) return -1;
    return 0;
}
static int decode_status_link(const au8 *resp,int r,int *state)
{
    /* Same 18-byte usbpack envelope and 300-byte STATUS payload as the proven host
     * reader (caddx_picker_link / caddx_relay_qos). work_id=0 selects primary radio.
     * The 252 offset is in the PAYLOAD, never in the envelope or a peer identity. */
    int link_off = A_SW_USBPACK_HDR + 252;
    if (r != A_SW_USBPACK_HDR + 300 + 1) return -1;
    if (resp[5] != 1 || resp[6] != 0 || resp[7] != 0 || resp[8] != 0 || resp[link_off] > 2) return -1;
    *state=resp[link_off]; return 0;
}
static int op_read_link(void *ctx, int *state)
{
    a_sw_rpc_ctx *c = (a_sw_rpc_ctx *)ctx;
    au8 f[32], resp[512];
    int n = a_sw_status_frame(c->work_id, f, sizeof f);
    if (n < 0) return -1;
    int r = rpc_call(A_SW_RPC_PORT, f, n, resp, sizeof resp);
    int result=decode_status_link(resp,r,state);
#ifdef ASCENT_CLASSIC_PAIR_RELEASE
    if(result>=0 && *state==A_SW_LINK_CONNECT && c->verify_peer && c->has_requested_target &&
       a_sw_target_valid(resp+A_SW_USBPACK_HDR+254) &&
       a_memcmp(resp+A_SW_USBPACK_HDR+254,c->requested_target,4)!=0) {
        const au8 *peer=resp+A_SW_USBPACK_HDR+254;
        trace_rpc("classic_target_contradiction",((au32)peer[0]<<24)|((au32)peer[1]<<16)|((au32)peer[2]<<8)|peer[3],-1);
        return -1; /* positive contradiction is never a verified target */
    }
#endif
    trace_rpc("status_link",result<0 ? 0xffffffffu : (au32)*state,result);
    return result;
}
static au64 op_now(void *ctx) { (void)ctx; return a_now_ms(); }
static void op_sleep(void *ctx, au32 ms) {
    a_sw_rpc_ctx *c = ctx;
    if (c->service) c->service(c->service_ctx);
    a_sleep_ms((as64)ms);
}

void a_sw_rpc_bind(a_sw_ops *ops, a_sw_rpc_ctx *ctx, au32 work_id)
{
    ctx->work_id = work_id; ctx->service = 0; ctx->service_ctx = 0;
#ifdef ASCENT_CLASSIC_PAIR_RELEASE
    ctx->pair_entered=0;ctx->has_requested_target=0;ctx->verify_peer=0;
#endif
    ops->ctx = ctx;
    ops->send_standby   = op_standby;
    ops->set_candidates = op_setcand;
    ops->pair_rearm     = op_pair;
    ops->read_link      = op_read_link;
    ops->now_ms         = op_now;
    ops->sleep_ms       = op_sleep;
}
