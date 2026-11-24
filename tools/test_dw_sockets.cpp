// test_dw_sockets.cpp — comprehensive unit test for Winsock UDP sockets & Demonware responder
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <vector>
#include <string>
#include <arpa/inet.h>
#include <unistd.h>

#include "win32_shim.h"

// Simulated guest memory arena (64MB)
static uint8_t g_guest_arena[64 * 1024 * 1024];
static uint32_t g_arena_alloc = 0x10000;

extern "C" {
uint32_t win32_mem_read32(uint32_t a) {
    assert(a + 4 <= sizeof(g_guest_arena));
    uint32_t v;
    memcpy(&v, &g_guest_arena[a], 4);
    return v;
}
void win32_mem_write32(uint32_t a, uint32_t v) {
    assert(a + 4 <= sizeof(g_guest_arena));
    memcpy(&g_guest_arena[a], &v, 4);
}
uint8_t win32_mem_read8(uint32_t a) {
    assert(a < sizeof(g_guest_arena));
    return g_guest_arena[a];
}
void win32_mem_write8(uint32_t a, uint8_t v) {
    assert(a < sizeof(g_guest_arena));
    g_guest_arena[a] = v;
}
uint8_t* win32_guest_ptr(uint32_t a) {
    return &g_guest_arena[a];
}
bool win32_bulk_dst_hooked(uint32_t a, uint32_t n) { return false; }
uint32_t win32_shim_alloc(uint32_t size, uint32_t align) {
    uint32_t a = (g_arena_alloc + align - 1) & ~(align - 1);
    g_arena_alloc = a + size;
    return a;
}
const char* win32_game_dir() { return "/tmp"; }
}

// Forward declarations of winsock entry points in dw_local.cpp
void impl_wsock_ordinal_2(State&);   // bind
void impl_wsock_ordinal_3(State&);   // closesocket
void impl_wsock_ordinal_4(State&);   // connect
void impl_ws2_ordinal_6(State&);     // getsockname
void impl_ws2_ordinal_7(State&);     // getsockopt
void impl_wsock_ordinal_9(State&);   // htons
void impl_wsock_ordinal_10(State&);  // inet_addr
void impl_wsock_ordinal_12(State&);  // ioctlsocket
void impl_ws2_ordinal_12(State&);    // inet_ntoa
void impl_wsock_ordinal_16(State&);  // recv
void impl_wsock_ordinal_17(State&);  // recvfrom
void impl_wsock_ordinal_18(State&);  // select
void impl_wsock_ordinal_19(State&);  // send
void impl_wsock_ordinal_20(State&);  // sendto
void impl_wsock_ordinal_21(State&);  // setsockopt
void impl_wsock_ordinal_23(State&);  // socket
void impl_wsock_ordinal_52(State&);  // gethostbyname
void impl_wsock_ordinal_57(State&);  // gethostname
void impl_wsock_ordinal_111(State&); // WSAGetLastError
void impl_ws2_ordinal_112(State&);  // WSASetLastError

// Helper to set up stack arguments for stdcall
static State make_state(const std::vector<uint32_t>& args) {
    State st{};
    // Stack grows down. Put return addr at ESP, args at ESP+4, ESP+8, ...
    uint32_t sp = 0x200000;
    st.gpr.rsp.dword = sp;
    win32_mem_write32(sp, 0xDEADBEEF); // return addr
    for (size_t i = 0; i < args.size(); i++) {
        win32_mem_write32(sp + 4 + (uint32_t)i * 4, args[i]);
    }
    return st;
}

