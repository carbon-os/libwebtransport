// new libwebtransport (pure C++)
#include "wtclient.h"

#include "quiche/quic/core/crypto/proof_verifier.h"
#include "quiche/quic/core/http/spdy_utils.h"
#include "quiche/quic/core/http/web_transport_http3.h"
#include "quiche/quic/core/quic_connection.h"
#include "quiche/quic/core/quic_default_clock.h"
#include "quiche/quic/core/quic_utils.h"
#include "quiche/web_transport/web_transport_headers.h"

namespace wt {

class InsecureProofVerifier : public quic::ProofVerifier {
public:
    quic::QuicAsyncStatus VerifyProof(
        const std::string&, const uint16_t, const std::string&,
        quic::QuicTransportVersion, absl::string_view,
        const std::vector<std::string>&, const std::string&, const std::string&,
        const quic::ProofVerifyContext*, std::string*,
        std::unique_ptr<quic::ProofVerifyDetails>*,
        std::unique_ptr<quic::ProofVerifierCallback>) override {
        return quic::QUIC_SUCCESS;
    }
    quic::QuicAsyncStatus VerifyCertChain(
        const std::string&, const uint16_t, const std::vector<std::string>&,
        const std::string&, const std::string&, const quic::ProofVerifyContext*,
        std::string*, std::unique_ptr<quic::ProofVerifyDetails>*,
        uint8_t*, std::unique_ptr<quic::ProofVerifierCallback>) override {
        return quic::QUIC_SUCCESS;
    }
    std::unique_ptr<quic::ProofVerifyContext> CreateDefaultContext() override {
        return nullptr;
    }
};

class FingerprintProofVerifier
    : public quic::WebTransportFingerprintProofVerifier {
public:
    using quic::WebTransportFingerprintProofVerifier::
        WebTransportFingerprintProofVerifier;
protected:
    bool IsKeyTypeAllowedByPolicy(const quic::CertificateView& cert) override {
        if (cert.public_key_type() == quic::PublicKeyType::kRsa) return false;
        return WebTransportFingerprintProofVerifier::IsKeyTypeAllowedByPolicy(cert);
    }
};

// ─── WTSessionCache ──────────────────────────────────────────────────────────

void WTSessionCache::Insert(const quic::QuicServerId& id,
                             bssl::UniquePtr<SSL_SESSION> session,
                             const quic::TransportParameters& params,
                             const quic::ApplicationState* app_state) {
    auto& e = entries_[id];
    if (session)   e.session   = std::move(session);
    if (app_state) e.app_state = std::make_unique<quic::ApplicationState>(*app_state);
    e.params = std::make_unique<quic::TransportParameters>(params);
}

std::unique_ptr<quic::QuicResumptionState>
WTSessionCache::Lookup(const quic::QuicServerId& id,
                        quic::QuicWallTime, const SSL_CTX*) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return nullptr;
    if (!it->second.session) { entries_.erase(it); return nullptr; }
    auto state = std::make_unique<quic::QuicResumptionState>();
    state->tls_session      = std::move(it->second.session);
    state->transport_params = std::make_unique<quic::TransportParameters>(*it->second.params);
    state->token            = it->second.token;
    if (it->second.app_state)
        state->application_state =
            std::make_unique<quic::ApplicationState>(*it->second.app_state);
    return state;
}

void WTSessionCache::OnNewTokenReceived(const quic::QuicServerId& id,
                                         absl::string_view token) {
    auto it = entries_.find(id);
    if (it != entries_.end()) it->second.token = std::string(token);
}

// ─── WTClientStream ──────────────────────────────────────────────────────────

void WTClientStream::OnBodyAvailable() {
    while (HasBytesToRead()) {
        struct iovec iov;
        if (GetReadableRegions(&iov, 1) == 0) break;
        MarkConsumed(iov.iov_len);
    }
    if (sequencer()->IsClosed()) OnFinRead();
    else                         sequencer()->SetUnblocked();
}

// ─── WTClientSession ─────────────────────────────────────────────────────────

WTClientSession::WTClientSession(
    const quic::QuicConfig& config,
    const quic::ParsedQuicVersionVector& versions,
    quic::QuicConnection* connection,
    const quic::QuicServerId& server_id,
    quic::QuicCryptoClientConfig* crypto_config)
    : quic::QuicSpdyClientSession(config, versions, connection,
                                  server_id, crypto_config,
                                  quic::QuicPriorityType::kWebTransport) {}

std::unique_ptr<quic::QuicSpdyClientStream>
WTClientSession::CreateClientStream() {
    return std::make_unique<WTClientStream>(
        GetNextOutgoingBidirectionalStreamId(), this, quic::BIDIRECTIONAL);
}

