#include "webtransport.h"
#include "wt_base.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "wtserver.h"
#include "wtclient.h"

namespace web_transport {

// ─── StreamImpl (forward declaration) ────────────────────────────────────────

class StreamImpl;

// ─── StreamVisitorBridge ─────────────────────────────────────────────────────

class StreamVisitorBridge final : public wt::StreamVisitor {
public:
    explicit StreamVisitorBridge(StreamImpl* s) : impl_(s) {}

    void OnCanRead() override;  // defined after StreamImpl is complete
    void OnCanWrite() override {}
    void OnResetStreamReceived(uint32_t) override { impl_ = nullptr; }
    void OnStopSendingReceived(uint32_t) override {}
    void OnWriteSideClosed() override {}

private:
    StreamImpl* impl_;  // raw, NOT owning
};

// ─── StreamImpl ──────────────────────────────────────────────────────────────

class StreamImpl final : public Stream {
public:
    explicit StreamImpl(wt::Stream* s) : wt_stream_(s) {
        s->SetVisitor(std::make_unique<StreamVisitorBridge>(this));
    }

    bool write(const uint8_t* data, size_t len) override {
        return wt_stream_ && wt_stream_->Write(data, len);
    }
    bool close_write() override {
        return wt_stream_ && wt_stream_->FinWrite();
    }
    void reset(uint32_t code) override {
        if (wt_stream_) wt_stream_->ResetStream(code);
    }
    void on_data(DataCallback cb) override { data_cb_ = std::move(cb); }

    size_t read(uint8_t* dst, size_t len) override {
        return wt_stream_ ? wt_stream_->Read(dst, len) : 0;
    }
    size_t readable_bytes() const override {
        return wt_stream_ ? wt_stream_->ReadableBytes() : 0;
    }
    uint64_t id() const override {
        return wt_stream_ ? wt_stream_->id() : 0;
    }

    void handle_can_read() {
        if (!wt_stream_ || !data_cb_) return;
        size_t n = wt_stream_->ReadableBytes();
        if (n == 0) return;
        tmp_buf_.resize(n);
        size_t got = wt_stream_->Read(tmp_buf_.data(), n);
        if (got > 0) data_cb_(tmp_buf_.data(), got);
    }

private:
    wt::Stream* wt_stream_;
    DataCallback         data_cb_;
    std::vector<uint8_t> tmp_buf_;
};

void StreamVisitorBridge::OnCanRead() {
    if (impl_) impl_->handle_can_read();
}

// ─── SessionImpl ─────────────────────────────────────────────────────────────

class SessionImpl final : public Session,
                          public wt::SessionVisitor {
public:
    explicit SessionImpl(wt::Session* s) : wt_session_(s) {}

    // ── Session interface ────────────────────────────────────────────────────

    Stream* open_bidi_stream() override {
        auto* ws = wt_session_ ? wt_session_->OpenBidirectionalStream() : nullptr;
        return ws ? wrap_stream(ws) : nullptr;
    }

    Stream* open_unidi_stream() override {
        auto* ws = wt_session_ ? wt_session_->OpenUnidirectionalStream() : nullptr;
        return ws ? wrap_stream(ws) : nullptr;
    }

    bool send_datagram(const uint8_t* data, size_t len) override {
        return wt_session_ && wt_session_->SendDatagram(data, len);
    }

    void close(uint32_t code, const std::string& reason) override {
        if (wt_session_) wt_session_->Close(code, reason);
    }

    bool        is_ready()     const override { return wt_session_ && wt_session_->IsReady(); }
    std::string peer_address() const override { return wt_session_ ? wt_session_->PeerAddress() : ""; }

    void on_ready(ReadyCallback cb)         override { ready_cb_  = std::move(cb); }
    void on_close(CloseCallback cb)         override { close_cb_  = std::move(cb); }
    void on_bidi_stream(StreamCallback cb)  override { bidi_cb_   = std::move(cb); }
    void on_unidi_stream(StreamCallback cb) override { unidi_cb_  = std::move(cb); }
    void on_datagram(DatagramCallback cb)   override { dgram_cb_  = std::move(cb); }

    // ── wt::SessionVisitor callbacks ─────────────────────────────────────────

    void OnSessionReady(const std::string&) override {
        if (ready_cb_) ready_cb_(*this);
    }

    void OnSessionClosed(uint32_t code, const std::string& msg) override {
        wt_session_ = nullptr;
        if (close_cb_) close_cb_(code, msg);
    }

    void OnIncomingBidirectionalStream(wt::Stream* ws) override {
        if (bidi_cb_) bidi_cb_(*wrap_stream(ws));
    }

    void OnIncomingUnidirectionalStream(wt::Stream* ws) override {
        if (unidi_cb_) unidi_cb_(*wrap_stream(ws));
    }

    void OnDatagramReceived(const uint8_t* data, size_t len) override {
        if (dgram_cb_) dgram_cb_(data, len);
    }

    void OnCanCreateNewOutgoingBidirectionalStream()  override {}
    void OnCanCreateNewOutgoingUnidirectionalStream() override {}

    bool transferred() const { return transferred_; }
    void mark_transferred()  { transferred_ = true; }

private:
    Stream* wrap_stream(wt::Stream* ws) {
        auto s = std::make_unique<StreamImpl>(ws);
        Stream* raw = s.get();
        owned_streams_.push_back(std::move(s));
        return raw;
    }

    wt::Session* wt_session_;
    bool         transferred_ = false;

    ReadyCallback    ready_cb_;
    CloseCallback    close_cb_;
    StreamCallback   bidi_cb_;
    StreamCallback   unidi_cb_;
    DatagramCallback dgram_cb_;

    std::vector<std::unique_ptr<Stream>> owned_streams_;
};

// ─── Server::Impl ─────────────────────────────────────────────────────────────

struct ServerVisitorImpl : public wt::ServerVisitor {
    using PathMap = std::map<std::string, Server::SessionHandler>;
    PathMap handlers;

