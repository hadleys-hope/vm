#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace hope::mqtt {

// Packet encoding, separate from the socket so it can be tested without a broker.
std::vector<std::uint8_t> encodeConnect(std::string_view clientId, std::uint16_t keepAliveSeconds);
std::vector<std::uint8_t> encodeSubscribe(std::uint16_t packetId, std::string_view filter);
std::vector<std::uint8_t> encodePublish(std::string_view topic, std::string_view payload, bool retain = false);
std::vector<std::uint8_t> encodePing();
void appendRemainingLength(std::vector<std::uint8_t>& out, std::size_t length);

// Minimal MQTT 3.1.1 client: QoS 0 only, clean session, one TCP connection.
// One reader thread delivers PUBLISH packets to the handler; publish() may be called from any thread.
// When the connection drops, connected() turns false and the owner reconnects (connect() again).
class Client {
public:
    using Handler = std::function<void(std::string_view topic, std::string_view payload)>;

    Client(std::string host, int port, std::string clientId, Handler onMessage);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void connect(int timeoutMs = 3000);  // throws on failure
    void subscribe(std::string_view filter);
    bool publish(std::string_view topic, std::string_view payload, bool retain = false);  // false if not connected
    void close();
    bool connected() const { return connected_; }

    std::uint64_t sent() const { return sent_; }
    std::uint64_t received() const { return received_; }

private:
    std::string host_;
    int port_;
    std::string clientId_;
    Handler onMessage_;
    int fd_ = -1;
    std::atomic<bool> connected_{false};
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> received_{0};
    std::uint16_t nextPacketId_ = 1;
    std::mutex writeMutex_;
    std::thread reader_;

    bool writeAll(const std::vector<std::uint8_t>& bytes);
    bool readExact(std::uint8_t* out, std::size_t n);
    void readLoop();
};

} // namespace hope::mqtt
