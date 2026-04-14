// internal libwebtransport base types (namespace wt)
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wt {

struct StreamPriority {
    uint64_t send_group_id = 0;
    uint64_t send_order    = 0;
};

class StreamVisitor {
public:
    virtual ~StreamVisitor() = default;
    virtual void OnCanRead()  {}
    virtual void OnCanWrite() {}
    virtual void OnResetStreamReceived(uint32_t error_code) {}
    virtual void OnStopSendingReceived(uint32_t error_code) {}
    virtual void OnWriteSideClosed() {}
};

class Stream {
public:
    virtual ~Stream() = default;
    virtual bool   Write(const uint8_t* data, size_t len) = 0;
    virtual bool   FinWrite() = 0;
    virtual void   ResetStream(uint32_t error_code = 0) = 0;
    virtual void   StopSending(uint32_t error_code = 0) = 0;
    virtual size_t Read(uint8_t* dst, size_t len) = 0;
    virtual size_t ReadableBytes() const = 0;
    virtual uint64_t id() const = 0;
    virtual void   SetPriority(const StreamPriority& p) = 0;
    virtual void   SetVisitor(std::unique_ptr<StreamVisitor> v) = 0;
};

class SessionVisitor {
public:
    virtual ~SessionVisitor() = default;
    virtual void OnSessionReady(const std::string& negotiated_subprotocol) {}
    virtual void OnSessionClosed(uint32_t error_code, const std::string& error_message) {}
    virtual void OnIncomingBidirectionalStream(Stream*) {}
    virtual void OnIncomingUnidirectionalStream(Stream*) {}
    virtual void OnDatagramReceived(const uint8_t* data, size_t len) {}
    virtual void OnCanCreateNewOutgoingBidirectionalStream()  {}
    virtual void OnCanCreateNewOutgoingUnidirectionalStream() {}
};

class Session {
public:
    virtual ~Session() = default;
    virtual Stream* OpenBidirectionalStream()  = 0;
    virtual Stream* OpenUnidirectionalStream() = 0;
    virtual bool    SendDatagram(const uint8_t* data, size_t len) = 0;
    virtual void    Close(uint32_t error_code = 0, const std::string& reason = "") = 0;
    virtual bool        IsReady()     const = 0;
    virtual std::string PeerAddress() const = 0;
    virtual void SetVisitor(std::unique_ptr<SessionVisitor> v) = 0;
};

// ─── Server ──────────────────────────────────────────────────────────────────

struct ServerConfig {
    std::string cert_pem;
    std::string key_pem;
    std::string secret = "webtransport_server_secret";
};

class ServerVisitor {
public:
    virtual ~ServerVisitor() = default;
    virtual std::unique_ptr<SessionVisitor> OnNewSession(
        Session*           session,
        const std::string& path,
        const std::string& peer_address,
        const std::string& requested_protocol) = 0;
};

class Server {
public:
    Server();
    ~Server();

    void SetVisitor(std::unique_ptr<ServerVisitor> v);

    bool Listen(const std::string& host, uint16_t port, const ServerConfig& cfg);
    bool ListenFromFiles(const std::string& host, uint16_t port,
                         const std::string& cert_file,
                         const std::string& key_file,
                         const std::string& secret = "webtransport_server_secret");

    void AddPath(const std::string& path);
    void Run();
    void RunOnce(int timeout_ms = 50);
    void Stop();
    void Start();
    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ─── Client ──────────────────────────────────────────────────────────────────

struct CertificateFingerprint {
    std::string algorithm = "sha-256";
    std::string value;
};

struct ClientConfig {
    bool verify_certificate = true;
    std::vector<CertificateFingerprint> certificate_fingerprints;
    std::vector<std::string> protocols;
};

class ClientVisitor {
public:
    virtual ~ClientVisitor() = default;
    virtual void OnConnected() {}
    virtual void OnWebTransportReady(Session* session) {}
    virtual void OnConnectionFailed(const std::string& reason) {}
};

class Client {
public:
    Client();
    ~Client();

    void SetConfig(const ClientConfig& cfg);
    void SetVisitor(std::unique_ptr<ClientVisitor> v);

    bool Connect(const std::string& host, uint16_t port,
                 const std::string& path,
                 const std::string& server_hostname = "");
    void Close();
    void Run();
    void RunOnce(int timeout_ms = 50);
    void Stop();
    void Start();
    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wt