    std::unique_ptr<wt::SessionVisitor> OnNewSession(
        wt::Session* session,
        const std::string& path,
        const std::string& /*peer*/,
        const std::string& /*proto*/) override
    {
        auto it = handlers.find(path);
        if (it == handlers.end()) return nullptr;

        auto impl = std::make_unique<SessionImpl>(session);
        it->second(*impl);
        impl->mark_transferred();
        return std::unique_ptr<wt::SessionVisitor>(impl.release());
    }
};

struct Server::Impl {
    wt::Server                           server;
    ServerVisitorImpl* visitor; 

    Impl() {
        auto v = std::make_unique<ServerVisitorImpl>();
        visitor = v.get();
        server.SetVisitor(std::move(v)); 
    }
};

Server::Server() : impl_(std::make_unique<Impl>()) {}
Server::~Server() = default;

void Server::WebTransport(const std::string& path, SessionHandler handler) {
    impl_->visitor->handlers[path] = std::move(handler);
}

void Server::listen(const std::string& host, uint16_t port,
                    const std::string& cert_file,
                    const std::string& key_file,
                    const std::string& secret) {
    if (!impl_->server.ListenFromFiles(host, port, cert_file, key_file, secret))
        throw std::runtime_error("web_transport::Server: listen failed");
    impl_->server.Run();
}

void Server::listen_async(const std::string& host, uint16_t port,
                          const std::string& cert_file,
                          const std::string& key_file,
                          const std::string& secret) {
    if (!impl_->server.ListenFromFiles(host, port, cert_file, key_file, secret))
        throw std::runtime_error("web_transport::Server: listen failed");
    impl_->server.Start();
}

void Server::stop()           { impl_->server.Stop(); }
void Server::shutdown()       { impl_->server.Shutdown(); }
void Server::run_once(int ms) { impl_->server.RunOnce(ms); }

// ─── WebTransport (client) Impl ──────────────────────────────────────────────

static bool parse_url(const std::string& url,
                       std::string& host, uint16_t& port, std::string& path) {
    auto colon = url.rfind(':');
    auto slash = url.find('/', colon == std::string::npos ? 0 : colon);
    if (colon == std::string::npos) return false;

    host = url.substr(0, colon);
    std::string port_str = (slash != std::string::npos)
        ? url.substr(colon + 1, slash - colon - 1)
        : url.substr(colon + 1);
    path = (slash != std::string::npos) ? url.substr(slash) : "/";

    try { port = static_cast<uint16_t>(std::stoi(port_str)); }
    catch (...) { return false; }
    return true;
}

struct ClientVisitorImpl : public wt::ClientVisitor {
    std::mutex              mu;
    std::condition_variable cv;

    Session* ready_session = nullptr;  
    bool     failed        = false;

    WebTransport::ConnectCallback async_cb;

    void OnConnected() override {}

    void OnWebTransportReady(wt::Session* s) override {
        auto impl = std::make_unique<SessionImpl>(s);
        impl->mark_transferred();
        Session* raw = impl.get();

        s->SetVisitor(std::move(impl));

        {
            std::lock_guard lk(mu);
            ready_session = raw;
        }
        cv.notify_all();

        if (async_cb) async_cb(raw);
    }

    void OnConnectionFailed(const std::string&) override {
        {
            std::lock_guard lk(mu);
            failed = true;
        }
        cv.notify_all();
        if (async_cb) async_cb(nullptr);
    }
};

struct WebTransport::Impl {
    std::string  host;
    uint16_t     port = 0;
    std::string  path;
    ClientConfig cfg;

    wt::Client                           client;
    ClientVisitorImpl* visitor; 

    Impl(std::string url, ClientConfig c) : cfg(std::move(c)) {
        if (!parse_url(url, host, port, path))
            throw std::runtime_error("web_transport: invalid url: " + url);

        wt::ClientConfig wc;
        wc.verify_certificate = cfg.verify_certificate;
        for (auto& fp : cfg.fingerprints) {
            wt::CertificateFingerprint f;
            f.algorithm = "sha-256";
            f.value     = fp;
            wc.certificate_fingerprints.push_back(std::move(f));
        }
        wc.protocols = cfg.protocols;

        client.SetConfig(wc);
        
        auto v = std::make_unique<ClientVisitorImpl>();
        visitor = v.get();
        client.SetVisitor(std::move(v)); 
    }
};

WebTransport::WebTransport(std::string url, ClientConfig cfg)
    : impl_(std::make_unique<Impl>(std::move(url), std::move(cfg))) {}

WebTransport::~WebTransport() = default;

Session* WebTransport::connect(int timeout_ms) {
    auto& im = *impl_;
    if (!im.client.Connect(im.host, im.port, im.path))
        return nullptr;

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    std::unique_lock lk(im.visitor->mu);
    while (!im.visitor->ready_session && !im.visitor->failed) {
        lk.unlock();
        im.client.RunOnce(10);
        lk.lock();
        if (std::chrono::steady_clock::now() >= deadline) break;
    }
    return im.visitor->ready_session;
}

void WebTransport::connect_async(ConnectCallback cb) {
    auto& im = *impl_;
    im.visitor->async_cb = std::move(cb);
    if (!im.client.Connect(im.host, im.port, im.path)) {
        if (im.visitor->async_cb) im.visitor->async_cb(nullptr);
        return;
    }
    im.client.Start();
}

void WebTransport::run_once(int ms) { impl_->client.RunOnce(ms); }
void WebTransport::shutdown()       { impl_->client.Shutdown(); }

} // namespace web_transport