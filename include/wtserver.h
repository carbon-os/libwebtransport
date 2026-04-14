// new libwebtransport
#pragma once

#include "wt_base.h"
#include "wtengine.h"
#include "wtsession.h"

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <thread>

#include "quiche/quic/core/crypto/proof_source.h"
#include "quiche/quic/core/crypto/quic_crypto_server_config.h"
#include "quiche/quic/core/deterministic_connection_id_generator.h"
#include "quiche/quic/core/http/quic_server_session_base.h"
#include "quiche/quic/core/http/quic_spdy_server_stream_base.h"
#include "quiche/quic/core/quic_dispatcher.h"
#include "quiche/quic/core/quic_packet_reader.h"
#include "quiche/quic/core/quic_types.h"
#include "quiche/quic/core/quic_versions.h"
#include "quiche/quic/core/web_transport_interface.h"
#include "quiche/common/http/http_header_block.h"
#include "quiche/quic/core/quic_config.h"

namespace wt {

// ─── Promise helper ──────────────────────────────────────────────────────────

template <typename T>
class WTPromise {
public:
    void Finally(std::function<void(T*)> fn) {
        if (result_) fn(result_.get());
        else         callbacks_.push_back(std::move(fn));
    }
    void Resolve(std::unique_ptr<T> v) {
        if (result_) return;
        result_ = std::move(v);
        for (auto& fn : callbacks_) fn(result_.get());
        callbacks_.clear();
    }
private:
    std::unique_ptr<T>                 result_;
    std::list<std::function<void(T*)>> callbacks_;
};

// ─── WTServerBackend ─────────────────────────────────────────────────────────

struct WTWebTransportResponse {
    quiche::HttpHeaderBlock             response_headers;
    std::unique_ptr<quic::WebTransportVisitor> visitor;
};

using WTRespPromise    = WTPromise<WTWebTransportResponse>;
using WTRespPromisePtr = std::shared_ptr<WTRespPromise>;

class WTServerBackend {
public:
    WTServerBackend() = default;

    // Takes ownership of the visitor (fixes the prior memory leak).
    void SetServerVisitor(std::unique_ptr<ServerVisitor> sv) {
        server_visitor_owner_ = std::move(sv);
    }
    ServerVisitor* GetServerVisitor() const { return server_visitor_owner_.get(); }

    void AddPath(const std::string& path) { paths_.insert(path); }
    bool SupportsWebTransport() const     { return true; }

    WTRespPromisePtr ProcessWebTransportRequest(
        const quiche::HttpHeaderBlock& request_headers,
        quic::WebTransportSession* session,
        const quic::QuicSocketAddress& peer_address);

private:
    std::unique_ptr<ServerVisitor>          server_visitor_owner_;
    std::set<std::string>                   paths_;
    std::vector<std::shared_ptr<WTSession>> active_sessions_;
};

// ─── WTServerStream ──────────────────────────────────────────────────────────

class WTServerStream : public quic::QuicSpdyServerStreamBase {
public:
    WTServerStream(quic::QuicStreamId id, quic::QuicSpdySession* session,
                   quic::StreamType type, WTServerBackend* backend);
    WTServerStream(quic::PendingStream* pending, quic::QuicSpdySession* session,
                   WTServerBackend* backend);
    ~WTServerStream() override;

    void OnInitialHeadersComplete(bool fin, size_t frame_len,
                                  const quic::QuicHeaderList& headers) override;
    void OnTrailingHeadersComplete(bool fin, size_t frame_len,
                                   const quic::QuicHeaderList& headers) override;
    void OnBodyAvailable() override;
    void OnCanWrite() override;
    void OnInvalidHeaders() override;

protected:
    void SendResponse();
    void SendErrorResponse(int code = 500);
    void SendNotFoundResponse();

private:
    WTServerBackend*         backend_;
    quiche::HttpHeaderBlock  request_headers_;
    std::string              body_;
    int64_t                  content_length_ = -1;
    bool                     response_sent_  = false;
    std::set<WTRespPromisePtr> pending_promises_;
};

// ─── WTServerSession ─────────────────────────────────────────────────────────

class WTServerSession : public quic::QuicServerSessionBase {
public:
    WTServerSession(const quic::QuicConfig& config,
                    const quic::ParsedQuicVersionVector& versions,
                    quic::QuicConnection* connection,
                    quic::QuicSession::Visitor* visitor,
                    quic::QuicCryptoServerStreamBase::Helper* helper,
                    const quic::QuicCryptoServerConfig* crypto_config,
                    quic::QuicCompressedCertsCache* certs_cache,
                    WTServerBackend* backend);
    ~WTServerSession() override;

