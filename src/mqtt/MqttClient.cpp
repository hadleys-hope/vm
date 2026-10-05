#include "mqtt/MqttClient.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <stdexcept>

namespace hope::mqtt {
namespace {

constexpr std::uint8_t CONNECT = 0x10, CONNACK = 0x20, PUBLISH = 0x30, SUBSCRIBE = 0x82, SUBACK = 0x90,
                       PINGREQ = 0xC0, PINGRESP = 0xD0, DISCONNECT = 0xE0;
constexpr std::uint16_t kKeepAlive = 30;

void appendString(std::vector<std::uint8_t>& out, std::string_view s) {
    out.push_back(static_cast<std::uint8_t>(s.size() >> 8));
    out.push_back(static_cast<std::uint8_t>(s.size() & 0xFF));
    out.insert(out.end(), s.begin(), s.end());
}

std::vector<std::uint8_t> packet(std::uint8_t header, const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> out{header};
    appendRemainingLength(out, body.size());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

} // namespace

void appendRemainingLength(std::vector<std::uint8_t>& out, std::size_t length) {
    do {
        std::uint8_t byte = length % 128;
        length /= 128;
        if (length > 0) byte |= 0x80;
        out.push_back(byte);
    } while (length > 0);
}

std::vector<std::uint8_t> encodeConnect(std::string_view clientId, std::uint16_t keepAliveSeconds) {
    std::vector<std::uint8_t> body;
    body.reserve(12 + clientId.size());
    appendString(body, "MQTT");
    body.push_back(4);     // protocol level 3.1.1
    body.push_back(0x02);  // clean session
    body.push_back(static_cast<std::uint8_t>(keepAliveSeconds >> 8));
    body.push_back(static_cast<std::uint8_t>(keepAliveSeconds & 0xFF));
    appendString(body, clientId);
    return packet(CONNECT, body);
}

std::vector<std::uint8_t> encodeSubscribe(std::uint16_t packetId, std::string_view filter) {
    std::vector<std::uint8_t> body{static_cast<std::uint8_t>(packetId >> 8), static_cast<std::uint8_t>(packetId & 0xFF)};
    appendString(body, filter);
    body.push_back(0);  // QoS 0
    return packet(SUBSCRIBE, body);
}

std::vector<std::uint8_t> encodePublish(std::string_view topic, std::string_view payload) {
    std::vector<std::uint8_t> out{PUBLISH};
    appendRemainingLength(out, 2 + topic.size() + payload.size());
    appendString(out, topic);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> encodePing() { return {PINGREQ, 0}; }

Client::Client(std::string host, int port, std::string clientId, Handler onMessage)
    : host_(std::move(host)), port_(port), clientId_(std::move(clientId)), onMessage_(std::move(onMessage)) {}

Client::~Client() { close(); }

void Client::connect(int timeoutMs) {
    close();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &res) != 0 || !res) {
        throw std::runtime_error("mqtt: cannot resolve " + host_);
    }
    int fd = -1;
    for (auto* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) throw std::runtime_error("mqtt: cannot connect to " + host_ + ":" + std::to_string(port_));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    fd_ = fd;
    if (!writeAll(encodeConnect(clientId_, kKeepAlive))) throw std::runtime_error("mqtt: connect write failed");
    pollfd p{fd_, POLLIN, 0};
    if (::poll(&p, 1, timeoutMs) <= 0) throw std::runtime_error("mqtt: no CONNACK");
    std::uint8_t ack[4];
    if (!readExact(ack, 4) || ack[0] != CONNACK || ack[3] != 0) throw std::runtime_error("mqtt: connection refused");
    connected_ = true;
    reader_ = std::thread([this] { readLoop(); });
}

void Client::subscribe(std::string_view filter) {
    std::lock_guard lock(writeMutex_);
    const auto bytes = encodeSubscribe(nextPacketId_++, filter);
    if (nextPacketId_ == 0) nextPacketId_ = 1;
    if (::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(bytes.size())) {
        connected_ = false;
    }
}

bool Client::publish(std::string_view topic, std::string_view payload) {
    if (!connected_) return false;
    if (!writeAll(encodePublish(topic, payload))) return false;
    ++sent_;
    return true;
}

bool Client::writeAll(const std::vector<std::uint8_t>& bytes) {
    std::lock_guard lock(writeMutex_);
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto n = ::send(fd_, bytes.data() + done, bytes.size() - done, MSG_NOSIGNAL);
        if (n <= 0) {
            connected_ = false;
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool Client::readExact(std::uint8_t* out, std::size_t n) {
    std::size_t done = 0;
    while (done < n) {
        const auto r = ::recv(fd_, out + done, n - done, 0);
        if (r <= 0) return false;
        done += static_cast<std::size_t>(r);
    }
    return true;
}

void Client::readLoop() {
    using clock = std::chrono::steady_clock;
    auto lastPing = clock::now();
    std::vector<std::uint8_t> body;
    while (connected_) {
        pollfd p{fd_, POLLIN, 0};
        const int ready = ::poll(&p, 1, 1000);
        if (clock::now() - lastPing > std::chrono::seconds(kKeepAlive / 2)) {
            if (!writeAll(encodePing())) break;
            lastPing = clock::now();
        }
        if (ready <= 0) continue;
        std::uint8_t header;
        if (!readExact(&header, 1)) break;
        std::size_t length = 0, multiplier = 1;
        std::uint8_t byte;
        do {
            if (!readExact(&byte, 1)) { connected_ = false; return; }
            length += (byte & 0x7F) * multiplier;
            multiplier *= 128;
        } while ((byte & 0x80) && multiplier <= 128 * 128 * 128);
        body.resize(length);
        if (length && !readExact(body.data(), length)) break;
        if ((header & 0xF0) == PUBLISH && length >= 2) {
            const std::size_t topicLen = (body[0] << 8) | body[1];
            std::size_t offset = 2 + topicLen;
            if (header & 0x06) offset += 2;  // QoS > 0 carries a packet id
            if (offset > length) continue;
            ++received_;
            onMessage_(std::string_view(reinterpret_cast<const char*>(body.data()) + 2, topicLen),
                       std::string_view(reinterpret_cast<const char*>(body.data()) + offset, length - offset));
        }
        // SUBACK and PINGRESP need no action
    }
    connected_ = false;
}

void Client::close() {
    if (fd_ >= 0) {
        if (connected_) {
            std::vector<std::uint8_t> bye{DISCONNECT, 0};
            writeAll(bye);
        }
        connected_ = false;
        ::shutdown(fd_, SHUT_RDWR);
    }
    if (reader_.joinable()) reader_.join();
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

} // namespace hope::mqtt
