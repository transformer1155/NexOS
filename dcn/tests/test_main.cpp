#include "dcn.h"
#include "dcn_kernel.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
static uint32_t g_now = 0;
#define T(name, cond) do{ if(cond){ g_pass++; printf("  PASS  %s\n", name); } \
                          else    { g_fail++; printf("  FAIL  %s\n", name); } }while(0)

/* ---------------- WPA2 full handshake (in-process supplicant+auth) ---------- */
static void test_wpa2_handshake(void){
    printf("[WPA2 4-way handshake]\n");
    const uint8_t aa[6]  = {0x00,0x0b,0x85,0x11,0x59,0xac};
    const uint8_t spa[6] = {0x00,0x0b,0x85,0x11,0x4c,0xa0};
    const char* ssid = "NexOS-AP";
    const char* pass = "Sup3rSecret!";
    uint8_t pmk[32], gtk[32];
    dcn_pbkdf2_hmac_sha1((const uint8_t*)pass, strlen(pass), (const uint8_t*)ssid, strlen(ssid), 4096, pmk, 32);
    for (int i = 0; i < 32; i++) gtk[i] = (uint8_t)(i*7 + 3);

    dcn_wpa2_authenticator_t auth; dcn_wpa2_authenticator_init(&auth, aa, spa, pmk, gtk);
    dcn_wpa2_supplicant_t   sup;  dcn_wpa2_supplicant_init(&sup, spa, aa, ssid, pass);

    uint8_t m1[256], m2[256], m3[256], m4[256];
    int n1 = dcn_wpa2_auth_m1(&auth, m1);
    uint8_t anonce[32]; memcpy(anonce, m1 + 17, 32);   /* ANonce lives at offset 17 */
    int n2 = dcn_wpa2_on_m1(&sup, anonce, m2);
    T("supplicant builds M2", n2 > 0);
    int n3 = dcn_wpa2_auth_on_m2(&auth, m2, (size_t)n2, m3);
    T("authenticator verifies M2 MIC + builds M3", n3 > 0);
    int n4 = dcn_wpa2_on_m3(&sup, m3, (size_t)n3, m4);
    T("supplicant verifies M3 MIC + unwraps GTK + builds M4", n4 > 0);
    int ok = dcn_wpa2_auth_on_m4(&auth, m4, (size_t)n4);
    T("authenticator verifies M4 MIC", ok == 1);
    T("both sides derived identical PTK", memcmp(sup.ptk, auth.ptk, 64) == 0);
    T("both sides derived identical GTK", memcmp(sup.gtk, auth.gtk, 32) == 0);

    /* wrong password must fail MIC verification on M2 */
    dcn_wpa2_supplicant_t bad; dcn_wpa2_supplicant_init(&bad, spa, aa, ssid, "wrong-password");
    int n2b = dcn_wpa2_on_m1(&bad, m1 + 17, m2);
    int n3b = dcn_wpa2_auth_on_m2(&auth, m2, (size_t)n2b, m3);
    T("wrong password rejected (M2 MIC fail)", n3b == 0);
}

