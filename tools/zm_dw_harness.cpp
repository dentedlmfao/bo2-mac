// zm_dw_harness.cpp — Mock server backed by dw_local.cpp's real Darwin UDP socket
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <vector>
#include <string>
#include <arpa/inet.h>
#include <unistd.h>

#include "win32_shim.h"

// Simulated guest memory arena (16MB)
static uint8_t g_guest_arena[16 * 1024 * 1024];
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
void impl_ws2_ordinal_6(State&);     // getsockname
void impl_wsock_ordinal_17(State&);  // recvfrom
void impl_wsock_ordinal_18(State&);  // select
void impl_wsock_ordinal_20(State&);  // sendto
void impl_wsock_ordinal_23(State&);  // socket

static State make_state(const std::vector<uint32_t>& args) {
    State st{};
    uint32_t sp = 0x200000;
    st.gpr.rsp.dword = sp;
    win32_mem_write32(sp, 0xDEADBEEF);
    for (size_t i = 0; i < args.size(); i++) {
        win32_mem_write32(sp + 4 + (uint32_t)i * 4, args[i]);
    }
    return st;
}

int main(int argc, char** argv) {
    uint16_t req_port = 0; // ephemeral by default
    if (argc > 1) {
        req_port = (uint16_t)atoi(argv[1]);
    }

    // 1. Create UDP socket via dw_local.cpp
    State st_sock = make_state({2 /* AF_INET */, 2 /* SOCK_DGRAM */, 17 /* IPPROTO_UDP */});
    impl_wsock_ordinal_23(st_sock);
    uint32_t s = st_sock.gpr.rax.dword;
    if (s == 0xFFFFFFFFu) {
        fprintf(stderr, "ERROR: socket() failed\n");
        return 1;
    }

    // 2. Bind socket
    uint32_t sa_bind = 0x1000;
    win32_mem_write8(sa_bind, 2); win32_mem_write8(sa_bind + 1, 0); // AF_INET
    win32_mem_write8(sa_bind + 2, (uint8_t)(req_port >> 8));
    win32_mem_write8(sa_bind + 3, (uint8_t)(req_port & 0xFF));
    win32_mem_write32(sa_bind + 4, 0); // INADDR_ANY
    State st_bind = make_state({s, sa_bind, 16});
    impl_wsock_ordinal_2(st_bind);
    if (st_bind.gpr.rax.dword != 0) {
        fprintf(stderr, "ERROR: bind() failed\n");
        return 1;
    }

    // 3. Query assigned port via getsockname
    uint32_t sa_gs = 0x2000, len_gs = 0x2100;
    win32_mem_write32(len_gs, 16);
    State st_gs = make_state({s, sa_gs, len_gs});
    impl_ws2_ordinal_6(st_gs);
    uint16_t bound_port = (uint16_t)(win32_mem_read8(sa_gs + 2) << 8 | win32_mem_read8(sa_gs + 3));

    // Announce ready to parent process
    printf("READY port=%u\n", bound_port);
    fflush(stdout);

    // 4. Server loop
    uint32_t rx_buf = 0x3000, from_sa = 0x4000, from_len = 0x4100;
    uint32_t tx_buf = 0x5000;
    uint32_t rd_set = 0x6000, wr_set = 0x6100, ex_set = 0x6200;

    int max_iterations = 200; // up to ~10 seconds at 50ms intervals
    while (max_iterations-- > 0) {
        // Select check
        win32_mem_write32(rd_set, 1);
        win32_mem_write32(rd_set + 4, s);
        win32_mem_write32(wr_set, 0);
        win32_mem_write32(ex_set, 0);

        State st_sel = make_state({1, rd_set, wr_set, ex_set, 0});
        impl_wsock_ordinal_18(st_sel);
        if (st_sel.gpr.rax.dword > 0 && win32_mem_read32(rd_set) == 1) {
            // Read datagram
            win32_mem_write32(from_len, 16);
            State st_recv = make_state({s, rx_buf, 2048, 0, from_sa, from_len});
            impl_wsock_ordinal_17(st_recv);
            uint32_t n = st_recv.gpr.rax.dword;
            if (n > 0 && n != 0xFFFFFFFFu) {
                std::vector<uint8_t> payload(n);
                for (uint32_t i = 0; i < n; i++) payload[i] = win32_mem_read8(rx_buf + i);

                std::string msg(payload.begin(), payload.end());
                if (msg == "QUIT") {
                    break;
                }

                // Check for CoD out-of-band prefix \xff\xff\xff\xff
                if (n >= 4 && payload[0] == 0xFF && payload[1] == 0xFF && payload[2] == 0xFF && payload[3] == 0xFF) {
                    std::string cmd = msg.substr(4);
                    std::string resp;

                    if (cmd.find("getchallenge") == 0) {
                        resp = std::string("\xff\xff\xff\xff", 4) + "challengeResponse 1337042";
                    } else if (cmd.find("getinfo") == 0) {
                        resp = std::string("\xff\xff\xff\xff", 4) + "infoResponse\n\\challenge\\1337042\\hostname\\BO2-Darwin-Server\\mapname\\zm_transit\\gametype\\zclassic\\clients\\1\\sv_maxclients\\8";
                    } else if (cmd.find("ping") == 0) {
                        resp = std::string("\xff\xff\xff\xff", 4) + "ack";
                    } else {
                        resp = std::string("\xff\xff\xff\xff", 4) + "echoResponse " + cmd;
                    }

                    for (size_t i = 0; i < resp.size(); i++) {
                        win32_mem_write8(tx_buf + (uint32_t)i, (uint8_t)resp[i]);
                    }
                    State st_send = make_state({s, tx_buf, (uint32_t)resp.size(), 0, from_sa, 16});
                    impl_wsock_ordinal_20(st_send);
                }
            }
        }
        usleep(50000); // 50ms sleep
    }

    // 5. Clean up
    State st_close = make_state({s});
    impl_wsock_ordinal_3(st_close);
    return 0;
}
