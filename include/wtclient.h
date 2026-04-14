// new libwebtransport
#pragma once

#include "wt_base.h"
#include "wtengine.h"
#include "wtsession.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <thread>

#include "quiche/quic/core/crypto/quic_crypto_client_config.h"
#include "quiche/quic/core/crypto/transport_parameters.h"
#include "quiche/quic/core/crypto/web_transport_fingerprint_proof_verifier.h"
#include "quiche/quic/core/deterministic_connection_id_generator.h"
#include "quiche/quic/core/http/quic_spdy_client_session.h"
#include "quiche/quic/core/http/quic_spdy_client_stream.h"
#include "quiche/quic/core/quic_config.h"
#include "quiche/quic/core/quic_packet_reader.h"
#include "quiche/quic/core/quic_server_id.h"
#include "quiche/quic/core/quic_types.h"

namespace wt {

class WTSessionCache : public quic::SessionCache {
public:
    WTSessionCache()  = default;
    ~WTSessionCache() = default;

    void Insert(const quic::QuicServerId& id,
                bssl::UniquePtr<SSL_SESSION> session,
                const quic::TransportParameters& params,
                const quic::ApplicationState* app_state) override;

    std::unique_ptr<quic::QuicResumptionState>
    Lookup(const quic::QuicServerId& id, quic::QuicWallTime now,
           const SSL_CTX* ctx) override;

    void ClearEarlyData(const quic::QuicServerId&) override {}
    void OnNewTokenReceived(const quic::QuicServerId& id,
                            absl::string_view token) override;
    void RemoveExpiredEntries(quic::QuicWallTime) override {}
    void Clear() override { entries_.clear(); }

    size_t GetSize() const override { return entries_.size(); }
    size_t GetMaxSize() const override { return 1024; }
    void UpdateMaxSize(size_t) override {}

private:
    struct Entry {
        bssl::UniquePtr<SSL_SESSION> session;
        std::unique_ptr<quic::TransportParameters> params;
        std::unique_ptr<quic::ApplicationState>    app_state;
        std::string token;
    };
    std::map<quic::QuicServerId, Entry> entries_;
};

class WTClientStream : public quic::QuicSpdyClientStream {
public:
    WTClientStream(quic::QuicStreamId id,
                   quic::QuicSpdyClientSession* session,
                   quic::StreamType type)
        : quic::QuicSpdyClientStream(id, session, type) {}
    void OnBodyAvailable() override;
};

class WTClientSession : public quic::QuicSpdyClientSession {
public:
    WTClientSession(const quic::QuicConfig& config,
                    const quic::ParsedQuicVersionVector& versions,
                    quic::QuicConnection* connection,
                    const quic::QuicServerId& server_id,
                    quic::QuicCryptoClientConfig* crypto_config);

    std::unique_ptr<quic::QuicSpdyClientStream> CreateClientStream() override;

    quic::WebTransportHttp3VersionSet
    LocallySupportedWebTransportVersions() const override {
        return quic::kDefaultSupportedWebTransportVersions;
    }
    quic::HttpDatagramSupport LocalHttpDatagramSupport() override {
        return quic::HttpDatagramSupport::kRfcAndDraft04;
    }
    void OnCanCreateNewOutgoingStream(bool unidirectional) override;

    void AddSessionVisitor(quic::WebTransportSessionId id,
                           quic::WebTransportVisitor* v) {
        session_visitors_.try_emplace(id, v);
    }

private:
    absl::flat_hash_map<quic::QuicStreamId,
                        quic::WebTransportVisitor*> session_visitors_;
};

struct Client::Impl : public quic::QuicSocketEventListener,
                      public quic::ProcessPacketInterface {
    Impl();
    ~Impl();

    void SetConfig(const ClientConfig& cfg)           { config_ = cfg; }
    void SetVisitor(std::unique_ptr<ClientVisitor> v) { visitor_ = std::move(v); }

    bool Connect(const std::string& host, uint16_t port,
                 const std::string& path,
                 const std::string& server_hostname);
    void Close();
    void Run();
    void RunOnce(int timeout_ms);
    void Stop();
    void Start();
    void Shutdown();

    void OnSocketEvent(quic::QuicEventLoop*, quic::QuicUdpSocketFd,
                       quic::QuicSocketEventMask events) override;
    void ProcessPacket(const quic::QuicSocketAddress& self_addr,
                       const quic::QuicSocketAddress& peer_addr,
                       const quic::QuicReceivedPacket& pkt) override;

    bool HandleConnecting();
    bool CheckForWebTransportSession();
    bool StartConnect();
    bool IsConnected() const;
    bool EncryptionBeingEstablished() const;

    void NotifyConnected(bool success, const std::string& reason = "");
    void NotifyWebTransportReady(WTSession* session);

    WTEngine                                        engine_;
    ClientConfig                                    config_;
    std::unique_ptr<ClientVisitor>                  visitor_;

    quic::QuicConfig                                quic_config_;
    std::unique_ptr<quic::QuicCryptoClientConfig>   crypto_config_;
    std::unique_ptr<WTClientSession>                session_;
    std::unique_ptr<quic::QuicPacketWriter>         writer_;
    std::unique_ptr<quic::QuicPacketReader>         packet_reader_;

    quic::QuicServerId                              server_id_;
    quic::QuicSocketAddress                         server_addr_;
    quic::QuicSocketAddress                         local_addr_;
    quic::DeterministicConnectionIdGenerator        conn_id_gen_;
    quic::ParsedQuicVersionVector                   supported_versions_;

    quic::QuicUdpSocketFd                           fd_ = quic::kQuicInvalidSocketFd;

    std::string                                     wt_path_;
    std::shared_ptr<WTSession>                      active_session_;

    quic::QuicStreamId                              wt_stream_id_      = 0;
    bool                                            wt_stream_created_ = false;

    bool     connect_attempted_      = false;
    bool     connection_in_progress_ = false;
    bool     wait_for_encryption_    = false;
    bool     wt_support_notify_      = false;
    bool     connection_recheck_     = false;
    uint32_t num_connect_attempts_   = 0;

    std::atomic<bool> running_{false};
    std::thread       thread_;

    static constexpr uint8_t kConnIdLen = quic::kQuicDefaultConnectionIdLength;
};

} // namespace wt