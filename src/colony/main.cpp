// hope-runtime: hosts one VM per house and connects them to the world over MQTT.
//
//   hope-runtime --programs comfort.hbc,eco.hbc,... [--houses 300] [--threads N] [--mqtt host:port]
//   hope-runtime --programs ... --houses 5000 --bench 20      (no broker: synthetic sensors, prints throughput)
//
// Bus (same as the Python controllers in world): in hh/house/{id}/sensors, hh/env/weather, hh/env/power;
// out hh/house/{id}/actuators, hh/house/{id}/log, hh/runtime/status.
// Batched: in hh/batch/sensors (one message per tick, columns: {"t":..,"id":[..],"t_in":[..],..}); a house that
// came in a batch is answered in hh/batch/actuators ({"rows":[{"id":..,"heater_on":..},..]}, one per worker pass).
// House i runs programs[i % k]. Houses are sharded over worker threads by id % threads; each VM is only ever
// touched by its own worker, so the VMs need no locks. A shard keeps only the newest reading per house: if a
// worker falls behind, stale readings are dropped instead of queueing up (memory and latency stay bounded).
// A handler that throws or runs out of budget is a fault: the VM is rebuilt on the next reading; after
// kMaxFaults faults the house is quarantined and the world's built-in thermostat takes over.

#include "colony/FlatJson.hpp"
#include "hbc/HbcReader.hpp"
#include "host/HouseIo.hpp"
#include "mqtt/MqttClient.hpp"
#include "vm/Disasm.hpp"
#include "vm/VM.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace hope;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<bool> running{true};
constexpr int kMaxFaults = 3;               // faults before a house is quarantined ...
constexpr std::int64_t kQuarantineTicks = 600;  // ... for this many simulated minutes
const std::unordered_set<std::string> kBoolActuators{"heater_on", "valve_open", "appliances_on"};

struct Options {
    std::string host = "localhost";
    int port = 1883;
    int houses = 300;
    int threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::string> programs;
    std::uint64_t budget = 10000;  // loop iterations + calls per handler run
    int bench = 0;
    std::string clientId = "hope-runtime";
};

// The house whose next run is recorded and published on hh/runtime/trace: asked for on
// hh/runtime/trace/request, otherwise a different house every 1.5 s so every program shows up.
std::atomic<int> traceHouse{-1};

struct Program {
    std::string source;   // the .hope next to the .hbc, if there is one
    std::string name;
    hbc::Module module;
};

using Readings = std::vector<std::pair<std::string, double>>;

struct House {
    int id{};
    const Program* program{};
    host::HouseIo io;
    std::unique_ptr<vm::VM> vm;
    int faults{};
    std::int64_t quarantinedUntil{-1};
    Readings pending;  // newest unprocessed readings (guarded by the shard mutex)
    bool dirty{};
    bool batched{};    // the pending readings came in a batch: answer in a batch
};

struct Env {  // hh/env/weather and hh/env/power, merged into every house's readings
    std::mutex m;
    std::vector<std::pair<std::string, double>> values;
    void set(std::string_view payload) {
        std::lock_guard lock(m);
        colony::readFlatJson(payload, [&](const std::string& k, double v) {
            for (auto& kv : values) if (kv.first == k) { kv.second = v; return; }
            values.emplace_back(k, v);
        }, [](const std::string&, const std::string&) {});
    }
    std::vector<std::pair<std::string, double>> copy() { std::lock_guard lock(m); return values; }
};

struct Stats {
    std::atomic<std::uint64_t> handled{0}, faults{0}, restarts{0}, coalesced{0}, quarantined{0}, skipped{0};
    std::mutex m;
    std::vector<double> micros;  // handler times since the last report
    void sample(double us) { std::lock_guard lock(m); if (micros.size() < 200000) micros.push_back(us); }
};

class Shard {
public:
    std::vector<House*> houses;
    std::unordered_map<int, House*> byId;
    std::mutex m;
    std::condition_variable cv;
    std::vector<House*> dirty;

