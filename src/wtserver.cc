// new libwebtransport (pure C++)
#include "wtserver.h"

#include <fstream>
#include <sstream>

#include "quiche/quic/core/crypto/proof_source_x509.h"
#include "quiche/quic/core/http/spdy_utils.h"
#include "quiche/quic/core/http/web_transport_http3.h"
#include "quiche/quic/core/http/quic_server_initiated_spdy_stream.h"
#include "quiche/quic/core/quic_connection.h"
#include "quiche/quic/core/quic_utils.h"
#include "quiche/quic/tools/quic_simple_crypto_server_stream_helper.h"
#include "quiche/common/platform/api/quiche_reference_counted.h"
#include "quiche/web_transport/web_transport_headers.h"

namespace wt {

// ─── WTServerBackend ─────────────────────────────────────────────────────────

WTRespPromisePtr WTServerBackend::ProcessWebTransportRequest(
    const quiche::HttpHeaderBlock& req_headers,
    quic::WebTransportSession* quic_session,
    const quic::QuicSocketAddress& peer_addr) {

    auto promise = std::make_shared<WTRespPromise>();

    auto respond = [&](int status) {
        auto resp = std::make_unique<WTWebTransportResponse>();
        resp->response_headers[":status"] = std::to_string(status);
        promise->Resolve(std::move(resp));
    };

    auto path_it = req_headers.find(":path");
    std::string path = (path_it != req_headers.end())
                           ? std::string(path_it->second) : "";
    std::string peer_str = peer_addr.ToString();

    std::string requested_proto;
    auto proto_it = req_headers.find("wt-available-protocols");
    if (proto_it != req_headers.end()) {
        auto parsed = webtransport::ParseSubprotocolRequestHeader(proto_it->second);
        if (parsed.ok() && !parsed->empty())
            requested_proto = parsed->front();
    }

    ServerVisitor* sv = GetServerVisitor();

    if (sv) {
        auto wtsession = std::make_shared<WTSession>(quic_session, peer_str);
        wtsession->SetCloseCallback([this](WTSession* s) {
            active_sessions_.erase(
                std::remove_if(active_sessions_.begin(), active_sessions_.end(),
                               [s](const std::shared_ptr<WTSession>& p) {
                                   return p.get() == s;
                               }),
                active_sessions_.end());
        });

        auto session_visitor = sv->OnNewSession(
            wtsession.get(), path, peer_str, requested_proto);

        if (!session_visitor) { respond(404); return promise; }

        wtsession->SetVisitor(std::move(session_visitor));
        active_sessions_.push_back(wtsession);

        auto resp = std::make_unique<WTWebTransportResponse>();
        resp->response_headers[":status"] = "200";
        resp->visitor = std::make_unique<WTSessionBridge>(wtsession);
        promise->Resolve(std::move(resp));
        return promise;
    }

    if (paths_.count(path)) {
        auto wtsession = std::make_shared<WTSession>(quic_session, peer_str);
        wtsession->SetCloseCallback([this](WTSession* s) {
            active_sessions_.erase(
                std::remove_if(active_sessions_.begin(), active_sessions_.end(),
                               [s](const std::shared_ptr<WTSession>& p) {
                                   return p.get() == s;
                               }),
                active_sessions_.end());
        });
        active_sessions_.push_back(wtsession);

        auto resp = std::make_unique<WTWebTransportResponse>();
        resp->response_headers[":status"] = "200";
        resp->visitor = std::make_unique<WTSessionBridge>(wtsession);
        promise->Resolve(std::move(resp));
        return promise;
    }

    respond(404);
    return promise;
}

// ─── WTServerStream ──────────────────────────────────────────────────────────

WTServerStream::WTServerStream(quic::QuicStreamId id,
                                quic::QuicSpdySession* session,
                                quic::StreamType type,
                                WTServerBackend* backend)
    : quic::QuicSpdyServerStreamBase(id, session, type), backend_(backend) {}

WTServerStream::WTServerStream(quic::PendingStream* pending,
                                quic::QuicSpdySession* session,
                                WTServerBackend* backend)
    : quic::QuicSpdyServerStreamBase(pending, session), backend_(backend) {}

WTServerStream::~WTServerStream() {
    for (auto& p : pending_promises_) {
        auto resp = std::make_unique<WTWebTransportResponse>();
        resp->response_headers[":status"] = "500";
        p->Resolve(std::move(resp));
    }
}

void WTServerStream::OnInitialHeadersComplete(
    bool fin, size_t frame_len, const quic::QuicHeaderList& headers) {
    quic::QuicSpdyStream::OnInitialHeadersComplete(fin, frame_len, headers);
    if (!response_sent_ &&
        !quic::SpdyUtils::CopyAndValidateHeaders(
            headers, &content_length_, &request_headers_)) {
        SendErrorResponse(400);
    }
    ConsumeHeaderList();
    if (!fin && !response_sent_) {
        auto it = request_headers_.find(":method");
        if (it != request_headers_.end() &&
            absl::StartsWith(it->second, "CONNECT"))
            SendResponse();
    }
}

void WTServerStream::OnTrailingHeadersComplete(
    bool, size_t, const quic::QuicHeaderList&) { SendErrorResponse(400); }

void WTServerStream::OnBodyAvailable() {
    while (HasBytesToRead()) {
        struct iovec iov;
        if (GetReadableRegions(&iov, 1) == 0) break;
        body_.append(static_cast<char*>(iov.iov_base), iov.iov_len);
        if (content_length_ >= 0 &&
            body_.size() > static_cast<uint64_t>(content_length_)) {
            SendErrorResponse(); return;
        }
        MarkConsumed(iov.iov_len);
    }
    if (!sequencer()->IsClosed()) { sequencer()->SetUnblocked(); return; }
    OnFinRead();
    if (!write_side_closed() && !fin_buffered()) SendResponse();
}

void WTServerStream::OnCanWrite() { quic::QuicSpdyStream::OnCanWrite(); }
void WTServerStream::OnInvalidHeaders() { SendErrorResponse(400); }

void WTServerStream::SendResponse() {
    if (request_headers_.empty() || response_sent_) {
        if (!response_sent_) SendErrorResponse();
        return;
    }
    if (web_transport() != nullptr) {
        quic::QuicSocketAddress peer = spdy_session()->connection()->peer_address();
        WTRespPromisePtr prom =
            backend_->ProcessWebTransportRequest(request_headers_, web_transport(), peer);
        pending_promises_.insert(prom);
        prom->Finally([this, prom](WTWebTransportResponse* resp) {
            pending_promises_.erase(prom);
            if (resp->response_headers[":status"] == "200") {
                WriteHeaders(std::move(resp->response_headers), false, nullptr);
                if (resp->visitor) {
                    static_cast<WTServerSession*>(session())->AddSessionVisitor(
                        id(), resp->visitor.get());
                    web_transport()->SetVisitor(std::move(resp->visitor));
                }
                web_transport()->HeadersReceived(request_headers_);
            } else {
                WriteHeaders(std::move(resp->response_headers), true, nullptr);
            }
        });
        return;
    }
    SendNotFoundResponse();
}

void WTServerStream::SendErrorResponse(int code) {
    if (!reading_stopped()) StopReading();
    quiche::HttpHeaderBlock h;
    h[":status"] = (code > 0) ? std::to_string(code) : "500";
    WriteHeaders(std::move(h), true, nullptr);
    response_sent_ = true;
}

void WTServerStream::SendNotFoundResponse() {
    quiche::HttpHeaderBlock h;
    h[":status"] = "404";
    WriteHeaders(std::move(h), true, nullptr);
    response_sent_ = true;
}

// ─── WTServerSession ─────────────────────────────────────────────────────────

WTServerSession::WTServerSession(
    const quic::QuicConfig& config,
    const quic::ParsedQuicVersionVector& versions,
    quic::QuicConnection* connection,
    quic::QuicSession::Visitor* visitor,
    quic::QuicCryptoServerStreamBase::Helper* helper,
    const quic::QuicCryptoServerConfig* crypto_config,
    quic::QuicCompressedCertsCache* certs_cache,
    WTServerBackend* backend)
    : quic::QuicServerSessionBase(config, versions, connection, visitor,
                                  helper, crypto_config, certs_cache,
                                  quic::QuicPriorityType::kWebTransport),
      backend_(backend) {}

WTServerSession::~WTServerSession() { DeleteConnection(); }

std::unique_ptr<quic::QuicCryptoServerStreamBase>
WTServerSession::CreateQuicCryptoServerStream(
    const quic::QuicCryptoServerConfig* cfg,
    quic::QuicCompressedCertsCache* cache) {
    return CreateCryptoServerStream(cfg, cache, this, stream_helper());
}

void WTServerSession::OnStreamFrame(const quic::QuicStreamFrame& frame) {
    if (!IsIncomingStream(frame.stream_id) && !WillNegotiateWebTransport()) {
        connection()->CloseConnection(
            quic::QUIC_INVALID_STREAM_ID, "client sent data on server push stream",
            quic::ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
        return;
    }
    quic::QuicSpdySession::OnStreamFrame(frame);
}

void WTServerSession::OnCanCreateNewOutgoingStream(bool unidirectional) {
    if (!SupportsWebTransport()) return;
    for (auto& [id, v] : session_visitors_) {
        if (unidirectional) v->OnCanCreateNewOutgoingUnidirectionalStream();
        else                v->OnCanCreateNewOutgoingBidirectionalStream();
    }
}

quic::QuicSpdyStream* WTServerSession::CreateIncomingStream(quic::QuicStreamId id) {
    if (!ShouldCreateIncomingStream(id)) return nullptr;
    auto* s = new WTServerStream(id, this, quic::BIDIRECTIONAL, backend_);
    ActivateStream(absl::WrapUnique(s));
    return s;
}

quic::QuicSpdyStream* WTServerSession::CreateIncomingStream(quic::PendingStream* pending) {
    auto* s = new WTServerStream(pending, this, backend_);
    ActivateStream(absl::WrapUnique(s));
    return s;
}

quic::QuicSpdyStream* WTServerSession::CreateOutgoingBidirectionalStream() {
    if (!WillNegotiateWebTransport() || !ShouldCreateOutgoingBidirectionalStream())
        return nullptr;
    auto* s = new quic::QuicServerInitiatedSpdyStream(
        GetNextOutgoingBidirectionalStreamId(), this, quic::BIDIRECTIONAL);
    ActivateStream(absl::WrapUnique(s));
    return s;
}

// ─── WTDispatcher ────────────────────────────────────────────────────────────

WTDispatcher::WTDispatcher(
    const quic::QuicConfig* config,
    const quic::QuicCryptoServerConfig* crypto_config,
    quic::QuicVersionManager* version_manager,
    std::unique_ptr<quic::QuicConnectionHelperInterface> helper,
    std::unique_ptr<quic::QuicCryptoServerStreamBase::Helper> sh,
    std::unique_ptr<quic::QuicAlarmFactory> af,
    WTServerBackend* backend,
    uint8_t conn_id_len,
    quic::ConnectionIdGeneratorInterface& id_gen)
    : quic::QuicDispatcher(config, crypto_config, version_manager,
                           std::move(helper), std::move(sh), std::move(af),
                           conn_id_len, id_gen),
      backend_(backend) {}

std::unique_ptr<quic::QuicSession> WTDispatcher::CreateQuicSession(
    quic::QuicConnectionId connection_id,
    const quic::QuicSocketAddress& self_address,
    const quic::QuicSocketAddress& peer_address,
    absl::string_view,
    const quic::ParsedQuicVersion& version,
    const quic::ParsedClientHello&,
    quic::ConnectionIdGeneratorInterface& id_gen) {

    auto* conn = new quic::QuicConnection(
        connection_id, self_address, peer_address,
        helper(), alarm_factory(), writer(),
        /*owns_writer=*/false, quic::Perspective::IS_SERVER,
        quic::ParsedQuicVersionVector{version}, id_gen);

    auto session = std::make_unique<WTServerSession>(
        config(), GetSupportedVersions(), conn,
        this, session_helper(), crypto_config(),
        compressed_certs_cache(), backend_);
    session->Initialize();
    return session;
}

// ─── Server::Impl ────────────────────────────────────────────────────────────

Server::Impl::Impl()
    : packet_reader_(std::make_unique<quic::QuicPacketReader>()),
      version_manager_({quic::ParsedQuicVersion::RFCv1()}),
      conn_id_gen_(kConnIdLen) {}

Server::Impl::~Impl() {
    if (fd_ != quic::kQuicInvalidSocketFd) {
        engine_.UnregisterSocket(fd_);
        DestroySocket(fd_);
    }
}

bool Server::Impl::Listen(const std::string& host, uint16_t port,
                           const ServerConfig& cfg) {
    std::stringstream cert_stream(cfg.cert_pem);
    auto chain = quiche::QuicheReferenceCountedPointer<quic::ProofSource::Chain>(
        new quic::ProofSource::Chain(
            quic::CertificateView::LoadPemFromStream(&cert_stream)));

    std::stringstream key_stream(cfg.key_pem);
    auto privkey = quic::CertificatePrivateKey::LoadPemFromStream(&key_stream);
    if (!privkey) return false;

    auto proof_source = quic::ProofSourceX509::Create(chain, std::move(*privkey));
    if (!proof_source) return false;

    crypto_config_ = std::make_unique<quic::QuicCryptoServerConfig>(
        cfg.secret, quic::QuicRandom::GetInstance(),
        std::move(proof_source), quic::KeyExchangeSource::Default());

    quic::QuicIpAddress ip;
    if (!ip.FromString(host)) return false;
    server_addr_ = quic::QuicSocketAddress(ip, port);
    fd_ = CreateServerSocket(server_addr_);
    if (fd_ == quic::kQuicInvalidSocketFd) return false;
    server_addr_ = GetLocalAddress(fd_);

    auto helper_copy    = std::make_unique<quic::QuicDefaultConnectionHelper>();
    auto session_helper = std::make_unique<quic::QuicSimpleCryptoServerStreamHelper>();
    auto af             = engine_.CreateAlarmFactory();

    dispatcher_ = std::make_unique<WTDispatcher>(
        &quic_config_, crypto_config_.get(), &version_manager_,
        std::move(helper_copy), std::move(session_helper), std::move(af),
        &backend_, kConnIdLen, conn_id_gen_);

    dispatcher_->InitializeWithWriter(new NativeSocketWriter(fd_));

    engine_.RegisterSocket(fd_,
        quic::kSocketEventReadable | quic::kSocketEventWritable, this);

    return true;
}

void Server::Impl::OnSocketEvent(quic::QuicEventLoop*,
                                  quic::QuicUdpSocketFd,
                                  quic::QuicSocketEventMask events) {
    if (events & quic::kSocketEventReadable) {
        dispatcher_->ProcessBufferedChlos(16);
        bool more = true;
        while (more) {
            more = packet_reader_->ReadAndDispatchPackets(
                fd_, server_addr_.port(),
                *quic::QuicDefaultClock::Get(), dispatcher_.get(), nullptr);
        }
        if (dispatcher_->HasChlosBuffered())
            engine_.RearmSocket(fd_, quic::kSocketEventReadable);
    }
    if (events & quic::kSocketEventWritable)
        dispatcher_->OnCanWrite();
}

void Server::Impl::RunOnce(int timeout_ms) { engine_.RunOnce(timeout_ms); }
void Server::Impl::Run()  { running_ = true; while (running_) RunOnce(50); }
void Server::Impl::Stop() { running_ = false; }

void Server::Impl::Start() {
    running_ = true;
    thread_ = std::thread([this] { while (running_) RunOnce(50); });
}

void Server::Impl::Shutdown() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (dispatcher_) dispatcher_->Shutdown();
}

// ─── Server public API ────────────────────────────────────────────────────────

Server::Server()  : impl_(std::make_unique<Impl>()) {}
Server::~Server() = default;

void Server::SetVisitor(std::unique_ptr<ServerVisitor> v) {
    impl_->backend_.SetServerVisitor(std::move(v));
}

bool Server::Listen(const std::string& host, uint16_t port, const ServerConfig& cfg) {
    return impl_->Listen(host, port, cfg);
}

bool Server::ListenFromFiles(const std::string& host, uint16_t port,
                              const std::string& cert_file,
                              const std::string& key_file,
                              const std::string& secret) {
    auto read_file = [](const std::string& path) -> std::string {
        std::ifstream f(path);
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    ServerConfig cfg;
    cfg.cert_pem = read_file(cert_file);
    cfg.key_pem  = read_file(key_file);
    cfg.secret   = secret;
    return impl_->Listen(host, port, cfg);
}

void Server::AddPath(const std::string& path) { impl_->AddPath(path); }
void Server::Run()                       { impl_->Run(); }
void Server::RunOnce(int timeout_ms)     { impl_->RunOnce(timeout_ms); }
void Server::Stop()                      { impl_->Stop(); }
void Server::Start()                     { impl_->Start(); }
void Server::Shutdown()                  { impl_->Shutdown(); }

} // namespace wt