int main() {
    printf("=== BO2 Darwin Winsock UDP & Local Responder Unit Test ===\n");

    // 1. Create two UDP sockets
    printf("[TEST 1] Creating UDP sockets...\n");
    State st_s1 = make_state({2 /* AF_INET */, 2 /* SOCK_DGRAM */, 17 /* IPPROTO_UDP */});
    impl_wsock_ordinal_23(st_s1);
    uint32_t s1 = st_s1.gpr.rax.dword;
    assert(s1 >= 0x200);

    State st_s2 = make_state({2 /* AF_INET */, 2 /* SOCK_DGRAM */, 17 /* IPPROTO_UDP */});
    impl_wsock_ordinal_23(st_s2);
    uint32_t s2 = st_s2.gpr.rax.dword;
    assert(s2 >= 0x200 && s2 != s1);
    printf("  Created socket s1=%u, s2=%u\n", s1, s2);

    // 2. Bind both sockets to port 0 (ephemeral)
    printf("[TEST 2] Binding sockets to ephemeral ports...\n");
    uint32_t sa1 = 0x1000;
    // sockaddr_in: family=AF_INET(2), port=0, ip=0 (INADDR_ANY)
    win32_mem_write8(sa1, 2); win32_mem_write8(sa1 + 1, 0);
    win32_mem_write8(sa1 + 2, 0); win32_mem_write8(sa1 + 3, 0);
    win32_mem_write32(sa1 + 4, 0);
    State st_b1 = make_state({s1, sa1, 16});
    impl_wsock_ordinal_2(st_b1);
    assert(st_b1.gpr.rax.dword == 0);

    uint32_t sa2 = 0x1100;
    win32_mem_write8(sa2, 2); win32_mem_write8(sa2 + 1, 0);
    win32_mem_write8(sa2 + 2, 0); win32_mem_write8(sa2 + 3, 0);
    win32_mem_write32(sa2 + 4, 0);
    State st_b2 = make_state({s2, sa2, 16});
    impl_wsock_ordinal_2(st_b2);
    assert(st_b2.gpr.rax.dword == 0);

    // 3. Test getsockname to retrieve assigned ports
    printf("[TEST 3] Calling getsockname on bound sockets...\n");
    uint32_t gsa1 = 0x2000, len1 = 0x2100;
    win32_mem_write32(len1, 16);
    State st_gs1 = make_state({s1, gsa1, len1});
    impl_ws2_ordinal_6(st_gs1);
    assert(st_gs1.gpr.rax.dword == 0);
    assert(win32_mem_read32(len1) == 16);
    assert(win32_mem_read8(gsa1) == 2); // AF_INET
    uint16_t port1 = (uint16_t)(win32_mem_read8(gsa1 + 2) << 8 | win32_mem_read8(gsa1 + 3));
    assert(port1 > 0);

    uint32_t gsa2 = 0x2200, len2 = 0x2300;
    win32_mem_write32(len2, 16);
    State st_gs2 = make_state({s2, gsa2, len2});
    impl_ws2_ordinal_6(st_gs2);
    assert(st_gs2.gpr.rax.dword == 0);
    uint16_t port2 = (uint16_t)(win32_mem_read8(gsa2 + 2) << 8 | win32_mem_read8(gsa2 + 3));
    assert(port2 > 0);
    assert(port1 != port2);
    printf("  getsockname: s1 bound to port %u, s2 bound to port %u\n", port1, port2);

    // 4. Test port collision fallback (natural collision between s1 and s3)
    printf("[TEST 4] Testing bind collision fallback...\n");
    State st_s3 = make_state({2, 2, 17});
    impl_wsock_ordinal_23(st_s3);
    uint32_t s3 = st_s3.gpr.rax.dword;
    uint32_t sa3 = 0x1200;
    win32_mem_write8(sa3, 2); win32_mem_write8(sa3 + 1, 0);
    win32_mem_write8(sa3 + 2, (uint8_t)(port1 >> 8)); win32_mem_write8(sa3 + 3, (uint8_t)(port1 & 0xFF));
    win32_mem_write32(sa3 + 4, 0);
    State st_b3 = make_state({s3, sa3, 16});
    impl_wsock_ordinal_2(st_b3);
    assert(st_b3.gpr.rax.dword == 0); // Must succeed via fallback!

    uint32_t gsa3 = 0x2400, len3 = 0x2500;
    win32_mem_write32(len3, 16);
    State st_gs3 = make_state({s3, gsa3, len3});
    impl_ws2_ordinal_6(st_gs3);
    uint16_t port3 = (uint16_t)(win32_mem_read8(gsa3 + 2) << 8 | win32_mem_read8(gsa3 + 3));
    assert(port3 > 0 && port3 != port1);
    printf("  Collision fallback: requested %u (held by s1), successfully bound to %u\n", port1, port3);

    // 5. Send packet from s1 to s2 over 127.0.0.1
    printf("[TEST 5] Testing sendto from s1 to s2 (127.0.0.1:%u)...\n", port2);
    const char msg[] = "Hello BO2 Multiplayer UDP!";
    uint32_t msg_len = (uint32_t)strlen(msg);
    uint32_t send_buf = 0x3000;
    for (uint32_t i = 0; i < msg_len; i++) win32_mem_write8(send_buf + i, msg[i]);

    uint32_t dst_sa = 0x4000;
    win32_mem_write8(dst_sa, 2); win32_mem_write8(dst_sa + 1, 0);
    win32_mem_write8(dst_sa + 2, (uint8_t)(port2 >> 8)); win32_mem_write8(dst_sa + 3, (uint8_t)(port2 & 0xFF));
    // 127.0.0.1 = 0x7F000001
    win32_mem_write8(dst_sa + 4, 127); win32_mem_write8(dst_sa + 5, 0);
    win32_mem_write8(dst_sa + 6, 0); win32_mem_write8(dst_sa + 7, 1);

    State st_send = make_state({s1, send_buf, msg_len, 0, dst_sa, 16});
    impl_wsock_ordinal_20(st_send);
    assert(st_send.gpr.rax.dword == msg_len);
    printf("  sendto sent %u bytes\n", msg_len);

    // Give Darwin network stack a millisecond
    usleep(20000);

    // 6. Test select readability of s2
    printf("[TEST 6] Testing select readiness of s2...\n");
    uint32_t rd_set = 0x5000, wr_set = 0x5100, ex_set = 0x5200;
    // fd_set layout: count(uint32), sockets...
    win32_mem_write32(rd_set, 1);
    win32_mem_write32(rd_set + 4, s2);
    win32_mem_write32(wr_set, 0);
    win32_mem_write32(ex_set, 0);

    State st_sel = make_state({1, rd_set, wr_set, ex_set, 0});
    impl_wsock_ordinal_18(st_sel);
    assert(st_sel.gpr.rax.dword == 1);
    assert(win32_mem_read32(rd_set) == 1);
    assert(win32_mem_read32(rd_set + 4) == s2);
    printf("  select successfully reported s2 readable!\n");

    // 7. Test ioctlsocket FIONREAD on s2
    printf("[TEST 7] Testing ioctlsocket FIONREAD on s2...\n");
    uint32_t nread_ptr = 0x5300;
    win32_mem_write32(nread_ptr, 0);
    State st_ioctl = make_state({s2, 0x4004667f /* FIONREAD */, nread_ptr});
    impl_wsock_ordinal_12(st_ioctl);
    assert(st_ioctl.gpr.rax.dword == 0);
    uint32_t bytes_avail = win32_mem_read32(nread_ptr);
    assert(bytes_avail == msg_len);
    printf("  ioctlsocket FIONREAD: %u bytes waiting\n", bytes_avail);

    // 8. Test recvfrom on s2
    printf("[TEST 8] Testing recvfrom on s2...\n");
    uint32_t recv_buf = 0x6000, from_sa = 0x6100, from_len = 0x6200;
    win32_mem_write32(from_len, 16);
    State st_recv = make_state({s2, recv_buf, 1024, 0, from_sa, from_len});
    impl_wsock_ordinal_17(st_recv);
    assert(st_recv.gpr.rax.dword == msg_len);

    char recvd[64] = {0};
    for (uint32_t i = 0; i < msg_len; i++) recvd[i] = (char)win32_mem_read8(recv_buf + i);
    assert(strcmp(recvd, msg) == 0);

    uint16_t from_port = (uint16_t)(win32_mem_read8(from_sa + 2) << 8 | win32_mem_read8(from_sa + 3));
    uint32_t from_ip = (uint32_t)win32_mem_read8(from_sa + 4) << 24 | win32_mem_read8(from_sa + 5) << 16 |
                       win32_mem_read8(from_sa + 6) << 8 | win32_mem_read8(from_sa + 7);
    assert(from_port == port1);
    assert(from_ip == 0x7F000001); // 127.0.0.1
    printf("  recvfrom got exact payload \"%s\" from 127.0.0.1:%u\n", recvd, from_port);

    // 9. Non-blocking empty receive test
    printf("[TEST 9] Testing non-blocking recvfrom on empty socket...\n");
    State st_recv_empty = make_state({s2, recv_buf, 1024, 0, from_sa, from_len});
    impl_wsock_ordinal_17(st_recv_empty);
    assert(st_recv_empty.gpr.rax.dword == 0xFFFFFFFFu); // SOCKET_ERROR
    State st_err = make_state({});
    impl_wsock_ordinal_111(st_err);
    assert(st_err.gpr.rax.dword == 10035 /* WSAEWOULDBLOCK */);
    printf("  Empty recvfrom correctly returned WSAEWOULDBLOCK\n");

    // 9b. Test getsockopt (WS2_32 / WSOCK32 ordinal 7)
    printf("[TEST 9b] Testing getsockopt (SO_TYPE and SO_ERROR)...\n");
    uint32_t optval_ptr = 0x6800, optlen_ptr = 0x6900;
    win32_mem_write32(optlen_ptr, 4);
    win32_mem_write32(optval_ptr, 0xDEADBEEF);
    // SO_TYPE = 0x1008 on Darwin and Winsock
    State st_gso_type = make_state({s1, 0xFFFF /* SOL_SOCKET */, 0x1008 /* SO_TYPE */, optval_ptr, optlen_ptr});
    impl_ws2_ordinal_7(st_gso_type);
    assert(st_gso_type.gpr.rax.dword == 0);
    assert(win32_mem_read32(optval_ptr) == 2 /* SOCK_DGRAM */);

    // SO_ERROR = 0x1007 on Darwin and Winsock
    win32_mem_write32(optval_ptr, 0xDEADBEEF);
    win32_mem_write32(optlen_ptr, 4);
    State st_gso_err = make_state({s1, 0xFFFF /* SOL_SOCKET */, 0x1007 /* SO_ERROR */, optval_ptr, optlen_ptr});
    impl_ws2_ordinal_7(st_gso_err);
    assert(st_gso_err.gpr.rax.dword == 0);
    assert(win32_mem_read32(optval_ptr) == 0 /* no error */);
    printf("  getsockopt: SO_TYPE=SOCK_DGRAM and SO_ERROR=0 verified!\n");

    // 9c. Test select timeout with no data
    printf("[TEST 9c] Testing select with timeout...\n");
    uint32_t tv_ptr = 0x6A00;
    win32_mem_write32(tv_ptr, 0);     // tv_sec = 0
    win32_mem_write32(tv_ptr + 4, 10000); // tv_usec = 10ms
    win32_mem_write32(rd_set, 1);
    win32_mem_write32(rd_set + 4, s2); // empty socket
    State st_sel_to = make_state({1, rd_set, 0, 0, tv_ptr});
    impl_wsock_ordinal_18(st_sel_to);
    assert(st_sel_to.gpr.rax.dword == 0); // Timed out, 0 sockets ready
    assert(win32_mem_read32(rd_set) == 0);
    printf("  select timeout: correctly waited and returned 0 ready sockets\n");

    // 9d. Test connected UDP sendto with to == NULL
    printf("[TEST 9d] Testing connected UDP sendto with to == NULL...\n");
    // Connect s3 to s2 (127.0.0.1:port2)
    uint32_t conn_sa = 0x6B00;
    win32_mem_write8(conn_sa, 2); win32_mem_write8(conn_sa + 1, 0);
    win32_mem_write8(conn_sa + 2, (uint8_t)(port2 >> 8)); win32_mem_write8(conn_sa + 3, (uint8_t)(port2 & 0xFF));
    win32_mem_write8(conn_sa + 4, 127); win32_mem_write8(conn_sa + 5, 0);
    win32_mem_write8(conn_sa + 6, 0); win32_mem_write8(conn_sa + 7, 1);
    State st_conn = make_state({s3, conn_sa, 16});
    impl_wsock_ordinal_4(st_conn);
    assert(st_conn.gpr.rax.dword == 0);

    const char conn_msg[] = "Connected UDP message!";
    uint32_t cmsg_len = (uint32_t)strlen(conn_msg);
    uint32_t csend_buf = 0x6C00;
    for (uint32_t i = 0; i < cmsg_len; i++) win32_mem_write8(csend_buf + i, conn_msg[i]);
    // Call sendto with to = 0, tolen = 0
    State st_csend = make_state({s3, csend_buf, cmsg_len, 0, 0, 0});
    impl_wsock_ordinal_20(st_csend);
    assert(st_csend.gpr.rax.dword == cmsg_len);

    usleep(20000);
    State st_crecv = make_state({s2, recv_buf, 1024, 0, from_sa, from_len});
    impl_wsock_ordinal_17(st_crecv);
    assert(st_crecv.gpr.rax.dword == cmsg_len);
    char crecvd[64] = {0};
    for (uint32_t i = 0; i < cmsg_len; i++) crecvd[i] = (char)win32_mem_read8(recv_buf + i);
    assert(strcmp(crecvd, conn_msg) == 0);
    printf("  connected sendto: s3 -> s2 via to=NULL succeeded: \"%s\"\n", crecvd);

    // 10. Test Demonware STUN packet routing
    printf("[TEST 10] Testing synthetic Demonware STUN packet...\n");
    // STUN address request: [0x1e, 0x02, 0x00]
    uint32_t stun_buf = 0x7000;
    win32_mem_write8(stun_buf, 0x1e); win32_mem_write8(stun_buf + 1, 2); win32_mem_write8(stun_buf + 2, 0);

    // Resolve stun host first to get a 10.77.x.x IP
    uint32_t stun_name = 0x7100;
    const char sname[] = "t6-stun.demonware.net";
    for (size_t i = 0; i <= strlen(sname); i++) win32_mem_write8(stun_name + (uint32_t)i, sname[i]);
    State st_dns = make_state({stun_name});
    impl_wsock_ordinal_52(st_dns);
    assert(st_dns.gpr.rax.dword != 0);

    // Target: 10.77.0.1:3074
    uint32_t stun_dst = 0x7200;
    win32_mem_write8(stun_dst, 2); win32_mem_write8(stun_dst + 1, 0);
    win32_mem_write8(stun_dst + 2, (uint8_t)(3074 >> 8)); win32_mem_write8(stun_dst + 3, (uint8_t)(3074 & 0xFF));
    win32_mem_write8(stun_dst + 4, 10); win32_mem_write8(stun_dst + 5, 77);
    win32_mem_write8(stun_dst + 6, 0); win32_mem_write8(stun_dst + 7, 1);

    State st_stun_send = make_state({s1, stun_buf, 3, 0, stun_dst, 16});
    impl_wsock_ordinal_20(st_stun_send);
    assert(st_stun_send.gpr.rax.dword == 3);

    // Read back synthetic STUN response on s1
    State st_stun_recv = make_state({s1, recv_buf, 1024, 0, from_sa, from_len});
    impl_wsock_ordinal_17(st_stun_recv);
    assert(st_stun_recv.gpr.rax.dword > 0);
    assert(win32_mem_read8(recv_buf) == 0x1f /* STUN_ADDR_REPLY */);
    printf("  Synthetic STUN response received: type=0x%02x\n", win32_mem_read8(recv_buf));

    // 11. DNS resolution test
    printf("[TEST 11] Testing host DNS and Demonware name resolution...\n");
    // "bo2-native" -> 10.77.1.2
    uint32_t name_bo2 = 0x7300;
    const char bo2_host[] = "bo2-native";
    for (size_t i = 0; i <= strlen(bo2_host); i++) win32_mem_write8(name_bo2 + (uint32_t)i, bo2_host[i]);
    State st_dns_bo2 = make_state({name_bo2});
    impl_wsock_ordinal_52(st_dns_bo2);
    assert(st_dns_bo2.gpr.rax.dword != 0);

    // Host DNS test: "127.0.0.1" -> 127.0.0.1
    uint32_t name_local = 0x7400;
    const char local_host[] = "127.0.0.1";
    for (size_t i = 0; i <= strlen(local_host); i++) win32_mem_write8(name_local + (uint32_t)i, local_host[i]);
    State st_dns_local = make_state({name_local});
    impl_wsock_ordinal_52(st_dns_local);
    assert(st_dns_local.gpr.rax.dword != 0);
    uint32_t he = st_dns_local.gpr.rax.dword;
    uint32_t addr_list = win32_mem_read32(he + 12);
    uint32_t ip_addr = win32_mem_read32(addr_list);
    assert(win32_mem_read8(ip_addr) == 127);
    assert(win32_mem_read8(ip_addr + 3) == 1);
    printf("  DNS resolution: 127.0.0.1 resolved successfully to hostent!\n");

    // 12. WSASetLastError test
    printf("[TEST 12] Testing WSASetLastError / WSAGetLastError...\n");
    State st_seterr = make_state({10048 /* WSAEADDRINUSE */});
    impl_ws2_ordinal_112(st_seterr);
    State st_geterr = make_state({});
    impl_wsock_ordinal_111(st_geterr);
    assert(st_geterr.gpr.rax.dword == 10048);
    printf("  WSASetLastError verified.\n");

    // 13. closesocket test
    printf("[TEST 13] Testing closesocket...\n");
    State st_c1 = make_state({s1}); impl_wsock_ordinal_3(st_c1); assert(st_c1.gpr.rax.dword == 0);
    State st_c2 = make_state({s2}); impl_wsock_ordinal_3(st_c2); assert(st_c2.gpr.rax.dword == 0);
    State st_c3 = make_state({s3}); impl_wsock_ordinal_3(st_c3); assert(st_c3.gpr.rax.dword == 0);

    // Closed socket should return WSAENOTSOCK
    State st_send_closed = make_state({s1, send_buf, msg_len, 0, dst_sa, 16});
    impl_wsock_ordinal_20(st_send_closed);
    assert(st_send_closed.gpr.rax.dword == 0xFFFFFFFFu);
    impl_wsock_ordinal_111(st_geterr);
    assert(st_geterr.gpr.rax.dword == 10038 /* WSAENOTSOCK */);
    printf("  Closed socket correctly rejected with WSAENOTSOCK.\n");

    // 14. Edge Cases: Buffer truncation, invalid sockets, invalid addresses
    printf("[TEST 14] Testing Edge Cases...\n");
    // 14a. sendto on invalid socket
    State st_bad_sock = make_state({9999, send_buf, msg_len, 0, dst_sa, 16});
    impl_wsock_ordinal_20(st_bad_sock);
    assert(st_bad_sock.gpr.rax.dword == 0xFFFFFFFFu);
    impl_wsock_ordinal_111(st_geterr);
    assert(st_geterr.gpr.rax.dword == 10038 /* WSAENOTSOCK */);

    // 14b. getsockname with buffer too small (< 16 bytes)
    State st_s4 = make_state({2, 2, 17});
    impl_wsock_ordinal_23(st_s4);
    uint32_t s4 = st_s4.gpr.rax.dword;
    uint32_t small_len = 0x8000;
    win32_mem_write32(small_len, 8); // only 8 bytes instead of 16
    State st_bad_gs = make_state({s4, 0x8100, small_len});
    impl_ws2_ordinal_6(st_bad_gs);
    assert(st_bad_gs.gpr.rax.dword == 0xFFFFFFFFu);
    impl_wsock_ordinal_111(st_geterr);
    assert(st_geterr.gpr.rax.dword == 10014 /* WSAEFAULT */);

    // Truncated recvfrom
    // Re-create s5, s6
    State st_s5 = make_state({2, 2, 17}); impl_wsock_ordinal_23(st_s5); uint32_t s5 = st_s5.gpr.rax.dword;
    State st_s6 = make_state({2, 2, 17}); impl_wsock_ordinal_23(st_s6); uint32_t s6 = st_s6.gpr.rax.dword;
    State st_b5 = make_state({s5, sa1, 16}); impl_wsock_ordinal_2(st_b5);
    State st_b6 = make_state({s6, sa2, 16}); impl_wsock_ordinal_2(st_b6);

    uint32_t gsa6 = 0x2600, len6 = 0x2700;
    win32_mem_write32(len6, 16);
    State st_gs6 = make_state({s6, gsa6, len6});
    impl_ws2_ordinal_6(st_gs6);
    uint16_t port6 = (uint16_t)(win32_mem_read8(gsa6 + 2) << 8 | win32_mem_read8(gsa6 + 3));

    win32_mem_write8(dst_sa + 2, (uint8_t)(port6 >> 8)); win32_mem_write8(dst_sa + 3, (uint8_t)(port6 & 0xFF));
    State st_snd5 = make_state({s5, send_buf, msg_len, 0, dst_sa, 16});
    impl_wsock_ordinal_20(st_snd5);
    assert(st_snd5.gpr.rax.dword == msg_len);

    usleep(20000);

    // Recv with only 5 bytes buffer
    uint32_t trunc_buf = 0x8500;
    State st_recv_trunc = make_state({s6, trunc_buf, 5, 0, from_sa, from_len});
    impl_wsock_ordinal_17(st_recv_trunc);
    assert(st_recv_trunc.gpr.rax.dword == 5);
    impl_wsock_ordinal_111(st_geterr);
    assert(st_geterr.gpr.rax.dword == 10040 /* WSAEMSGSIZE */);
    char trunc_str[6] = {0};
    for (int i = 0; i < 5; i++) trunc_str[i] = (char)win32_mem_read8(trunc_buf + i);
    assert(strncmp(trunc_str, "Hello", 5) == 0);
    printf("  Buffer truncation: requested 5 bytes, received exactly 5 bytes (\"%s\") with WSAEMSGSIZE\n", trunc_str);

    // 14d. Non-existent domain lookup
    uint32_t bad_dns = 0x8600;
    const char bad_name[] = "this-domain-does-not-exist-1234567.invalid";
    for (size_t i = 0; i <= strlen(bad_name); i++) win32_mem_write8(bad_dns + (uint32_t)i, bad_name[i]);
    State st_bad_dns = make_state({bad_dns});
    impl_wsock_ordinal_52(st_bad_dns);
    assert(st_bad_dns.gpr.rax.dword == 0);
    impl_wsock_ordinal_111(st_geterr);
    assert(st_geterr.gpr.rax.dword == 11001 /* WSAHOST_NOT_FOUND */);
    printf("  Non-existent host resolution: correctly returned WSAHOST_NOT_FOUND\n");

    // Clean up
    State st_cs4 = make_state({s4}); impl_wsock_ordinal_3(st_cs4);
    State st_cs5 = make_state({s5}); impl_wsock_ordinal_3(st_cs5);
    State st_cs6 = make_state({s6}); impl_wsock_ordinal_3(st_cs6);

    printf("\n>>> ALL 14 TEST SUITES PASSED SUCCESSFULLY! <<<\n");
    return 0;
}