/* ---------------- LAN discovery (loopback bus, 3 nodes) ------------------- */
struct tnode { dcn_transport_t* t; dcn_discovery_t* d; uint8_t mac[6]; };
static int feed_disc(void* u, dcn_msg_t* m){
    struct tnode* nd = (struct tnode*)u;
    if (memcmp(m->src, nd->mac, 6) == 0) return 0;
    dcn_discovery_on_message(nd->d, m, g_now);
    return 0;
}
static void pump_all(struct tnode* ns, int n){
    for (int i = 0; i < n; i++) dcn_transport_pump(ns[i].t, feed_disc, &ns[i]);
}
static void test_discovery(void){
    printf("[LAN discovery]\n");
    dcn_transport_loopback_reset();
    struct tnode ns[3];
    uint8_t macs[3][6] = {{0x02,0,0,0,0,0x0a},{0x02,0,0,0,0,0x0b},{0x02,0,0,0,0,0x0c}};
    for (int i = 0; i < 3; i++){
        memcpy(ns[i].mac, macs[i], 6);
        ns[i].t = dcn_transport_loopback_create(ns[i].mac);
        char nm[16]; snprintf(nm, sizeof(nm), "node%d", i);
        ns[i].d = dcn_discovery_create(ns[i].t, nm, 4);
    }
    /* heartbeat: every node beacons; everyone learns the other two */
    g_now = 1000;
    for (int i = 0; i < 3; i++) dcn_discovery_emit_beacon(ns[i].d);
    pump_all(ns, 3);
    T("node0 discovered 2 peers via beacon",  dcn_discovery_count(ns[0].d) == 2);
    T("node1 discovered 2 peers via beacon",  dcn_discovery_count(ns[1].d) == 2);
    T("node2 discovered 2 peers via beacon",  dcn_discovery_count(ns[2].d) == 2);

    /* explicit scan + reply */
    dcn_discovery_scan(ns[0].d);
    pump_all(ns, 3);                 /* node0 query -> node1/2 reply -> deliver to node0 */
    pump_all(ns, 3);
    T("node0 still has 2 peers after scan", dcn_discovery_count(ns[0].d) == 2);

    /* eviction: refresh node1 only, advance time past timeout, tick */
    g_now = 7000;
    dcn_discovery_emit_beacon(ns[1].d);
    pump_all(ns, 3);
    for (int i = 0; i < 3; i++) dcn_discovery_tick(ns[i].d, g_now);
    T("stale peers evicted, fresh peer kept (node0 sees node1 only)",
      dcn_discovery_count(ns[0].d) == 1);

    for (int i = 0; i < 3; i++){ dcn_discovery_destroy(ns[i].d); dcn_transport_destroy(ns[i].t); }
    dcn_transport_loopback_reset();
}

/* ---------------- Data chunking + dynamic (fail) scheduling -------------- */
static void identity(const uint8_t* in, size_t inlen, uint8_t* out, size_t* outlen, void* u){
    (void)u; memcpy(out, in, inlen); *outlen = inlen;
}
static void test_chunk(void){
    printf("[Data chunking + dynamic scheduling]\n");
    const char* msg = "DISTRIBUTED COMPUTE NETWORK — split, dispatch, merge.";
    size_t len = strlen(msg);
    dcn_node_t local[1];
    memset(&local[0], 0, sizeof(local[0]));
    memcpy(local[0].mac, "\x02\x00\x00\x00\x00\x01", 6);
    local[0].capacity = 64;

    /* normal split + assign + run + merge */
    dcn_chunk_plan_t* p = dcn_chunk_create((const uint8_t*)msg, len);
    T("split_by_size -> multiple blocks", dcn_chunk_split_by_size(p, 8) == DCN_OK && dcn_chunk_count(p) > 1);
    T("assign all blocks to local node", dcn_chunk_assign(p, local, 1) == dcn_chunk_count(p));
    dcn_chunk_run_local(p, identity, NULL);
    T("all blocks done", dcn_chunk_all_done(p) == 1);
    uint8_t out[256]; size_t outlen = 0;
    T("merge reconstructs original", dcn_chunk_merge(p, out, sizeof(out), &outlen) == DCN_OK
                                    && outlen == len && memcmp(out, msg, len) == 0);
    dcn_chunk_destroy(p);

    /* split_by_count */
    p = dcn_chunk_create((const uint8_t*)msg, len);
    T("split_by_count(5)", dcn_chunk_split_by_count(p, 5) == DCN_OK && dcn_chunk_count(p) == 5);
    dcn_chunk_destroy(p);

    /* fail + reassign (dynamic scheduling / fault tolerance) */
    p = dcn_chunk_create((const uint8_t*)msg, len);
    dcn_chunk_split_by_size(p, 8);
    dcn_chunk_assign(p, local, 1);
    T("fail block 0 returns it to PENDING", dcn_chunk_fail(p, 0) == DCN_OK
                                          && dcn_chunk_block(p,0)->state == DCN_BLOCK_PENDING);
    T("reassign picks it up again", dcn_chunk_assign(p, local, 1) == 1);
    dcn_chunk_run_local(p, identity, NULL);
    outlen = 0;
    T("merge after reassign still correct", dcn_chunk_merge(p, out, sizeof(out), &outlen) == DCN_OK
                                         && outlen == len && memcmp(out, msg, len) == 0);
    dcn_chunk_destroy(p);
}