    void offer(int id, Readings readings, bool batched, Stats& stats) {
        const auto it = byId.find(id);
        if (it == byId.end()) return;
        {
            std::lock_guard lock(m);
            House* h = it->second;
            h->pending = std::move(readings);
            h->batched = batched;
            if (h->dirty) ++stats.coalesced; else { h->dirty = true; dirty.push_back(h); }
        }
        cv.notify_one();
    }
};

void parseOptions(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) throw std::runtime_error("missing value for " + a); return argv[++i]; };
        if (a == "--mqtt") { auto v = next(); auto c = v.rfind(':'); o.host = v.substr(0, c); o.port = c == std::string::npos ? 1883 : std::stoi(v.substr(c + 1)); }
        else if (a == "--houses") o.houses = std::stoi(next());
        else if (a == "--threads") o.threads = std::max(1, std::stoi(next()));
        else if (a == "--budget") o.budget = std::stoull(next());
        else if (a == "--bench") o.bench = std::stoi(next());
        else if (a == "--client-id") o.clientId = next();
        else if (a == "--programs") { std::stringstream ss(next()); std::string p; while (std::getline(ss, p, ',')) if (!p.empty()) o.programs.push_back(p); }
        else throw std::runtime_error("unknown option " + a);
    }
    if (o.programs.empty()) throw std::runtime_error("--programs a.hbc,b.hbc,... is required");
}

long rssKb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) if (line.rfind("VmRSS:", 0) == 0) return std::stol(line.substr(6));
    return 0;
}

void buildVm(House& h) {
    h.vm = std::make_unique<vm::VM>(h.program->module, 1000 + h.id, &h.io);
    h.vm->initializeGlobals();
}

// Runs one reading through the house's program; returns the actuator payload ("" if nothing to send).
std::string jsonString(std::string_view s) {
    std::string out;
    colony::appendJsonString(out, s);
    return out;
}

// One recorded handler run: what the house read, every instruction (function index, ip), every call out
// of the program with its arguments and result, and what it decided. Marked with "trace" so the worker
// publishes it on hh/runtime/trace instead of the house's log.
std::string traceJson(const House& h, const vm::Trace& t, std::int64_t tick, double us) {
    std::string out = "{\"trace\":true,\"house\":" + std::to_string(h.id) + ",\"program\":" + jsonString(h.program->name) +
                      ",\"tick\":" + std::to_string(tick) + ",\"us\":" + std::to_string(static_cast<int>(us)) + ",\"steps\":[";
    for (std::size_t i = 0; i < t.steps.size(); ++i)
        out += (i ? ",[" : "[") + std::to_string(t.steps[i].fn) + "," + std::to_string(t.steps[i].ip) + "]";
    out += "],\"calls\":[";
    for (std::size_t i = 0; i < t.calls.size(); ++i) {
        const auto& c = t.calls[i];
        out += (i ? ",{" : "{") + std::string("\"step\":") + std::to_string(c.step) + ",\"name\":" + jsonString(c.name) +
               ",\"args\":" + jsonString(c.args) + ",\"result\":" + jsonString(c.result) + "}";
    }
    out += "],\"sensors\":{";
    bool first = true;
    for (const auto& [k, v] : h.io.sensors) {
        out += (first ? "" : ",") + jsonString(k) + ":";
        colony::appendJsonNumber(out, v);
        first = false;
    }
    out += "},\"actuators\":{";
    first = true;
    for (const auto& [k, v] : h.io.actuators) {
        out += (first ? "" : ",") + jsonString(k) + ":";
        colony::appendJsonNumber(out, v);
        first = false;
    }
    for (const auto& [k, v] : h.io.texts) {
        out += (first ? "" : ",") + jsonString(k) + ":" + jsonString(v);
        first = false;
    }
    out += "},\"topic\":" + jsonString(h.batched ? "hh/batch/actuators" : "hh/house/" + std::to_string(h.id) + "/actuators") + "}";
    return out;
}