void WTClientSession::OnCanCreateNewOutgoingStream(bool unidirectional) {
    if (!SupportsWebTransport()) return;
    for (auto& [id, v] : session_visitors_) {
        if (unidirectional) v->OnCanCreateNewOutgoingUnidirectionalStream();
        else                v->OnCanCreateNewOutgoingBidirectionalStream();
    }
}

// ─── Client::Impl ────────────────────────────────────────────────────────────

Client::Impl::Impl()
    : packet_reader_(std::make_unique<quic::QuicPacketReader>()),
      conn_id_gen_(kConnIdLen),
      supported_versions_({quic::ParsedQuicVersion::RFCv1()}) {}

Client::Impl::~Impl() {
    Shutdown(); // Ensures the background thread joins safely
    if (fd_ != quic::kQuicInvalidSocketFd) {
        engine_.UnregisterSocket(fd_);
        DestroySocket(fd_);
    }
}

bool Client::Impl::Connect(const std::string& host, uint16_t port,
                             const std::string& path,
                             const std::string& server_hostname) {
    wt_path_ = path;

    std::unique_ptr<quic::ProofVerifier> verifier;
    if (!config_.certificate_fingerprints.empty()) {
        auto fv = std::make_unique<FingerprintProofVerifier>(
            engine_.helper()->GetClock(), 14);
        for (auto& fp : config_.certificate_fingerprints) {
            quic::WebTransportHash h;
            h.algorithm = fp.algorithm;
            h.value     = fp.value;
            if (!fv->AddFingerprint(h)) return false;
        }
        verifier = std::move(fv);
    } else {
        verifier = std::make_unique<InsecureProofVerifier>();
    }

    crypto_config_ = std::make_unique<quic::QuicCryptoClientConfig>(
        std::move(verifier), std::make_unique<WTSessionCache>());
    crypto_config_->set_user_agent_id("libwebtransport/1.0");

    quic::QuicIpAddress ip;
    if (!ip.FromString(host)) return false;
    server_addr_ = quic::QuicSocketAddress(ip, port);

    std::string sni = server_hostname.empty() ? host : server_hostname;
    server_id_ = quic::QuicServerId(sni, port);

    int family = ip.IsIPv4() ? AF_INET : AF_INET6;
    fd_ = CreateClientSocket(family, &local_addr_);
    if (fd_ == quic::kQuicInvalidSocketFd) return false;

    engine_.RegisterSocket(fd_,
        quic::kSocketEventReadable | quic::kSocketEventWritable, this);

    connection_in_progress_ = true;
    connection_recheck_     = true;
    return true;
}

bool Client::Impl::StartConnect() {
    auto* conn = new quic::QuicConnection(
        quic::QuicUtils::CreateRandomConnectionId(kConnIdLen),
        quic::QuicSocketAddress(), server_addr_,
        engine_.helper(), engine_.alarm_factory(),
        new NativeSocketWriter(fd_),
        /*owns_writer=*/true,
        quic::Perspective::IS_CLIENT,
        supported_versions_, conn_id_gen_);

    session_ = std::make_unique<WTClientSession>(
        quic_config_, supported_versions_, conn,
        server_id_, crypto_config_.get());

    session_->Initialize();
    session_->CryptoConnect();
    connect_attempted_ = true;
    return true;
}

bool Client::Impl::IsConnected() const {
    return session_ && session_->connection() &&
           session_->connection()->connected();
}

bool Client::Impl::EncryptionBeingEstablished() const {
    return session_ && !session_->IsEncryptionEstablished() &&
           session_->connection()->connected();
}

bool Client::Impl::HandleConnecting() {
    bool recheck = false;
    if (connection_in_progress_) {
        if (!wait_for_encryption_) {
            if (!IsConnected() &&
                num_connect_attempts_ <=
                    quic::QuicCryptoClientStream::kMaxClientHellos) {
                StartConnect();
                wait_for_encryption_ = true;
            }
        }
        if (wait_for_encryption_) {
            if (EncryptionBeingEstablished()) return true;
            wait_for_encryption_ = false;
            if (IsConnected()) {
                connection_in_progress_ = false;
                NotifyConnected(true);
                wt_support_notify_ = true;
                recheck = true;
            } else {
                num_connect_attempts_++;
                recheck = true;
            }
        }
    }
    if (wt_support_notify_ && IsConnected()) {
        if (session_->SupportsWebTransport()) {
            quiche::HttpHeaderBlock headers;
            headers[":scheme"]    = "https";
            headers[":authority"] = server_id_.host();
            headers[":path"]      = wt_path_;
            headers[":method"]    = "CONNECT";
            headers[":protocol"]  = "webtransport";
            if (!config_.protocols.empty()) {
                auto ser = webtransport::SerializeSubprotocolRequestHeader(
                    config_.protocols);
                if (ser.ok()) headers["wt-available-protocols"] = *ser;
            }
            auto* stream = static_cast<quic::QuicSpdyClientStream*>(
                session_->CreateOutgoingBidirectionalStream());
            if (stream) {
                wt_stream_id_      = stream->id();
                wt_stream_created_ = true;
                stream->set_visitor(nullptr);
                stream->SendRequest(std::move(headers), "", /*fin=*/false);
            }
            wt_support_notify_ = false;
            recheck = true;
        } else {
            recheck = true;
        }
    }
    return recheck;
}

