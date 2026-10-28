#pragma once

// HTTP endpoint split shared by connecttohost, httpPrint and httpRange.
// Slices follow the 4.0.1 dismantle_host rules: case-sensitive http/https,
// a missing scheme stays plain HTTP, path and query bytes are copied raw.
// tryParseHttpEndpoint writes *out only after the URL is usable. A false
// return leaves *out unchanged so callers can fail before any transport write.

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace audiolib {

constexpr size_t kMaxHttpUrlLength = 2048;          // existing connecttohost ceiling
constexpr size_t kMinConnecttohostUrlLength = 8;    // existing connecttohost floor

struct ParsedHttpUrl {
    bool        ssl = false;
    bool        explicit_port = false;
    bool        has_path = false;
    bool        has_query = false;
    uint16_t    port = 0;
    const char* host = nullptr;
    size_t      host_len = 0;
    const char* path = nullptr;
    size_t      path_len = 0;
    const char* query = nullptr;
    size_t      query_len = 0;
};

enum class ConnectHostGate : uint8_t { Accept = 0, Empty, TooShort, TooLong, Malformed };

// False leaves *out untouched.
inline bool tryParseHttpEndpoint(const char* url, ParsedHttpUrl* out) {
    if (!out || !url || url[0] == '\0') return false;

    const size_t url_len = std::strlen(url);
    if (url_len > kMaxHttpUrlLength) return false;

    const char* p = url;
    bool        ssl = false;
    if (std::strncmp(p, "https://", 8) == 0) {
        ssl = true;
        p += 8;
    } else if (std::strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (std::strncmp(p, "https:", 6) == 0 || std::strncmp(p, "http:", 5) == 0) {
        return false; // scheme token without "://"
    }

    const char* const rest_end = url + url_len;
    const char* const port_sep = std::strchr(p, ':');
    const char* const path_sep = std::strchr(p, '/');
    const char* const query_sep = std::strchr(p, '?');

    const char* host_end = rest_end;
    if (port_sep && port_sep < host_end) host_end = port_sep;
    if (path_sep && path_sep < host_end) host_end = path_sep;
    if (query_sep && query_sep < host_end) host_end = query_sep;
    if (host_end == p) return false;

    const bool explicit_port = port_sep && (!path_sep || port_sep < path_sep);
    uint16_t   port = ssl ? static_cast<uint16_t>(443) : static_cast<uint16_t>(80);
    if (explicit_port) {
        // Same atoi-then-uint16_t narrowing the 4.0.1 parser stored.
        port = static_cast<uint16_t>(std::atoi(port_sep + 1));
        if (port == 0) return false;
    }

    ParsedHttpUrl parsed;
    parsed.ssl = ssl;
    parsed.explicit_port = explicit_port;
    parsed.port = port;
    parsed.host = p;
    parsed.host_len = static_cast<size_t>(host_end - p);
    if (path_sep) {
        const char* path_start = path_sep + 1;
        const char* path_end = query_sep ? query_sep : rest_end;
        if (path_end >= path_start) {
            parsed.has_path = true;
            parsed.path = path_start;
            parsed.path_len = static_cast<size_t>(path_end - path_start);
        }
    }
    if (query_sep) {
        parsed.has_query = true;
        parsed.query = query_sep + 1;
        parsed.query_len = static_cast<size_t>(rest_end - (query_sep + 1));
    }

    *out = parsed;
    return true;
}

// connecttohost length policy plus the structural parse.
// httpPrint and httpRange do not use the 8-character minimum.
inline ConnectHostGate classifyConnectHost(const char* url) {
    if (!url) return ConnectHostGate::Empty;
    const size_t n = std::strlen(url);
    if (n < kMinConnecttohostUrlLength) return ConnectHostGate::TooShort;
    if (n > kMaxHttpUrlLength) return ConnectHostGate::TooLong;
    ParsedHttpUrl ignored;
    if (!tryParseHttpEndpoint(url, &ignored)) return ConnectHostGate::Malformed;
    return ConnectHostGate::Accept;
}

} // namespace audiolib