// Every program: its source, its functions as readable bytecode, and how many houses run it.
std::string programsJson(const std::vector<std::unique_ptr<Program>>& programs, int houses) {
    std::string out = "[";
    for (std::size_t p = 0; p < programs.size(); ++p) {
        const auto& prog = *programs[p];
        const auto& m = prog.module;
        out += (p ? ",{" : "{") + std::string("\"name\":") + jsonString(prog.name) + ",\"source\":" + jsonString(prog.source) +
               ",\"houses\":" + std::to_string(houses / static_cast<int>(programs.size()) + (static_cast<int>(p) < houses % static_cast<int>(programs.size()) ? 1 : 0)) +
               ",\"functions\":[";
        for (std::size_t f = 0; f < m.functions.size(); ++f) {
            const auto& fn = m.functions[f];
            out += (f ? ",{" : "{") + std::string("\"name\":") + jsonString(hbc::stringConstant(m, fn.nameConstant)) +
                   ",\"params\":" + std::to_string(fn.parameterCount) + ",\"locals\":" + std::to_string(fn.localCount) + ",\"code\":[";
            const auto code = vm::disassemble(m, fn);
            for (std::size_t i = 0; i < code.size(); ++i)
                out += (i ? ",[" : "[") + std::to_string(code[i].ip) + "," + jsonString(code[i].op) + "," + jsonString(code[i].args) + "]";
            out += "]}";
        }
        out += "],\"bytes\":" + std::to_string([&] { std::size_t n = 0; for (const auto& fn : m.functions) n += fn.code.size(); return n; }()) + "}";
    }
    return out + "]";
}

Readings parseReadings(std::string_view payload) {
    Readings out;
    colony::readFlatJson(payload, [&](const std::string& k, double v) { out.emplace_back(k, v); },
                         [](const std::string&, const std::string&) {});
    return out;
}

