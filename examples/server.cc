#include <webtransport.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    const char* cert = argc > 1 ? argv[1] : "cert.pem";
    const char* key  = argc > 2 ? argv[2] : "key.pem";
    const char* host = argc > 3 ? argv[3] : "127.0.0.1";
    uint16_t    port = argc > 4 ? static_cast<uint16_t>(std::atoi(argv[4])) : 4433;

    web_transport::Server svr;

    svr.WebTransport("/chat", [](web_transport::Session& sess) {
        printf("[server] new session from %s\n",
               sess.peer_address().c_str());

        // ── Incoming bidirectional streams ───────────────────────────────────
        sess.on_bidi_stream([&sess](web_transport::BidirectionalStream& s) {
            printf("[server] bidi stream %" PRIu64 " opened\n", s.id());

            s.on_data([&s, &sess](const uint8_t* data, size_t len) {
                printf("[server] stream %" PRIu64 " got %zu bytes\n",
                       s.id(), len);

                // Echo back on the stream.
                s.write(data, len);
                s.close_write();

                // Also broadcast as a datagram.
                sess.send_datagram(data, len);
            });
        });

        // ── Incoming unidirectional streams ──────────────────────────────────
        sess.on_unidi_stream([](web_transport::UnidirectionalStream& s) {
            s.on_data([](const uint8_t* data, size_t len) {
                printf("[server] unidi got: %.*s\n", (int)len, data);
            });
        });

        // ── Datagrams ────────────────────────────────────────────────────────
        sess.on_datagram([&sess](const uint8_t* data, size_t len) {
            printf("[server] datagram: %.*s\n", (int)len, data);
            sess.send_datagram(data, len); // echo
        });

        // ── Session lifecycle ─────────────────────────────────────────────────
        sess.on_close([](uint32_t code, const std::string& reason) {
            printf("[server] session closed: code=%u reason=%s\n",
                   code, reason.c_str());
        });
    });

    printf("[server] listening on %s:%u (cert=%s key=%s)\n",
           host, port, cert, key);

    svr.listen(host, port, cert, key);
    return 0;
}