/* ---------------- Distributed execution over the loopback LAN ------------ */
static void sum_local(const uint8_t* in, size_t inlen, uint8_t* out, size_t* outlen, void* u){
    (void)u; uint32_t s = 0;
    for (size_t i = 0; i < inlen; i++) s += in[i];
    dcn_wr32be(out, s); *outlen = 4;
}
/* A tiny LAN: one coordinator + nw workers, all on the loopback bus, with
   discovery converged (everyone knows everyone else). */
struct dlan {
    dcn_transport_t* ct;
    dcn_discovery_t* cd;
    uint8_t cmac[6];
    dcn_transport_t* wt[2];
    dcn_worker_t*    wk[2];
    dcn_discovery_t* wd[2];
    uint8_t wmac[2][6];
    struct tnode all[3];
    int nw;
};
static void lan_setup(struct dlan* L, int nw){
    dcn_transport_loopback_reset();
    L->nw = nw;
    memcpy(L->cmac, "\x02\x00\x00\x00\x00\x01", 6);
    L->ct = dcn_transport_loopback_create(L->cmac);
    L->cd = dcn_discovery_create(L->ct, "coord", 64);
    for (int i = 0; i < nw; i++){
        L->wmac[i][0]=0x02; L->wmac[i][1]=0; L->wmac[i][2]=0;
        L->wmac[i][3]=0;    L->wmac[i][4]=0; L->wmac[i][5]=(uint8_t)(0x10+i);
        L->wt[i] = dcn_transport_loopback_create(L->wmac[i]);
        L->wk[i] = dcn_worker_create(L->wt[i]);
        L->wd[i] = dcn_discovery_create(L->wt[i], "worker", 64);
    }
    memcpy(L->all[0].mac, L->cmac, 6); L->all[0].t = L->ct; L->all[0].d = L->cd;
    for (int i = 0; i < nw; i++){
        memcpy(L->all[1+i].mac, L->wmac[i], 6);
        L->all[1+i].t = L->wt[i]; L->all[1+i].d = L->wd[i];
    }
    g_now = 1000;
    dcn_discovery_emit_beacon(L->cd);
    for (int i = 0; i < nw; i++) dcn_discovery_emit_beacon(L->wd[i]);
    pump_all(L->all, nw + 1);
}
static void lan_teardown(struct dlan* L){
    for (int i = 0; i < L->nw; i++){
        dcn_discovery_destroy(L->wd[i]);
        dcn_worker_destroy(L->wk[i]);
        dcn_transport_destroy(L->wt[i]);
    }
    dcn_discovery_destroy(L->cd);
    dcn_transport_destroy(L->ct);
    dcn_transport_loopback_reset();
}
static void test_distributed(void){
    printf("[Distributed execution over loopback LAN]\n");
    struct dlan L;
    lan_setup(&L, 2);
    T("coordinator discovered 2 worker peers", dcn_discovery_count(L.cd) == 2);

    const char* msg = "DISTRIBUTED-COMPUTE-OVER-LAN! split dispatch compute merge.";
    size_t len = strlen(msg);
    dcn_chunk_plan_t* p = dcn_chunk_create((const uint8_t*)msg, len);
    T("split into multiple blocks", dcn_chunk_split_by_size(p, 8) == DCN_OK && dcn_chunk_count(p) > 1);

    dcn_node_t peers[DCN_MAX_NODES];
    int n = dcn_discovery_list(L.cd, peers, DCN_MAX_NODES);
    int assigned = dcn_chunk_assign(p, peers, n);
    T("all blocks assigned across 2 discovered peers", n == 2 && assigned == dcn_chunk_count(p));

    int dispatched = dcn_sched_dispatch(p, L.cd, L.ct, DCN_OP_SUM);
    T("coordinator dispatched a TASK per block over the LAN", dispatched == dcn_chunk_count(p));

    int w0 = dcn_worker_pump(L.wk[0], 0);
    int w1 = dcn_worker_pump(L.wk[1], 0);
    T("both workers executed their received tasks", (w0 + w1) == dcn_chunk_count(p));

    int got = dcn_sched_collect(p, L.ct);
    T("coordinator collected every result + plan all done", got == dcn_chunk_count(p) && dcn_chunk_all_done(p));

    uint8_t out[256]; size_t outlen = 0;
    int mrg = dcn_chunk_merge(p, out, sizeof(out), &outlen);

    dcn_chunk_plan_t* ref = dcn_chunk_create((const uint8_t*)msg, len);
    dcn_chunk_split_by_size(ref, 8);
    dcn_node_t local[1]; memset(&local[0], 0, sizeof(local[0]));
    memcpy(local[0].mac, "\x02\x00\x00\x00\x00\x7e", 6);
    dcn_chunk_assign(ref, local, 1);
    dcn_chunk_run_local(ref, sum_local, NULL);
    uint8_t refout[256]; size_t refoutlen = 0;
    dcn_chunk_merge(ref, refout, sizeof(refout), &refoutlen);
    T("merged distributed result == local reference",
      mrg == DCN_OK && outlen == refoutlen && memcmp(out, refout, outlen) == 0);

    dcn_chunk_destroy(p); dcn_chunk_destroy(ref);
    lan_teardown(&L);
}