// Returns the actuators as the inside of a JSON object (no braces, no id): "\"t\":5,\"heater_on\":true,..."
std::string runHouse(House& h, const Readings& readings, const std::vector<std::pair<std::string, double>>& env,
                     const Options& o, Stats& stats, std::vector<std::string>& logs) {
    for (const auto& [k, v] : env) h.io.sensors[k] = v;
    for (const auto& [k, v] : readings) h.io.sensors[k] = v;
    const auto tick = static_cast<std::int64_t>(h.io.sensors["t"]);
    if (h.quarantinedUntil >= 0 && tick < h.quarantinedUntil) { ++stats.skipped; return ""; }
    if (h.quarantinedUntil >= 0) { h.quarantinedUntil = -1; h.faults = 0; --stats.quarantined; }
    const auto t0 = Clock::now();
    try {
        if (!h.vm) { buildVm(h); ++stats.restarts; }
        if (tick * 60000 < h.vm->virtualTimeMs()) buildVm(h);  // the world started over (new colony)
        h.io.actuators.clear();
        h.io.texts.clear();
        h.vm->setStepBudget(o.budget);
        int want = h.id;
        const bool tracing = traceHouse.load() == h.id && traceHouse.compare_exchange_strong(want, -1);
        vm::Trace trace;
        if (tracing) h.vm->setTrace(&trace);
        h.vm->simulateUntil(tick * 60000, 64);  // every/at handlers on the world's clock, 1 tick = 1 minute
        h.vm->post("Sensors");
        h.vm->dispatchEvents();
        if (tracing) {
            h.vm->setTrace(nullptr);
            logs.push_back(traceJson(h, trace, tick, std::chrono::duration<double, std::micro>(Clock::now() - t0).count()));
        }
    } catch (const std::exception& e) {
        ++stats.faults;
        h.vm.reset();  // rebuilt from scratch on the next reading
        if (++h.faults >= kMaxFaults) {
            h.quarantinedUntil = tick + kQuarantineTicks;
            ++stats.quarantined;
        }
        logs.push_back(std::string("{\"fault\":") + [&] { std::string s; colony::appendJsonString(s, e.what()); return s; }() +
                       ",\"faults\":" + std::to_string(h.faults) + ",\"quarantined\":" + (h.quarantinedUntil >= 0 ? "true" : "false") + "}");
        return "";
    }
    stats.sample(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
    ++stats.handled;
    while (!h.io.logs.empty()) {
        std::string line = "{\"log\":";
        colony::appendJsonString(line, h.io.logs.front());
        logs.push_back(line + "}");
        h.io.logs.pop_front();
    }
    if (h.io.actuators.empty() && h.io.texts.empty()) return "";
    std::string out = "\"t\":" + std::to_string(tick);
    for (const auto& [k, v] : h.io.actuators) {
        out += ",";
        colony::appendJsonString(out, k);
        out += ":";
        if (kBoolActuators.contains(k)) out += v > 0.5 ? "true" : "false";
        else colony::appendJsonNumber(out, v);
    }
    for (const auto& [k, v] : h.io.texts) {
        out += ",";
        colony::appendJsonString(out, k);
        out += ":";
        colony::appendJsonString(out, v);
    }
    return out;
}

void worker(Shard& shard, Env& env, const Options& o, Stats& stats, mqtt::Client* client) {
    std::vector<House*> batch;
    std::vector<Readings> readings;
    std::vector<char> batchedFlags;
    std::vector<std::string> logs;
    std::string rows;
    char topic[64];
    while (running) {
        {
            std::unique_lock lock(shard.m);
            shard.cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return !shard.dirty.empty() || !running; });
            batch.swap(shard.dirty);
            readings.clear();
            batchedFlags.clear();
            for (House* h : batch) {
                readings.push_back(std::move(h->pending));
                batchedFlags.push_back(h->batched);
                h->dirty = false;
            }
        }
        if (batch.empty()) continue;
        const auto envNow = env.copy();
        rows.clear();
        for (std::size_t i = 0; i < batch.size(); ++i) {
            House& h = *batch[i];
            logs.clear();
            const auto out = runHouse(h, readings[i], envNow, o, stats, logs);
            if (!client) continue;
            if (!out.empty() && batchedFlags[i]) {
                rows += rows.empty() ? "{\"id\":" : ",{\"id\":";
                rows += std::to_string(h.id) + "," + out + "}";
            } else if (!out.empty()) {
                std::snprintf(topic, sizeof topic, "hh/house/%d/actuators", h.id);
                client->publish(topic, "{" + out + "}");
            }
            std::snprintf(topic, sizeof topic, "hh/house/%d/log", h.id);
            for (const auto& l : logs) client->publish(l.rfind("{\"trace\":true", 0) == 0 ? "hh/runtime/trace" : topic, l);
        }
        if (client && !rows.empty()) client->publish("hh/batch/actuators", "{\"rows\":[" + rows + "]}");
        batch.clear();
    }
}

std::string statusJson(const Options& o, Stats& stats, const mqtt::Client* client, double seconds) {
    std::vector<double> us;
    { std::lock_guard lock(stats.m); us.swap(stats.micros); }
    std::sort(us.begin(), us.end());
    auto pct = [&](double p) { return us.empty() ? 0.0 : us[std::min(us.size() - 1, static_cast<std::size_t>(p * us.size()))]; };
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "{\"houses\":%d,\"threads\":%d,\"handled\":%llu,\"handled_per_s\":%.0f,\"handler_us_p50\":%.1f,"
                  "\"handler_us_p99\":%.1f,\"faults\":%llu,\"restarts\":%llu,\"quarantined\":%llu,\"coalesced\":%llu,"
                  "\"skipped\":%llu,\"mqtt_sent\":%llu,\"mqtt_received\":%llu,\"rss_mb\":%.1f}",
                  o.houses, o.threads, (unsigned long long)stats.handled.load(), us.size() / std::max(seconds, 1e-9),
                  pct(0.5), pct(0.99), (unsigned long long)stats.faults.load(), (unsigned long long)stats.restarts.load(),
                  (unsigned long long)stats.quarantined.load(), (unsigned long long)stats.coalesced.load(),
                  (unsigned long long)stats.skipped.load(),
                  (unsigned long long)(client ? client->sent() : 0), (unsigned long long)(client ? client->received() : 0),
                  rssKb() / 1024.0);
    return buf;
}

