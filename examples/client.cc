#include <webtransport.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>

int main(int argc, char** argv) {
    const char* url = argc > 1 ? argv[1] : "127.0.0.1:4433/chat";

    web_transport::ClientConfig cfg;
    cfg.verify_certificate = false;

    web_transport::WebTransport client(url, cfg);

    printf("[client] connecting to %s ...\n", url);

    web_transport::Session* sess = client.connect(/*timeout_ms=*/5000);
    if (!sess) {
        fprintf(stderr, "[client] connection failed\n");
        return 1;
    }
    printf("[client] connected (peer: %s)\n", sess->peer_address().c_str());

    // ── Datagram echo ─────────────────────────────────────────────────────────
    std::atomic<bool> dgram_received{false};
    sess->on_datagram([&](const uint8_t* data, size_t len) {
        printf("[client] datagram echo: %.*s\n", (int)len, data);
        dgram_received = true;
    });

    // ── Open a bidi stream ───────────────────────────────────────────────────
    std::atomic<bool> stream_done{false};

    web_transport::BidirectionalStream* s = sess->open_bidi_stream();
    if (!s) {
        fprintf(stderr, "[client] could not open bidi stream\n");
        sess->close();
        return 1;
    }
    printf("[client] stream %" PRIu64 " opened\n", s->id());

    s->on_data([&](const uint8_t* data, size_t len) {
        printf("[client] stream echo: %.*s\n", (int)len, data);
        stream_done = true;
    });

    s->write("Hello, Server!");
    s->close_write();

    sess->send_datagram("ping!");

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((!stream_done || !dgram_received) &&
           std::chrono::steady_clock::now() < deadline) {
        client.run_once(10);
    }

    if (!stream_done)    printf("[client] WARNING: no stream echo received\n");
    if (!dgram_received) printf("[client] WARNING: no datagram echo received\n");

    sess->close();
    client.shutdown();

    printf("[client] done\n");
    return 0;
}