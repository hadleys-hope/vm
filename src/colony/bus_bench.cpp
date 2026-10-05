// bus-bench: plays the world on the bus to load-test the broker and hope-runtime.
//
//   bus-bench --mqtt host:port --houses 5000 --tick-hz 20 --period 10 --seconds 10 [--monitor]
//
// Every tick it publishes hh/house/{id}/sensors for the houses whose phase is due (id % period == tick % period,
// as the world does), and measures how many actuator replies come back and how late. --monitor adds a second
// connection subscribed to hh/# that only counts, like a monitoring service.

#include "mqtt/MqttClient.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    std::string host = "localhost";
    int port = 1883, houses = 5000, period = 10, seconds = 10;
    double hz = 20;
    bool monitor = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--mqtt") { std::string v = argv[++i]; auto c = v.rfind(':'); host = v.substr(0, c); port = std::stoi(v.substr(c + 1)); }
        else if (a == "--houses") houses = std::stoi(argv[++i]);
        else if (a == "--tick-hz") hz = std::stod(argv[++i]);
        else if (a == "--period") period = std::stoi(argv[++i]);
        else if (a == "--seconds") seconds = std::stoi(argv[++i]);
        else if (a == "--monitor") monitor = true;
    }
    std::vector<Clock::time_point> sentAt(houses);
    std::mutex m;
    std::vector<double> lateMs;
    std::atomic<long> replies{0}, monitored{0};
    hope::mqtt::Client world(host, port, "bus-bench-world", [&](std::string_view topic, std::string_view) {
        if (!topic.ends_with("/actuators")) return;
        int id = 0;
        for (char c : topic.substr(9)) { if (c < '0' || c > '9') break; id = id * 10 + (c - '0'); }
        if (id < 0 || id >= houses) return;
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - sentAt[id]).count();
        ++replies;
        std::lock_guard lock(m);
        lateMs.push_back(ms);
    });
    world.connect();
    world.subscribe("hh/house/+/actuators");
    hope::mqtt::Client mon(host, port, "bus-bench-monitor", [&](std::string_view, std::string_view) { ++monitored; });
    if (monitor) { mon.connect(); mon.subscribe("hh/#"); }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    char topic[64], payload[400];
    const auto start = Clock::now();
    const auto tickDur = std::chrono::duration<double>(1.0 / hz);
    long published = 0, tick = 0, overrun = 0;
    while (Clock::now() - start < std::chrono::seconds(seconds)) {
        const auto due = start + std::chrono::duration_cast<Clock::duration>(tickDur * tick);
        std::this_thread::sleep_until(due);
        for (int id = static_cast<int>(tick % period); id < houses; id += period) {
            std::snprintf(topic, sizeof topic, "hh/house/%d/sensors", id);
            std::snprintf(payload, sizeof payload,
                          "{\"t\":%ld,\"id\":%d,\"t_in\":%.1f,\"power_ok\":true,\"on_ups\":false,\"limit_w\":0,"
                          "\"water_ok\":true,\"pipes_ok\":true,\"burst\":false,\"net_online\":true,\"draw_w\":2400,"
                          "\"heater_on\":true,\"residents\":2,\"pressure_kpa\":310.5}",
                          tick, id, 18.0 + (id + tick) % 50 / 10.0);
            sentAt[id] = Clock::now();
            world.publish(topic, payload);
            ++published;
        }
        if (Clock::now() > due + std::chrono::duration_cast<Clock::duration>(tickDur)) ++overrun;
        ++tick;
    }
    const double secs = std::chrono::duration<double>(Clock::now() - start).count();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::sort(lateMs.begin(), lateMs.end());
    auto pct = [&](double p) { return lateMs.empty() ? -1.0 : lateMs[std::min(lateMs.size() - 1, size_t(p * lateMs.size()))]; };
    std::printf("{\"houses\":%d,\"tick_hz\":%.0f,\"period\":%d,\"published_per_s\":%.0f,\"replies_per_s\":%.0f,"
                "\"reply_ratio\":%.3f,\"reply_ms_p50\":%.1f,\"reply_ms_p99\":%.1f,\"monitored_per_s\":%.0f,\"ticks_overrun\":%ld}\n",
                houses, hz, period, published / secs, replies / secs, double(replies) / std::max(1L, published),
                pct(0.5), pct(0.99), monitored / secs, overrun);
    world.close();
    mon.close();
}
