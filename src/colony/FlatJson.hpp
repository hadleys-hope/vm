#pragma once

#include <cctype>
#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace hope::colony {

// Reads a flat JSON object: numbers and booleans go to onNumber (true = 1.0), strings to onText.
// Nested objects and arrays are skipped. Malformed input stops the scan; what was read so far is kept.
template <class OnNumber, class OnText>
void readFlatJson(std::string_view s, OnNumber onNumber, OnText onText) {
    std::size_t i = 0;
    auto ws = [&] { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; };
    auto str = [&](std::string& out) -> bool {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) { out.push_back(s[i + 1]); i += 2; continue; }
            out.push_back(s[i++]);
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    };
    auto skipNested = [&] {
        int depth = 0;
        bool inStr = false;
        for (; i < s.size(); ++i) {
            const char c = s[i];
            if (inStr) { if (c == '\\') ++i; else if (c == '"') inStr = false; continue; }
            if (c == '"') inStr = true;
            else if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') { if (--depth == 0) { ++i; return; } }
        }
    };
    ws();
    if (i >= s.size() || s[i] != '{') return;
    ++i;
    std::string key, text;
    while (true) {
        ws();
        if (i < s.size() && s[i] == '}') return;
        if (!str(key)) return;
        ws();
        if (i >= s.size() || s[i] != ':') return;
        ++i;
        ws();
        if (i >= s.size()) return;
        const char c = s[i];
        if (c == '"') { if (!str(text)) return; onText(key, text); }
        else if (c == 't' && s.substr(i, 4) == "true") { onNumber(key, 1.0); i += 4; }
        else if (c == 'f' && s.substr(i, 5) == "false") { onNumber(key, 0.0); i += 5; }
        else if (c == 'n' && s.substr(i, 4) == "null") { i += 4; }
        else if (c == '{' || c == '[') { skipNested(); }
        else {
            double v = 0.0;
            const auto [ptr, ec] = std::from_chars(s.data() + i, s.data() + s.size(), v);
            if (ec != std::errc{}) return;
            onNumber(key, v);
            i = static_cast<std::size_t>(ptr - s.data());
        }
        ws();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        return;
    }
}

// Reads a JSON object of columns: {"t": 7, "id": [1, 5], "t_in": [20.5, 18.0], "on_ups": [false, true]}.
// Number and boolean arrays go to onColumn(key, values); a plain number goes to onScalar(key, value).
// Strings, nulls and arrays of anything else are skipped.
template <class OnColumn, class OnScalar>
void readColumnarJson(std::string_view s, OnColumn onColumn, OnScalar onScalar) {
    std::size_t i = 0;
    auto ws = [&] { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; };
    auto key = [&](std::string& out) -> bool {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size() && s[i] != '"') { if (s[i] == '\\' && i + 1 < s.size()) ++i; out.push_back(s[i++]); }
        if (i >= s.size()) return false;
        ++i;
        return true;
    };
    auto skipValue = [&] {
        int depth = 0;
        bool inStr = false;
        for (; i < s.size(); ++i) {
            const char c = s[i];
            if (inStr) { if (c == '\\') ++i; else if (c == '"') inStr = false; continue; }
            if (c == '"') inStr = true;
            else if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') { if (depth == 0) return; if (--depth == 0) { ++i; return; } }
            else if (c == ',' && depth == 0) return;
        }
    };
    auto number = [&](double& v) -> bool {
        if (s.substr(i, 4) == "true") { v = 1.0; i += 4; return true; }
        if (s.substr(i, 5) == "false") { v = 0.0; i += 5; return true; }
        const auto [ptr, ec] = std::from_chars(s.data() + i, s.data() + s.size(), v);
        if (ec != std::errc{}) return false;
        i = static_cast<std::size_t>(ptr - s.data());
        return true;
    };
    ws();
    if (i >= s.size() || s[i] != '{') return;
    ++i;
    std::string k;
    std::vector<double> values;
    while (true) {
        ws();
        if (i >= s.size() || s[i] == '}') return;
        if (!key(k)) return;
        ws();
        if (i >= s.size() || s[i] != ':') return;
        ++i;
        ws();
        if (i < s.size() && s[i] == '[') {
            const std::size_t start = i;
            ++i;
            values.clear();
            bool numeric = true;
            while (true) {
                ws();
                if (i < s.size() && s[i] == ']') { ++i; break; }
                double v;
                if (!number(v)) { numeric = false; break; }
                values.push_back(v);
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; break; }
                return;
            }
            if (numeric) onColumn(k, values);
            else { i = start; skipValue(); }
        } else {
            double v;
            const std::size_t start = i;
            if (number(v)) onScalar(k, v);
            else { i = start; skipValue(); }
        }
        ws();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        return;
    }
}

inline void appendJsonString(std::string& out, std::string_view s) {
    out.push_back('"');
    for (const char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (static_cast<unsigned char>(c) < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
        else out.push_back(c);
    }
    out.push_back('"');
}

inline void appendJsonNumber(std::string& out, double v) {
    char buf[32];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, ec == std::errc{} ? ptr : buf);
}

} // namespace hope::colony