    void AddSessionVisitor(quic::WebTransportSessionId id,
                           quic::WebTransportVisitor* v) {
        session_visitors_.try_emplace(id, v);
    }

    void OnStreamFrame(const quic::QuicStreamFrame& frame) override;
    void OnCanCreateNewOutgoingStream(bool unidirectional) override;

protected:
    quic::QuicSpdyStream* CreateIncomingStream(quic::QuicStreamId id) override;
    quic::QuicSpdyStream* CreateIncomingStream(quic::PendingStream*) override;
    quic::QuicSpdyStream* CreateOutgoingBidirectionalStream() override;

    std::unique_ptr<quic::QuicCryptoServerStreamBase>
    CreateQuicCryptoServerStream(const quic::QuicCryptoServerConfig*,
                                 quic::QuicCompressedCertsCache*) override;

    quic::WebTransportHttp3VersionSet
    LocallySupportedWebTransportVersions() const override {
        return quic::kDefaultSupportedWebTransportVersions;
    }
    quic::HttpDatagramSupport LocalHttpDatagramSupport() override {
        return quic::HttpDatagramSupport::kRfcAndDraft04;
    }

private:
    WTServerBackend* backend_;
    absl::flat_hash_map<quic::QuicStreamId, quic::WebTransportVisitor*> session_visitors_;
};

// ─── WTDispatcher ────────────────────────────────────────────────────────────

class WTDispatcher : public quic::QuicDispatcher {
public:
    WTDispatcher(const quic::QuicConfig* config,
                 const quic::QuicCryptoServerConfig* crypto_config,
                 quic::QuicVersionManager* version_manager,
                 std::unique_ptr<quic::QuicConnectionHelperInterface> helper,
                 std::unique_ptr<quic::QuicCryptoServerStreamBase::Helper> sh,
                 std::unique_ptr<quic::QuicAlarmFactory> alarm_factory,
                 WTServerBackend* backend,
                 uint8_t conn_id_len,
                 quic::ConnectionIdGeneratorInterface& id_gen);

protected:
    std::unique_ptr<quic::QuicSession> CreateQuicSession(
        quic::QuicConnectionId             connection_id,
        const quic::QuicSocketAddress&     self_address,
        const quic::QuicSocketAddress&     peer_address,
        absl::string_view                  alpn,
        const quic::ParsedQuicVersion&     version,
        const quic::ParsedClientHello&     parsed_chlo,
        quic::ConnectionIdGeneratorInterface& id_gen) override;

private:
    WTServerBackend* backend_;
};

// ─── Server::Impl ────────────────────────────────────────────────────────────

struct Server::Impl : public quic::QuicSocketEventListener {
    Impl();
    ~Impl();

    bool Listen(const std::string& host, uint16_t port, const ServerConfig& cfg);

    void Run();
    void RunOnce(int timeout_ms);
    void Stop();
    void Start();
    void Shutdown();
    void AddPath(const std::string& path) { backend_.AddPath(path); }

    void OnSocketEvent(quic::QuicEventLoop* loop,
                       quic::QuicUdpSocketFd fd,
                       quic::QuicSocketEventMask events) override;

    WTEngine                                        engine_;
    WTServerBackend                                 backend_;
    std::unique_ptr<quic::QuicDispatcher>           dispatcher_;
    std::unique_ptr<quic::QuicPacketReader>         packet_reader_;
    std::unique_ptr<quic::QuicCryptoServerConfig>   crypto_config_;
    quic::QuicConfig                                quic_config_;
    quic::QuicVersionManager                        version_manager_;
    quic::DeterministicConnectionIdGenerator        conn_id_gen_;

    quic::QuicUdpSocketFd                           fd_          = quic::kQuicInvalidSocketFd;
    quic::QuicSocketAddress                         server_addr_;
    std::atomic<bool>                               running_{false};
    std::thread                                     thread_;

    static constexpr uint8_t kConnIdLen = quic::kQuicDefaultConnectionIdLength;
};

} // namespace wt