bool Client::Impl::CheckForWebTransportSession() {
    if (!session_ || active_session_ || !wt_stream_created_) return false;

    auto* wt = session_->GetWebTransportSession(wt_stream_id_);
    if (!wt) return false;

    auto wtsession = std::make_shared<WTSession>(
        static_cast<quic::WebTransportSession*>(wt),
        server_addr_.ToString());
    wtsession->SetCloseCallback([this](WTSession*) { active_session_.reset(); });

    auto bridge = std::make_unique<WTSessionBridge>(wtsession);
    static_cast<WTClientSession*>(session_.get())->AddSessionVisitor(
        wt_stream_id_, bridge.get());
    wt->SetVisitor(std::move(bridge));

    active_session_ = wtsession;
    NotifyWebTransportReady(wtsession.get());
    return false;
}

void Client::Impl::NotifyConnected(bool success, const std::string& reason) {
    if (!visitor_) return;
    if (success) visitor_->OnConnected();
    else         visitor_->OnConnectionFailed(reason);
}

void Client::Impl::NotifyWebTransportReady(WTSession* session) {
    if (visitor_) visitor_->OnWebTransportReady(session);
}

void Client::Impl::ProcessPacket(
    const quic::QuicSocketAddress& self_addr,
    const quic::QuicSocketAddress& peer_addr,
    const quic::QuicReceivedPacket& pkt) {
    if (session_) session_->ProcessUdpPacket(self_addr, peer_addr, pkt);
}

void Client::Impl::OnSocketEvent(quic::QuicEventLoop*,
                                  quic::QuicUdpSocketFd,
                                  quic::QuicSocketEventMask events) {
    if (events & quic::kSocketEventReadable) {
        bool more = true;
        while (more) {
            more = packet_reader_->ReadAndDispatchPackets(
                fd_, local_addr_.port(),
                *quic::QuicDefaultClock::Get(), this, nullptr);
        }
    }
    if (events & quic::kSocketEventWritable) {
        if (session_ && IsConnected())
            session_->connection()->OnCanWrite();
    }
    if (connection_recheck_)
        connection_recheck_ = HandleConnecting();
    CheckForWebTransportSession();
}

void Client::Impl::RunOnce(int timeout_ms) { engine_.RunOnce(timeout_ms); }
void Client::Impl::Run()  { running_ = true; while (running_) RunOnce(50); }
void Client::Impl::Stop() { running_ = false; }

void Client::Impl::Start() {
    running_ = true;
    thread_  = std::thread([this] { while (running_) RunOnce(50); });
}

void Client::Impl::Shutdown() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    Close();
}

void Client::Impl::Close() {
    if (session_ && IsConnected()) {
        session_->connection()->CloseConnection(
            quic::QUIC_PEER_GOING_AWAY, "client closing",
            quic::ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
    }
    session_.reset();
}

// ─── Client public API ────────────────────────────────────────────────────────

Client::Client()  : impl_(std::make_unique<Impl>()) {}
Client::~Client() = default;

void Client::SetConfig(const ClientConfig& cfg)          { impl_->SetConfig(cfg); }
void Client::SetVisitor(std::unique_ptr<ClientVisitor> v) { impl_->SetVisitor(std::move(v)); }
bool Client::Connect(const std::string& host, uint16_t port,
                     const std::string& path,
                     const std::string& server_hostname) {
    return impl_->Connect(host, port, path, server_hostname);
}
void Client::Close()                  { impl_->Close(); }
void Client::Run()                    { impl_->Run(); }
void Client::RunOnce(int timeout_ms)  { impl_->RunOnce(timeout_ms); }
void Client::Stop()                   { impl_->Stop(); }
void Client::Start()                  { impl_->Start(); }
void Client::Shutdown()               { impl_->Shutdown(); }

} // namespace wt