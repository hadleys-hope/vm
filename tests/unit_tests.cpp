// Unit tests without a broker or a compiler: MQTT packet encoding and the bus JSON codec.
#include "colony/FlatJson.hpp"
#include "mqtt/MqttClient.hpp"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static void remainingLength() {
    using hope::mqtt::appendRemainingLength;
    const std::vector<std::pair<std::size_t, std::vector<std::uint8_t>>> cases = {
        {0, {0x00}}, {127, {0x7F}}, {128, {0x80, 0x01}}, {16383, {0xFF, 0x7F}}, {16384, {0x80, 0x80, 0x01}},
        {268435455, {0xFF, 0xFF, 0xFF, 0x7F}}};  // MQTT 3.1.1, 2.2.3
    for (const auto& [n, want] : cases) {
        std::vector<std::uint8_t> got;
        appendRemainingLength(got, n);
        CHECK(got == want);
    }
}

static void packets() {
    const auto c = hope::mqtt::encodeConnect("rt", 30);
    const std::vector<std::uint8_t> wantConnect{0x10, 14, 0, 4, 'M', 'Q', 'T', 'T', 4, 0x02, 0, 30, 0, 2, 'r', 't'};
    CHECK(c == wantConnect);
    const auto p = hope::mqtt::encodePublish("a/b", "xy");
    const std::vector<std::uint8_t> wantPublish{0x30, 7, 0, 3, 'a', '/', 'b', 'x', 'y'};
    CHECK(p == wantPublish);
    const auto s = hope::mqtt::encodeSubscribe(1, "hh/#");
    const std::vector<std::uint8_t> wantSub{0x82, 9, 0, 1, 0, 4, 'h', 'h', '/', '#', 0};
    CHECK(s == wantSub);
    const std::string big(200, 'z');
    const auto b = hope::mqtt::encodePublish("t", big);
    CHECK(b.size() == 1 + 2 + 2 + 1 + 200 && b[1] == ((203 % 128) | 0x80) && b[2] == 1);
}

static void flatJson() {
    std::map<std::string, double> nums;
    std::map<std::string, std::string> texts;
    hope::colony::readFlatJson(
        R"({"t": 42, "t_in": -3.5e1, "on_ups": true, "burst": false, "x": null, "nested": {"a": [1, {"b": 2}]},)"
        R"( "mode": "ONLINE \"x\"", "last": 7})",
        [&](const std::string& k, double v) { nums[k] = v; }, [&](const std::string& k, const std::string& v) { texts[k] = v; });
    CHECK(nums["t"] == 42 && nums["t_in"] == -35.0 && nums["on_ups"] == 1.0 && nums["burst"] == 0.0);
    CHECK(!nums.contains("x") && !nums.contains("nested") && !nums.contains("a"));
    CHECK(nums["last"] == 7 && texts["mode"] == "ONLINE \"x\"");
    std::map<std::string, double> partial;  // truncated input keeps what was read
    hope::colony::readFlatJson(R"({"a": 1, "b": )", [&](const std::string& k, double v) { partial[k] = v; },
                               [](const std::string&, const std::string&) {});
    CHECK(partial.size() == 1 && partial["a"] == 1);
    std::string out;
    hope::colony::appendJsonString(out, "say \"hi\"\n");
    CHECK(out == R"("say \"hi\"\u000a")");
}

int main() {
    remainingLength();
    packets();
    flatJson();
    std::printf(failures ? "%d FAILED\n" : "all unit tests passed\n", failures);
    return failures ? 1 : 0;
}
