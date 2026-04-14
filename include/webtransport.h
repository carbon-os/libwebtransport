// libwebtransport public API
// Single header for end users. Internal quiche types are never exposed.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace web_transport {

// ─── Stream ──────────────────────────────────────────────────────────────────

class Stream {
public:
    virtual ~Stream() = default;

    virtual bool write(const uint8_t* data, size_t len) = 0;

    bool write(std::string_view sv) {
        return write(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
    }

    virtual bool close_write() = 0;
    virtual void reset(uint32_t error_code = 0) = 0;

    using DataCallback = std::function<void(const uint8_t* data, size_t len)>;
    virtual void on_data(DataCallback cb) = 0;

    virtual size_t read(uint8_t* dst, size_t len) = 0;
    virtual size_t readable_bytes() const = 0;
    virtual uint64_t id() const = 0;
};

using BidirectionalStream  = Stream;
using UnidirectionalStream = Stream;

// ─── Session ─────────────────────────────────────────────────────────────────

class Session {
public:
    virtual ~Session() = default;

    virtual Stream* open_bidi_stream()  = 0;
    virtual Stream* open_unidi_stream() = 0;

    virtual bool send_datagram(const uint8_t* data, size_t len) = 0;
    bool send_datagram(std::string_view sv) {
        return send_datagram(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
    }

    virtual void close(uint32_t error_code = 0, const std::string& reason = "") = 0;

    virtual bool        is_ready()     const = 0;
    virtual std::string peer_address() const = 0;

    using ReadyCallback    = std::function<void(Session&)>;
    using CloseCallback    = std::function<void(uint32_t error_code, const std::string& reason)>;
    using StreamCallback   = std::function<void(Stream&)>;
    using DatagramCallback = std::function<void(const uint8_t* data, size_t len)>;

    virtual void on_ready(ReadyCallback cb)         = 0;
    virtual void on_close(CloseCallback cb)         = 0;
    virtual void on_bidi_stream(StreamCallback cb)  = 0;
    virtual void on_unidi_stream(StreamCallback cb) = 0;
    virtual void on_datagram(DatagramCallback cb)   = 0;
};

// ─── Server ──────────────────────────────────────────────────────────────────

class Server {
public:
    Server();
    ~Server();

    using SessionHandler = std::function<void(Session&)>;
    void WebTransport(const std::string& path, SessionHandler handler);

    void listen(const std::string& host, uint16_t port,
                const std::string& cert_file,
                const std::string& key_file,
                const std::string& secret = "webtransport_server_secret");

    void listen_async(const std::string& host, uint16_t port,
                      const std::string& cert_file,
                      const std::string& key_file,
                      const std::string& secret = "webtransport_server_secret");

    void stop();
    void shutdown();
    void run_once(int timeout_ms = 50);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ─── Client ──────────────────────────────────────────────────────────────────

struct ClientConfig {
    bool verify_certificate = false;
    std::vector<std::string> fingerprints;
    std::vector<std::string> protocols;
};

class WebTransport {
public:
    explicit WebTransport(std::string url, ClientConfig cfg = {});
    ~WebTransport();

    Session* connect(int timeout_ms = 5000);

    using ConnectCallback = std::function<void(Session* /*null on failure*/)>;
    void connect_async(ConnectCallback cb);

    void run_once(int timeout_ms = 50);
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace web_transport