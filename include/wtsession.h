// new libwebtransport
#pragma once

#include "wt_base.h"

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "quiche/quic/core/web_transport_interface.h"
#include "quiche/quic/platform/api/quic_logging.h"

namespace wt {

class WTSession;

class WTStream : public Stream {
public:
    WTStream(quic::WebTransportStream* qs, WTSession* owner);
    ~WTStream() override;

    bool   Write(const uint8_t* data, size_t len) override;
    bool   FinWrite() override;
    void   ResetStream(uint32_t error_code) override;
    void   StopSending(uint32_t error_code) override;
    size_t Read(uint8_t* dst, size_t len) override;
    size_t ReadableBytes() const override;
    uint64_t id() const override;
    void   SetPriority(const StreamPriority& p) override;
    void   SetVisitor(std::unique_ptr<StreamVisitor> v) override;

    void OnCanReadInternal();
    void OnCanWriteInternal();
    void OnResetStreamReceivedInternal(quic::WebTransportStreamError err);
    void OnStopSendingReceivedInternal(quic::WebTransportStreamError err);
    void OnWriteSideClosedInternal();
    void OnQuicStreamGone();

    bool IsGone() const { return quic_stream_ == nullptr; }

private:
    void TryFlushWriteQueue();

    quic::WebTransportStream*      quic_stream_;
    std::unique_ptr<StreamVisitor> visitor_;

    struct WriteChunk { std::vector<uint8_t> data; };
    std::deque<WriteChunk> write_queue_;
    std::vector<uint8_t>   read_buf_;

    bool send_fin_   = false;
    bool fin_sent_   = false;
    bool stop_rcvd_  = false;
    bool reset_rcvd_ = false;
};

class WTStreamBridge : public quic::WebTransportStreamVisitor {
public:
    explicit WTStreamBridge(WTStream* s) : stream_(s) {}
    ~WTStreamBridge() override;
    void OnCanRead() override  { if (stream_) stream_->OnCanReadInternal(); }
    void OnCanWrite() override { if (stream_) stream_->OnCanWriteInternal(); }
    void OnResetStreamReceived(quic::WebTransportStreamError e) override {
        if (stream_) stream_->OnResetStreamReceivedInternal(e);
    }
    void OnStopSendingReceived(quic::WebTransportStreamError e) override {
        if (stream_) stream_->OnStopSendingReceivedInternal(e);
    }
    void OnWriteSideInDataRecvdState() override {}
private:
    WTStream* stream_;
};

class WTSession : public Session {
public:
    using CloseCallback = std::function<void(WTSession*)>;

    WTSession(quic::WebTransportSession* qs, const std::string& peer_address);
    ~WTSession() override;

    Stream* OpenBidirectionalStream()  override;
    Stream* OpenUnidirectionalStream() override;
    bool    SendDatagram(const uint8_t* data, size_t len) override;
    void    Close(uint32_t error_code, const std::string& reason) override;
    bool        IsReady()     const override { return ready_; }
    std::string PeerAddress() const override { return peer_address_; }
    void SetVisitor(std::unique_ptr<SessionVisitor> v) override { visitor_ = std::move(v); }

    void SetCloseCallback(CloseCallback cb) { close_cb_ = std::move(cb); }

    void OnSessionReadyInternal(const std::optional<std::string>& proto);
    void OnSessionClosedInternal(quic::WebTransportSessionError code,
                                 const std::string& msg);
    void OnIncomingBidirectionalStreamInternal();
    void OnIncomingUnidirectionalStreamInternal();
    void OnDatagramReceivedInternal(absl::string_view dg);
    void OnCanCreateNewBidiStreamInternal();
    void OnCanCreateNewUnidiStreamInternal();
    void OnQuicSessionGone();

    WTStream* RegisterStream(quic::WebTransportStream* qs);
    quic::WebTransportSession* GetQuicSession() { return quic_session_; }

private:
    quic::WebTransportSession*             quic_session_;
    std::string                            peer_address_;
    std::unique_ptr<SessionVisitor>        visitor_;
    CloseCallback                          close_cb_;
    bool                                   ready_ = false;
    std::vector<std::unique_ptr<WTStream>> streams_;
};

class WTSessionBridge : public quic::WebTransportVisitor {
public:
    explicit WTSessionBridge(std::shared_ptr<WTSession> s)
        : session_(std::move(s)) {}
    ~WTSessionBridge() override;

    void OnSessionReady() override;
    void OnSessionClosed(quic::WebTransportSessionError code,
                         const std::string& msg) override;
    void OnIncomingBidirectionalStreamAvailable() override;
    void OnIncomingUnidirectionalStreamAvailable() override;
    void OnDatagramReceived(absl::string_view dg) override;
    void OnCanCreateNewOutgoingBidirectionalStream() override;
    void OnCanCreateNewOutgoingUnidirectionalStream() override;

private:
    std::shared_ptr<WTSession> session_;
};

} // namespace wt