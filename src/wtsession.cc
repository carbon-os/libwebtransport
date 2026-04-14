// new libwebtransport
#include "wtsession.h"

#include <algorithm>
#include <cstring>

#include "quiche/web_transport/stream_helpers.h"

namespace wt {

WTStreamBridge::~WTStreamBridge() {
    if (stream_) { stream_->OnQuicStreamGone(); stream_ = nullptr; }
}

WTStream::WTStream(quic::WebTransportStream* qs, WTSession*)
    : quic_stream_(qs) {}
WTStream::~WTStream() = default;

uint64_t WTStream::id() const { return quic_stream_ ? quic_stream_->GetStreamId() : 0; }

bool WTStream::Write(const uint8_t* data, size_t len) {
    if (!quic_stream_ || fin_sent_ || send_fin_) return false;
    WriteChunk c;
    c.data.assign(data, data + len);
    write_queue_.push_back(std::move(c));
    TryFlushWriteQueue();
    return true;
}

bool WTStream::FinWrite() {
    if (!quic_stream_ || fin_sent_) return false;
    send_fin_ = true;
    TryFlushWriteQueue();
    return true;
}

void WTStream::TryFlushWriteQueue() {
    if (!quic_stream_) return;
    while (!write_queue_.empty()) {
        auto& front = write_queue_.front();
        auto status = webtransport::WriteIntoStream(
            *quic_stream_,
            absl::string_view(reinterpret_cast<const char*>(front.data.data()),
                              front.data.size()));
        if (!status.ok()) return;
        write_queue_.pop_front();
    }
    if (send_fin_ && !fin_sent_) {
        auto status = webtransport::SendFinOnStream(*quic_stream_);
        if (status.ok()) fin_sent_ = true;
    }
}

void WTStream::ResetStream(uint32_t ec) { if (quic_stream_) quic_stream_->ResetWithUserCode(ec); }
void WTStream::StopSending(uint32_t ec) { if (quic_stream_) quic_stream_->SendStopSending(ec); }
size_t WTStream::ReadableBytes() const  { return read_buf_.size(); }

size_t WTStream::Read(uint8_t* dst, size_t len) {
    if (read_buf_.empty()) return 0;
    size_t n = std::min(len, read_buf_.size());
    std::memcpy(dst, read_buf_.data(), n);
    read_buf_.erase(read_buf_.begin(), read_buf_.begin() + n);
    return n;
}

void WTStream::SetPriority(const StreamPriority& p) {
    if (!quic_stream_) return;
    webtransport::StreamPriority wp;
    wp.send_group_id = p.send_group_id;
    wp.send_order    = p.send_order;
    quic_stream_->SetPriority(wp);
}

void WTStream::SetVisitor(std::unique_ptr<StreamVisitor> v) { visitor_ = std::move(v); }

void WTStream::OnCanReadInternal() {
    if (!quic_stream_) return;
    while (true) {
        auto pr = quic_stream_->PeekNextReadableRegion();
        if (pr.peeked_data.empty()) {
            if (pr.fin_next) (void)quic_stream_->SkipBytes(0);
            break;
        }
        size_t len = pr.peeked_data.size();
        size_t old = read_buf_.size();
        read_buf_.resize(old + len);
        std::memcpy(read_buf_.data() + old, pr.peeked_data.data(), len);
        bool fin = quic_stream_->SkipBytes(len);
        if (fin) break;
    }
    if (visitor_ && !read_buf_.empty()) visitor_->OnCanRead();
}

void WTStream::OnCanWriteInternal() {
    TryFlushWriteQueue();
    if (visitor_ && write_queue_.empty() && !fin_sent_) visitor_->OnCanWrite();
}

void WTStream::OnResetStreamReceivedInternal(quic::WebTransportStreamError e) {
    reset_rcvd_ = true;
    if (visitor_) visitor_->OnResetStreamReceived(static_cast<uint32_t>(e));
}

void WTStream::OnStopSendingReceivedInternal(quic::WebTransportStreamError e) {
    stop_rcvd_ = true;
    send_fin_  = true;
    TryFlushWriteQueue();
    if (visitor_) visitor_->OnStopSendingReceived(static_cast<uint32_t>(e));
}

void WTStream::OnWriteSideClosedInternal() {
    if (visitor_) visitor_->OnWriteSideClosed();
}

void WTStream::OnQuicStreamGone() { quic_stream_ = nullptr; }

// ─── WTSessionBridge ─────────────────────────────────────────────────────────

WTSessionBridge::~WTSessionBridge() {
    if (session_) session_->OnQuicSessionGone();
}

void WTSessionBridge::OnSessionReady() {
    if (!session_) return;
    // Do NOT call back into the WebTransportHttp3 session here.
    // HeadersReceived() has not returned yet; re-entering WebTransportHttp3
    // via GetNegotiatedSubprotocol() corrupts its partially-initialized state.
    session_->OnSessionReadyInternal(std::nullopt);
}

void WTSessionBridge::OnSessionClosed(quic::WebTransportSessionError code,
                                       const std::string& msg) {
    if (session_) session_->OnSessionClosedInternal(code, msg);
}

void WTSessionBridge::OnIncomingBidirectionalStreamAvailable() {
    if (session_) session_->OnIncomingBidirectionalStreamInternal();
}

void WTSessionBridge::OnIncomingUnidirectionalStreamAvailable() {
    if (session_) session_->OnIncomingUnidirectionalStreamInternal();
}

void WTSessionBridge::OnDatagramReceived(absl::string_view dg) {
    if (session_) session_->OnDatagramReceivedInternal(dg);
}

void WTSessionBridge::OnCanCreateNewOutgoingBidirectionalStream() {
    if (session_) session_->OnCanCreateNewBidiStreamInternal();
}

void WTSessionBridge::OnCanCreateNewOutgoingUnidirectionalStream() {
    if (session_) session_->OnCanCreateNewUnidiStreamInternal();
}

// ─── WTSession ───────────────────────────────────────────────────────────────

WTSession::WTSession(quic::WebTransportSession* qs, const std::string& peer)
    : quic_session_(qs), peer_address_(peer) {
    if (quic_session_) quic_session_->SetOnDraining([](){});
}

WTSession::~WTSession() = default;

Stream* WTSession::OpenBidirectionalStream() {
    if (!quic_session_ || !quic_session_->CanOpenNextOutgoingBidirectionalStream())
        return nullptr;
    auto* qs = quic_session_->OpenOutgoingBidirectionalStream();
    return qs ? RegisterStream(qs) : nullptr;
}

Stream* WTSession::OpenUnidirectionalStream() {
    if (!quic_session_ || !quic_session_->CanOpenNextOutgoingUnidirectionalStream())
        return nullptr;
    auto* qs = quic_session_->OpenOutgoingUnidirectionalStream();
    return qs ? RegisterStream(qs) : nullptr;
}

bool WTSession::SendDatagram(const uint8_t* data, size_t len) {
    if (!quic_session_) return false;
    auto st = quic_session_->SendOrQueueDatagram(
        absl::string_view(reinterpret_cast<const char*>(data), len));
    return st.code == webtransport::DatagramStatusCode::kSuccess;
}

void WTSession::Close(uint32_t ec, const std::string& reason) {
    if (quic_session_) quic_session_->CloseSession(ec, reason);
}

WTStream* WTSession::RegisterStream(quic::WebTransportStream* qs) {
    auto stream = std::make_unique<WTStream>(qs, this);
    WTStream* raw = stream.get();
    streams_.push_back(std::move(stream));
    qs->SetVisitor(std::make_unique<WTStreamBridge>(raw));
    return raw;
}

void WTSession::OnSessionReadyInternal(const std::optional<std::string>& proto) {
    ready_ = true;
    if (visitor_) visitor_->OnSessionReady(proto.value_or(""));
}

void WTSession::OnSessionClosedInternal(quic::WebTransportSessionError code,
                                         const std::string& msg) {
    ready_        = false;
    quic_session_ = nullptr;
    if (close_cb_) close_cb_(this);
    if (visitor_)  visitor_->OnSessionClosed(static_cast<uint32_t>(code), msg);
}

void WTSession::OnIncomingBidirectionalStreamInternal() {
    if (!quic_session_) return;
    while (auto* qs = quic_session_->AcceptIncomingBidirectionalStream()) {
        WTStream* s = RegisterStream(qs);
        if (visitor_) visitor_->OnIncomingBidirectionalStream(s);
        qs->visitor()->OnCanRead();
    }
}

void WTSession::OnIncomingUnidirectionalStreamInternal() {
    if (!quic_session_) return;
    while (auto* qs = quic_session_->AcceptIncomingUnidirectionalStream()) {
        WTStream* s = RegisterStream(qs);
        if (visitor_) visitor_->OnIncomingUnidirectionalStream(s);
        qs->visitor()->OnCanRead();
    }
}

void WTSession::OnDatagramReceivedInternal(absl::string_view dg) {
    if (visitor_)
        visitor_->OnDatagramReceived(
            reinterpret_cast<const uint8_t*>(dg.data()), dg.size());
}

void WTSession::OnCanCreateNewBidiStreamInternal() {
    if (visitor_) visitor_->OnCanCreateNewOutgoingBidirectionalStream();
}

void WTSession::OnCanCreateNewUnidiStreamInternal() {
    if (visitor_) visitor_->OnCanCreateNewOutgoingUnidirectionalStream();
}

void WTSession::OnQuicSessionGone() { quic_session_ = nullptr; }

} // namespace wt