// new libwebtransport (pure C++)
#include "wtengine.h"

#include <sys/socket.h>
#include <poll.h>
#include <cerrno>
#include <vector>
#include <algorithm>

#include "quiche/quic/core/quic_default_clock.h"

namespace wt {

// ─── NativeAlarm ─────────────────────────────────────────────────────────────

class NativeAlarm : public quic::QuicAlarm {
public:
    NativeAlarm(quic::QuicArenaScopedPtr<quic::QuicAlarm::Delegate> delegate, WTEngine* engine)
        : quic::QuicAlarm(std::move(delegate)), engine_(engine) {}

    ~NativeAlarm() override {
        engine_->UnregisterAlarm(this);
    }

    void FireImpl() { Fire(); }

protected:
    void SetImpl() override { engine_->RegisterAlarm(this); }
    void CancelImpl() override { engine_->UnregisterAlarm(this); }

private:
    WTEngine* engine_;
};

// ─── NativeAlarmFactory ──────────────────────────────────────────────────────

class NativeAlarmFactory : public quic::QuicAlarmFactory {
public:
    NativeAlarmFactory(WTEngine* engine) : engine_(engine) {}

    quic::QuicAlarm* CreateAlarm(quic::QuicAlarm::Delegate* delegate) override {
        return new NativeAlarm(quic::QuicArenaScopedPtr<quic::QuicAlarm::Delegate>(delegate), engine_);
    }

    quic::QuicArenaScopedPtr<quic::QuicAlarm> CreateAlarm(
        quic::QuicArenaScopedPtr<quic::QuicAlarm::Delegate> delegate,
        quic::QuicConnectionArena* arena) override {
        if (arena != nullptr) {
            return arena->New<NativeAlarm>(std::move(delegate), engine_);
        }
        return quic::QuicArenaScopedPtr<quic::QuicAlarm>(
            new NativeAlarm(std::move(delegate), engine_));
    }

private:
    WTEngine* engine_;
};

// ─── WTEngine ────────────────────────────────────────────────────────────────

WTEngine::WTEngine() {
    helper_        = std::make_unique<quic::QuicDefaultConnectionHelper>();
    alarm_factory_ = CreateAlarmFactory();
}

WTEngine::~WTEngine() = default;

std::unique_ptr<quic::QuicAlarmFactory> WTEngine::CreateAlarmFactory() {
    return std::make_unique<NativeAlarmFactory>(this);
}

void WTEngine::RegisterAlarm(NativeAlarm* alarm) {
    alarms_.insert(alarm);
}

void WTEngine::UnregisterAlarm(NativeAlarm* alarm) {
    alarms_.erase(alarm);
}

bool WTEngine::RegisterSocket(quic::QuicUdpSocketFd fd,
                               quic::QuicSocketEventMask events,
                               quic::QuicSocketEventListener* listener) {
    sockets_[fd] = {events, listener};
    return true;
}

bool WTEngine::UnregisterSocket(quic::QuicUdpSocketFd fd) {
    return sockets_.erase(fd) > 0;
}

bool WTEngine::RearmSocket(quic::QuicUdpSocketFd fd,
                            quic::QuicSocketEventMask events) {
    auto it = sockets_.find(fd);
    if (it != sockets_.end()) {
        it->second.events = events;
        return true;
    }
    return false;
}

void WTEngine::RunOnce(int timeout_ms) {
    quic::QuicTime now = helper_->GetClock()->Now();
    quic::QuicTime::Delta min_delta = quic::QuicTime::Delta::Infinite();

    for (auto* alarm : alarms_) {
        if (alarm->IsSet()) {
            if (alarm->deadline() <= now) {
                min_delta = quic::QuicTime::Delta::Zero();
                break;
            }
            quic::QuicTime::Delta d = alarm->deadline() - now;
            if (min_delta.IsInfinite() || d < min_delta) {
                min_delta = d;
            }
        }
    }

    int poll_timeout = timeout_ms;
    if (!min_delta.IsInfinite()) {
        int alarm_ms = min_delta.ToMilliseconds();
        if (alarm_ms < poll_timeout) {
            poll_timeout = std::max(0, alarm_ms);
        }
    }

    std::vector<pollfd> pfds;
    pfds.reserve(sockets_.size());
    for (const auto& [fd, data] : sockets_) {
        pollfd pfd{};
        pfd.fd = fd;
        if (data.events & quic::kSocketEventReadable) pfd.events |= POLLIN;
        if (data.events & quic::kSocketEventWritable) pfd.events |= POLLOUT;
        pfds.push_back(pfd);
    }

    int res = poll(pfds.data(), pfds.size(), poll_timeout);

    if (res > 0) {
        for (const auto& pfd : pfds) {
            if (pfd.revents == 0) continue;
            quic::QuicSocketEventMask revents = 0;
            if (pfd.revents & POLLIN) revents |= quic::kSocketEventReadable;
            if (pfd.revents & POLLOUT) revents |= quic::kSocketEventWritable;
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                revents |= quic::kSocketEventReadable;
            }
            if (revents) {
                auto it = sockets_.find(pfd.fd);
                if (it != sockets_.end()) {
                    it->second.listener->OnSocketEvent(nullptr, pfd.fd, revents);
                }
            }
        }
    }

