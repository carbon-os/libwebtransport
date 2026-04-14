// new libwebtransport (pure C++)
#pragma once

#include <memory>
#include <string>
#include <set>
#include <map>

// RESTORED: We need this for the base event types (QuicSocketEventMask, etc.)
#include "quiche/quic/core/io/quic_event_loop.h" 

#include "quiche/quic/core/quic_alarm.h"
#include "quiche/quic/core/quic_alarm_factory.h"
#include "quiche/quic/core/quic_default_connection_helper.h"
#include "quiche/quic/core/quic_udp_socket.h"
#include "quiche/quic/core/quic_packet_writer.h"
#include "quiche/quic/platform/api/quic_socket_address.h"
#include "quiche/quic/core/quic_types.h"

namespace wt {

class NativeAlarm;

class WTEngine {
public:
    WTEngine();
    ~WTEngine();

    quic::QuicAlarmFactory* alarm_factory() { return alarm_factory_.get(); }
    quic::QuicConnectionHelperInterface* helper()        { return helper_.get(); }

    bool RegisterSocket(quic::QuicUdpSocketFd fd,
                        quic::QuicSocketEventMask events,
                        quic::QuicSocketEventListener* listener);
    bool UnregisterSocket(quic::QuicUdpSocketFd fd);
    bool RearmSocket(quic::QuicUdpSocketFd fd,
                     quic::QuicSocketEventMask events);

    void RunOnce(int timeout_ms);

    // Creates an alarm factory synced to this engine's loop (used by Server Dispatcher)
    std::unique_ptr<quic::QuicAlarmFactory> CreateAlarmFactory();

    // Internal routing for the custom event loop
    void RegisterAlarm(NativeAlarm* alarm);
    void UnregisterAlarm(NativeAlarm* alarm);

private:
    std::unique_ptr<quic::QuicConnectionHelperInterface> helper_;
    std::unique_ptr<quic::QuicAlarmFactory>              alarm_factory_;

    struct SocketData {
        quic::QuicSocketEventMask events;
        quic::QuicSocketEventListener* listener;
    };
    std::map<quic::QuicUdpSocketFd, SocketData> sockets_;
    std::set<NativeAlarm*>                      alarms_;
};

class NativeSocketWriter : public quic::QuicPacketWriter {
public:
    explicit NativeSocketWriter(int fd);
    ~NativeSocketWriter() override = default;

    bool SupportsEcn() const override { return false; }
    bool IsBatchMode() const override { return false; }
    bool IsWriteBlocked() const override { return write_blocked_; }
    bool SupportsReleaseTime() const override { return false; }
    void SetWritable() override { write_blocked_ = false; }
    absl::optional<int> MessageTooBigErrorCode() const override;
    quic::QuicByteCount GetMaxPacketSize(const quic::QuicSocketAddress& peer_address) const override;
    quic::QuicPacketBuffer GetNextWriteLocation(const quic::QuicIpAddress&, const quic::QuicSocketAddress&) override;
    quic::WriteResult Flush() override;
    quic::WriteResult WritePacket(const char* buffer, size_t buf_len,
                                  const quic::QuicIpAddress& self_address,
                                  const quic::QuicSocketAddress& peer_address,
                                  quic::PerPacketOptions* options,
                                  const quic::QuicPacketWriterParams& params) override;
private:
    int fd_;
    bool write_blocked_;
};

quic::QuicUdpSocketFd CreateServerSocket(const quic::QuicSocketAddress& addr);
quic::QuicUdpSocketFd CreateClientSocket(int family, quic::QuicSocketAddress* local_addr_out = nullptr);
void DestroySocket(quic::QuicUdpSocketFd fd);
quic::QuicSocketAddress GetLocalAddress(quic::QuicUdpSocketFd fd);

} // namespace wt