/* Fault tolerance over the LAN: a block's worker "dies", the block returns to
   PENDING, the scheduler re-assigns and re-dispatches it, and the merged
   result is still byte-identical to a local reference. */
static void test_distributed_fault(void){
    printf("[Distributed fault tolerance (fail + reschedule over LAN)]\n");
    struct dlan L;
    lan_setup(&L, 2);

    const char* msg = "FAULT-TOLERANT DISTRIBUTED COMPUTE!";
    size_t len = strlen(msg);
    dcn_chunk_plan_t* p = dcn_chunk_create((const uint8_t*)msg, len);
    dcn_chunk_split_by_size(p, 8);
    dcn_node_t peers[DCN_MAX_NODES];
    int n = dcn_discovery_list(L.cd, peers, DCN_MAX_NODES);
    dcn_chunk_assign(p, peers, n);
    T("initial dispatch reached the workers", dcn_sched_dispatch(p, L.cd, L.ct, DCN_OP_SUM) == dcn_chunk_count(p));

    T("block 0 fails -> returns to PENDING", dcn_chunk_fail(p, 0) == DCN_OK
                                           && dcn_chunk_block(p, 0)->state == DCN_BLOCK_PENDING);
    T("rescheduler re-assigns block 0", dcn_chunk_assign(p, peers, n) == 1);
    T("re-dispatched after reschedule", dcn_sched_dispatch(p, L.cd, L.ct, DCN_OP_SUM) >= 1);

    dcn_worker_pump(L.wk[0], 0);
    dcn_worker_pump(L.wk[1], 0);
    dcn_sched_collect(p, L.ct);
    T("all blocks recovered + done after reschedule", dcn_chunk_all_done(p) == 1);

    uint8_t out[256]; size_t outlen = 0;
    dcn_chunk_plan_t* ref = dcn_chunk_create((const uint8_t*)msg, len);
    dcn_chunk_split_by_size(ref, 8);
    dcn_node_t local[1]; memset(&local[0], 0, sizeof(local[0]));
    memcpy(local[0].mac, "\x02\x00\x00\x00\x00\x7e", 6);
    dcn_chunk_assign(ref, local, 1);
    dcn_chunk_run_local(ref, sum_local, NULL);
    uint8_t refout[256]; size_t refoutlen = 0;
    dcn_chunk_merge(ref, refout, sizeof(refout), &refoutlen);
    T("merge after reschedule == local reference",
      dcn_chunk_merge(p, out, sizeof(out), &outlen) == DCN_OK
      && outlen == refoutlen && memcmp(out, refout, outlen) == 0);

    dcn_chunk_destroy(p); dcn_chunk_destroy(ref);
    lan_teardown(&L);
}