std::string syntheticSensors(int id, int round) {
    char buf[512];
    const double tIn = 18.0 + (id * 7 + round) % 60 / 10.0;
    std::snprintf(buf, sizeof buf,
                  "{\"t\":%d,\"id\":%d,\"sector\":%d,\"t_in\":%.1f,\"power_ok\":true,\"on_ups\":%s,\"limit_w\":%d,"
                  "\"water_ok\":true,\"pipes_ok\":true,\"burst\":false,\"net_online\":true,\"sludge\":0.3,\"draw_w\":2400,"
                  "\"heater_on\":true,\"residents\":2,\"pressure_kpa\":310.5,\"water_l_min\":1.2,\"leak_l_min\":0.0}",
                  1000 + round, id, id % 6 + 1, tIn, (id % 17 == 0) ? "true" : "false", (id % 11 == 0) ? 2500 : 0);
    return buf;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    try { parseOptions(argc, argv, o); } catch (const std::exception& e) { std::cerr << "hope-runtime: " << e.what() << '\n'; return 2; }
    std::signal(SIGINT, [](int) { running = false; });
    std::signal(SIGTERM, [](int) { running = false; });

    std::vector<std::unique_ptr<Program>> programs;
    for (const auto& path : o.programs) {
        auto p = std::make_unique<Program>();
        p->name = std::filesystem::path(path).stem().string();
        p->module = hbc::Reader::readFile(path);
        std::ifstream src(std::filesystem::path(path).replace_extension(".hope"));
        if (src) p->source.assign(std::istreambuf_iterator<char>(src), std::istreambuf_iterator<char>());
        programs.push_back(std::move(p));
    }
    const long rss0 = rssKb();
    const auto build0 = Clock::now();
    std::vector<std::unique_ptr<House>> houses;
    std::vector<std::unique_ptr<Shard>> shards;
    for (int t = 0; t < o.threads; ++t) shards.push_back(std::make_unique<Shard>());
    for (int id = 0; id < o.houses; ++id) {
        auto h = std::make_unique<House>();
        h->id = id;
        h->io.houseId = id;
        h->program = programs[id % programs.size()].get();
        buildVm(*h);
        auto& shard = *shards[id % o.threads];
        shard.houses.push_back(h.get());
        shard.byId[id] = h.get();
        houses.push_back(std::move(h));
    }
    const double buildMs = std::chrono::duration<double, std::milli>(Clock::now() - build0).count();
    std::fprintf(stderr, "hope-runtime: %d houses, %zu programs, %d threads; VMs built in %.1f ms, %.1f KB RSS per house\n",
                 o.houses, programs.size(), o.threads, buildMs, (rssKb() - rss0) / double(std::max(1, o.houses)));

    Env env;
    env.set(R"({"t_out":-38.5,"wind":9.0,"storm":false,"hour":13.5,"night":false,"daylight":0.8})");
    env.set(R"({"available_kw":3200,"demand_kw":2900,"shedding":0})");
    Stats stats;

    if (o.bench > 0) {  // no broker: every house gets a reading every round, as fast as the workers go
        std::vector<std::thread> workers;
        for (auto& s : shards) workers.emplace_back(worker, std::ref(*s), std::ref(env), std::cref(o), std::ref(stats), nullptr);
        const auto t0 = Clock::now();
        for (int round = 0; round < o.bench; ++round) {
            for (auto& h : houses) {
                shards[h->id % o.threads]->offer(h->id, parseReadings(syntheticSensors(h->id, round)), false, stats);
            }
            while (running) {  // one round at a time, like the world's ticks
                bool idle = true;
                for (auto& s : shards) { std::lock_guard lock(s->m); if (!s->dirty.empty()) idle = false; }
                const auto done = stats.handled + stats.faults + stats.skipped;
                if (idle && done >= static_cast<std::uint64_t>((round + 1) * o.houses)) break;
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
        const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        running = false;
        for (auto& s : shards) s->cv.notify_all();
        for (auto& w : workers) w.join();
        std::printf("%s\n", statusJson(o, stats, nullptr, secs).c_str());
        std::printf("bench: %d rounds x %d houses in %.3f s: %.0f readings/s, %.1f us per reading of wall time\n",
                    o.bench, o.houses, secs, o.bench * o.houses / secs, secs * 1e6 / (o.bench * o.houses));
        return 0;
    }

    mqtt::Client client(o.host, o.port, o.clientId, [&](std::string_view topic, std::string_view payload) {
        if (topic.starts_with("hh/house/") && topic.ends_with("/sensors")) {
            int id = 0;
            for (char c : topic.substr(9)) { if (c < '0' || c > '9') break; id = id * 10 + (c - '0'); }
            shards[id % o.threads]->offer(id, parseReadings(payload), false, stats);
        } else if (topic == "hh/batch/sensors") {
            std::vector<std::pair<std::string, std::vector<double>>> columns;
            Readings common;
            colony::readColumnarJson(
                payload, [&](const std::string& k, const std::vector<double>& v) { columns.emplace_back(k, v); },
                [&](const std::string& k, double v) { common.emplace_back(k, v); });
            const std::vector<double>* ids = nullptr;
            for (const auto& [k, v] : columns) if (k == "id") ids = &v;
            if (!ids) return;
            for (std::size_t row = 0; row < ids->size(); ++row) {
                Readings r = common;
                for (const auto& [k, v] : columns) if (row < v.size()) r.emplace_back(k, v[row]);
                const int id = static_cast<int>((*ids)[row]);
                if (id >= 0) shards[id % o.threads]->offer(id, std::move(r), true, stats);
            }
        } else if (topic == "hh/runtime/trace/request") {
            colony::readFlatJson(payload, [&](const std::string& k, double v) { if (k == "house") traceHouse = static_cast<int>(v); },
                                 [](const std::string&, const std::string&) {});
        } else if (topic == "hh/env/weather" || topic == "hh/env/power") {
            env.set(payload);
        }
    });
    std::vector<std::thread> workers;
    for (auto& s : shards) workers.emplace_back(worker, std::ref(*s), std::ref(env), std::cref(o), std::ref(stats), &client);
    int backoffMs = 250;
    auto lastStatus = Clock::now();
    auto traceSince = Clock::now();
    int traceNext = 0;
    while (running) {
        if (!client.connected()) {
            try {
                client.connect();
                client.subscribe("hh/house/+/sensors");
                client.subscribe("hh/batch/sensors");
                client.subscribe("hh/env/weather");
                client.subscribe("hh/env/power");
                client.subscribe("hh/runtime/trace/request");
                client.publish("hh/runtime/programs", programsJson(programs, o.houses), true);
                std::fprintf(stderr, "hope-runtime: connected to %s:%d\n", o.host.c_str(), o.port);
                backoffMs = 250;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "hope-runtime: %s, retrying in %d ms\n", e.what(), backoffMs);
                std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                backoffMs = std::min(5000, backoffMs * 2);
                continue;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        // a sampled trace every 1.5 s, stepping through the houses so every program shows; a house that gets
        // no reading within 3 s (not served, quarantined) is skipped
        const double traceAge = std::chrono::duration<double>(Clock::now() - traceSince).count();
        if ((traceHouse.load() == -1 && traceAge > 1.5) || traceAge > 3.0) {   // taken, or stale: next house
            traceNext = (traceNext + 97) % std::max(1, o.houses);
            traceHouse = traceNext;
            traceSince = Clock::now();
        }
        const double since = std::chrono::duration<double>(Clock::now() - lastStatus).count();
        if (since >= 2.0) {
            client.publish("hh/runtime/status", statusJson(o, stats, &client, since));
            lastStatus = Clock::now();
        }
    }
    for (auto& s : shards) s->cv.notify_all();
    for (auto& w : workers) w.join();
    client.close();
    return 0;
}
