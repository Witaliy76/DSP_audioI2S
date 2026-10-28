// Host fixture for AF-2.1 / E-AT2.
// Calls the production parser. An invalid URL must not overwrite a committed endpoint.

#include "../../src/audio_url.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failed = 0;

void check(bool cond, const char* expr, int line) {
    if (cond) return;
    std::printf("FAIL %s:%d: %s\n", "url_validation_test.cpp", line, expr);
    ++g_failed;
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

bool slice_eq(const char* data, size_t len, const char* literal) {
    const size_t n = std::strlen(literal);
    if (len != n) return false;
    if (n == 0) return true;
    return std::memcmp(data, literal, n) == 0;
}

bool same_endpoint(const audiolib::ParsedHttpUrl& a, const audiolib::ParsedHttpUrl& b) {
    return a.ssl == b.ssl && a.explicit_port == b.explicit_port && a.has_path == b.has_path && a.has_query == b.has_query && a.port == b.port && a.host == b.host &&
           a.host_len == b.host_len && a.path == b.path && a.path_len == b.path_len && a.query == b.query && a.query_len == b.query_len;
}

void expect_rejected(const char* url, const audiolib::ParsedHttpUrl& live) {
    audiolib::ParsedHttpUrl slot = live;
    const bool              ok = audiolib::tryParseHttpEndpoint(url, &slot);
    CHECK(!ok);
    CHECK(same_endpoint(slot, live));
}

void expect_http(const char* url, bool ssl, bool explicit_port, uint16_t port, const char* host, const char* path, const char* query) {
    audiolib::ParsedHttpUrl parsed;
    CHECK(audiolib::tryParseHttpEndpoint(url, &parsed));
    CHECK(parsed.ssl == ssl);
    CHECK(parsed.explicit_port == explicit_port);
    CHECK(parsed.port == port);
    CHECK(slice_eq(parsed.host, parsed.host_len, host));
    if (!path) {
        CHECK(!parsed.has_path);
        CHECK(parsed.path_len == 0);
    } else {
        CHECK(parsed.has_path);
        CHECK(slice_eq(parsed.path, parsed.path_len, path));
    }
    if (!query) {
        CHECK(!parsed.has_query);
        CHECK(parsed.query_len == 0);
    } else {
        CHECK(parsed.has_query);
        CHECK(slice_eq(parsed.query, parsed.query_len, query));
    }
}

std::string overlong_url() {
    std::string url = "http://example.com/";
    url.append(audiolib::kMaxHttpUrlLength, 'a');
    return url;
}

} // namespace

int main() {
    audiolib::ParsedHttpUrl live{};
    CHECK(audiolib::tryParseHttpEndpoint("https://radio.example:8443/live/a%2Fb.mp3?tok=a%2Fb", &live));
    CHECK(live.ssl);
    CHECK(live.port == 8443);
    CHECK(slice_eq(live.host, live.host_len, "radio.example"));
    CHECK(slice_eq(live.path, live.path_len, "live/a%2Fb.mp3"));
    CHECK(slice_eq(live.query, live.query_len, "tok=a%2Fb"));
    const audiolib::ParsedHttpUrl committed = live;

    expect_rejected("", committed);
    expect_rejected(nullptr, committed);
    expect_rejected("http://", committed);
    expect_rejected("https://", committed);
    expect_rejected("http:///", committed);
    expect_rejected("http://:80/stream", committed);
    expect_rejected("http:/example.com/a", committed);
    expect_rejected("https:example.com/a", committed);
    expect_rejected("http:://example.com/a", committed);
    expect_rejected("http://example.com:", committed);
    expect_rejected("http://example.com:/", committed);
    expect_rejected("http://example.com:abc/a", committed);
    expect_rejected("http://example.com:0/a", committed);
    const std::string too_long = overlong_url();
    expect_rejected(too_long.c_str(), committed);
    expect_rejected("http://example.com:65536/x", committed);
    CHECK(same_endpoint(live, committed));
    CHECK(!audiolib::tryParseHttpEndpoint("http://example.com/", nullptr));

    std::string at_limit = "http://example.com/";
    at_limit.append(audiolib::kMaxHttpUrlLength - at_limit.size(), 'p');
    CHECK(at_limit.size() == audiolib::kMaxHttpUrlLength);
    CHECK(audiolib::tryParseHttpEndpoint(at_limit.c_str(), &live));
    CHECK(!same_endpoint(live, committed));
    CHECK(slice_eq(live.host, live.host_len, "example.com"));
    CHECK(live.port == 80);
    CHECK(!live.ssl);
    CHECK(audiolib::classifyConnectHost(at_limit.c_str()) == audiolib::ConnectHostGate::Accept);
    std::string one_over = at_limit;
    one_over.push_back('z');
    expect_rejected(one_over.c_str(), committed);
    CHECK(audiolib::classifyConnectHost(one_over.c_str()) == audiolib::ConnectHostGate::TooLong);

    expect_http("http://example.com", false, false, 80, "example.com", nullptr, nullptr);
    expect_http("https://example.com", true, false, 443, "example.com", nullptr, nullptr);
    expect_http("http://example.com:8000/live/stream.mp3", false, true, 8000, "example.com", "live/stream.mp3", nullptr);
    expect_http("https://radio.example/a/b", true, false, 443, "radio.example", "a/b", nullptr);
    expect_http("http://example.com/a/b?x=1&y=2", false, false, 80, "example.com", "a/b", "x=1&y=2");
    expect_http("http://example.com/a%2Fb%20c.mp3?tok=a%2Fb", false, false, 80, "example.com", "a%2Fb%20c.mp3", "tok=a%2Fb");
    expect_http("http://example.com/a/b?x=1:2", false, false, 80, "example.com", "a/b", "x=1:2");
    expect_http("example.com:8000/live", false, true, 8000, "example.com", "live", nullptr);
    expect_http("http://example.com:080/x", false, true, 80, "example.com", "x", nullptr);
    expect_http("http://example.com:65535/x", false, true, 65535, "example.com", "x", nullptr);

    CHECK(audiolib::classifyConnectHost(nullptr) == audiolib::ConnectHostGate::Empty);
    CHECK(audiolib::classifyConnectHost("") == audiolib::ConnectHostGate::TooShort);
    CHECK(audiolib::classifyConnectHost("abcdefg") == audiolib::ConnectHostGate::TooShort);
    CHECK(audiolib::tryParseHttpEndpoint("abcdefg", &live));
    CHECK(slice_eq(live.host, live.host_len, "abcdefg"));
    CHECK(live.port == 80);
    CHECK(!live.ssl);
    CHECK(audiolib::classifyConnectHost(too_long.c_str()) == audiolib::ConnectHostGate::TooLong);
    CHECK(audiolib::classifyConnectHost("http://") == audiolib::ConnectHostGate::TooShort);
    CHECK(audiolib::classifyConnectHost("https://") == audiolib::ConnectHostGate::Malformed);
    CHECK(audiolib::classifyConnectHost("http:/example.com/a") == audiolib::ConnectHostGate::Malformed);
    CHECK(audiolib::classifyConnectHost("http://example.com:0/a") == audiolib::ConnectHostGate::Malformed);
    CHECK(audiolib::classifyConnectHost("http://example.com/a%2Fb.mp3?tok=a%2Fb") == audiolib::ConnectHostGate::Accept);
    CHECK(audiolib::classifyConnectHost("https://example.com:8443/live") == audiolib::ConnectHostGate::Accept);

    if (g_failed) {
        std::printf("url_validation: %d failed\n", g_failed);
        return 1;
    }
    std::printf("url_validation: PASS\n");
    return 0;
}