    now = helper_->GetClock()->Now();
    std::vector<NativeAlarm*> to_fire;
    for (auto* alarm : alarms_) {
        if (alarm->IsSet() && alarm->deadline() <= now) {
            to_fire.push_back(alarm);
        }
    }
    for (auto* alarm : to_fire) {
        if (alarm->IsSet() && alarm->deadline() <= now) {
            alarms_.erase(alarm);
            alarm->FireImpl();
        }
    }
}

// ─── NativeSocketWriter ──────────────────────────────────────────────────────

NativeSocketWriter::NativeSocketWriter(int fd) : fd_(fd), write_blocked_(false) {}

absl::optional<int> NativeSocketWriter::MessageTooBigErrorCode() const {
    return quic::kSocketErrorMsgSize;
}

quic::QuicByteCount NativeSocketWriter::GetMaxPacketSize(const quic::QuicSocketAddress&) const {
    return quic::kMaxOutgoingPacketSize;
}

quic::QuicPacketBuffer NativeSocketWriter::GetNextWriteLocation(const quic::QuicIpAddress&, const quic::QuicSocketAddress&) {
    return {nullptr, nullptr};
}

quic::WriteResult NativeSocketWriter::Flush() {
    return quic::WriteResult(quic::WRITE_STATUS_OK, 0);
}

quic::WriteResult NativeSocketWriter::WritePacket(const char* buffer, size_t buf_len,
                                                  const quic::QuicIpAddress& /*self_address*/,
                                                  const quic::QuicSocketAddress& peer_address,
                                                  quic::PerPacketOptions* /*options*/,
                                                  const quic::QuicPacketWriterParams& /*params*/) {
    sockaddr_storage peer_addr_storage = peer_address.generic_address();
    socklen_t peer_addr_len = (peer_addr_storage.ss_family == AF_INET) 
                                ? sizeof(sockaddr_in) 
                                : sizeof(sockaddr_in6);

    ssize_t bytes_written = sendto(fd_, buffer, buf_len, 0,
                                   reinterpret_cast<sockaddr*>(&peer_addr_storage),
                                   peer_addr_len);
    if (bytes_written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            write_blocked_ = true;
            return quic::WriteResult(quic::WRITE_STATUS_BLOCKED, errno);
        }
        return quic::WriteResult(quic::WRITE_STATUS_ERROR, errno);
    }
    return quic::WriteResult(quic::WRITE_STATUS_OK, bytes_written);
}

// ─── Socket Creation ─────────────────────────────────────────────────────────

quic::QuicUdpSocketFd CreateServerSocket(const quic::QuicSocketAddress& addr) {
    quic::QuicUdpSocketApi api;
    int af = addr.host().IsIPv4() ? AF_INET : AF_INET6;
    quic::QuicUdpSocketFd fd = api.Create(af,
        /*receive_buffer_size=*/2 * 1024 * 1024,
        /*send_buffer_size=*/2 * 1024 * 1024);
    if (fd == quic::kQuicInvalidSocketFd) return fd;
    if (!api.Bind(fd, addr)) {
        api.Destroy(fd);
        return quic::kQuicInvalidSocketFd;
    }
    return fd;
}

quic::QuicUdpSocketFd CreateClientSocket(int family, quic::QuicSocketAddress* local_addr_out) {
    quic::QuicUdpSocketApi api;
    quic::QuicUdpSocketFd fd = api.Create(family, 
        2 * 1024 * 1024, 2 * 1024 * 1024);
        
    if (fd == quic::kQuicInvalidSocketFd) return fd;

    quic::QuicSocketAddress any_addr(
        family == AF_INET ? quic::QuicIpAddress::Any4() : quic::QuicIpAddress::Any6(), 0);
        
    if (!api.Bind(fd, any_addr)) { 
        api.Destroy(fd); 
        return quic::kQuicInvalidSocketFd; 
    }
    
    if (local_addr_out) *local_addr_out = GetLocalAddress(fd);
    return fd;
}

void DestroySocket(quic::QuicUdpSocketFd fd) {
    if (fd != quic::kQuicInvalidSocketFd) quic::QuicUdpSocketApi().Destroy(fd);
}

quic::QuicSocketAddress GetLocalAddress(quic::QuicUdpSocketFd fd) {
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        return quic::QuicSocketAddress();
    return quic::QuicSocketAddress(addr);
}

} // namespace wt