/* ---------------- Kernel thin interface ----------------------------------- */
static void test_kernel_iface(void){
    printf("[Kernel thin interface]\n");
    dcn_kernel_init();

    const char* msg = "kernel-distributed payload";
    size_t len = strlen(msg);
    uint8_t out[256]; size_t outlen = 0;
    int rc = dcn_kernel_distribute((const uint8_t*)msg, len, 8, identity, NULL, out, sizeof(out), &outlen);
    T("dcn_kernel_distribute merges correctly", rc == DCN_OK && outlen == len && memcmp(out, msg, len) == 0);

    /* WPA2 connect through the thin interface via a mock AP */
    const uint8_t aa[6]  = {0x00,0x0b,0x85,0x11,0x59,0xac};
    /* The kernel node drives the 4-way handshake as STA, so its supplicant MAC
       is its own MAC (g_k->self = 02:00:00:00:00:01). The mock AP must be
       seeded with the SAME SPA, otherwise the derived PTKs diverge and M3 MIC
       verification fails. */
    const uint8_t spa[6] = {0x02,0x00,0x00,0x00,0x00,0x01};
    uint8_t pmk[32], gtk[32];
    const char* kssid = "NexOS-AP";
    const char* kpass = "Sup3rSecret!";
    /* Use strlen so the PMK matches what the supplicant derives from the same
       credential (a hardcoded length here would not track the actual strings). */
    dcn_pbkdf2_hmac_sha1((const uint8_t*)kpass, strlen(kpass),
                         (const uint8_t*)kssid, strlen(kssid), 4096, pmk, 32);
    for (int i = 0; i < 32; i++) gtk[i] = (uint8_t)(i*7 + 3);
    dcn_wifi_driver_t* ap = dcn_wifi_mock_ap_create(aa, spa, pmk, gtk);
    dcn_kernel_wifi_set_driver(ap);
    dcn_kernel_wifi_set_ap(aa);
    rc = dcn_kernel_wifi_connect("NexOS-AP", "Sup3rSecret!");
    T("dcn_kernel_wifi_connect succeeds (real 4-way)", rc == DCN_OK);
    dcn_wifi_mock_ap_destroy(ap);

    dcn_kernel_shutdown();
}

/* ---------------- main ---------------------------------------------------- */
int main(void){
    printf("==================================================\n");
    printf(" DCN — Distributed Compute Network test suite\n");
    printf("==================================================\n");

    int crypto_fail = dcn_crypto_selftest();
    if (crypto_fail == 0){
        printf("[Crypto self-test] ALL RFC/FIPS + real WPA2-capture vectors PASS\n");
        g_pass++;
    } else {
        printf("[Crypto self-test] FAILED at check #%d\n", crypto_fail);
        g_fail++;
    }

    test_wpa2_handshake();
    test_discovery();
    test_chunk();
    test_distributed();
    test_distributed_fault();
    test_kernel_iface();

    printf("==================================================\n");
    printf(" RESULT: %d passed, %d failed\n", g_pass, g_fail);
    printf("==================================================\n");
    return g_fail ? 1 : 0;
}
