#include "utils.hpp"
#include "credentials.hpp"
#include <platform/platform.hpp>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <array>

std::string get_cluster_username() {
    auto result = CredentialManager::instance().get("user");
    return result.is_ok() ? result.value : "unknown";
}

std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    return std::string(buf);
}

std::time_t parse_iso_time(const std::string& iso) {
    struct tm tm_buf = {};
    if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d",
               &tm_buf.tm_year, &tm_buf.tm_mon, &tm_buf.tm_mday,
               &tm_buf.tm_hour, &tm_buf.tm_min, &tm_buf.tm_sec) == 6) {
        tm_buf.tm_year -= 1900;
        tm_buf.tm_mon -= 1;
        return mktime(&tm_buf);
    }
    return 0;
}

int safe_stoi(const std::string& s, int fallback) {
    try {
        return std::stoi(s);
    } catch (...) {
        return fallback;
    }
}

static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::string& input) {
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    const auto* data = reinterpret_cast<const unsigned char*>(input.data());
    size_t len = input.size();
    for (size_t i = 0; i < len; i += 3) {
        unsigned val = data[i] << 16;
        if (i + 1 < len) val |= data[i + 1] << 8;
        if (i + 2 < len) val |= data[i + 2];
        out += B64_CHARS[(val >> 18) & 0x3F];
        out += B64_CHARS[(val >> 12) & 0x3F];
        out += (i + 1 < len) ? B64_CHARS[(val >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? B64_CHARS[val & 0x3F] : '=';
    }
    return out;
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::string base64_decode(const std::string& input) {
    std::string out;
    out.reserve(input.size() * 3 / 4);
    int val = 0, bits = -8;
    for (char c : input) {
        if (c == '\r' || c == '\n' || c == ' ') continue;
        int v = b64_val(c);
        if (v < 0) break;  // '=' or invalid → stop
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 0) {
            out += static_cast<char>((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

// ── In-process MD5 (RFC 1321) ─────────────────────────────
// Avoids fork+exec per file (~400ms each → 13s for 31 files).

namespace {

struct MD5Context {
    uint32_t state[4];
    uint64_t count;
    uint8_t buffer[64];
};

static constexpr uint32_t S[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

static constexpr uint32_t K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};

inline uint32_t rotl(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

void md5_transform(MD5Context& ctx, const uint8_t block[64]) {
    uint32_t M[16];
    for (int i = 0; i < 16; ++i)
        M[i] = uint32_t(block[i*4]) | (uint32_t(block[i*4+1]) << 8) |
                (uint32_t(block[i*4+2]) << 16) | (uint32_t(block[i*4+3]) << 24);

    uint32_t a = ctx.state[0], b = ctx.state[1], c = ctx.state[2], d = ctx.state[3];

    for (int i = 0; i < 64; ++i) {
        uint32_t f, g;
        if (i < 16)      { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5*i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d;           g = (3*i + 5) % 16; }
        else              { f = c ^ (b | ~d);        g = (7*i) % 16; }
        f += a + K[i] + M[g];
        a = d; d = c; c = b; b += rotl(f, S[i]);
    }
    ctx.state[0] += a; ctx.state[1] += b; ctx.state[2] += c; ctx.state[3] += d;
}

void md5_init(MD5Context& ctx) {
    ctx.state[0] = 0x67452301; ctx.state[1] = 0xefcdab89;
    ctx.state[2] = 0x98badcfe; ctx.state[3] = 0x10325476;
    ctx.count = 0;
}

void md5_update(MD5Context& ctx, const uint8_t* data, size_t len) {
    size_t offset = ctx.count % 64;
    ctx.count += len;
    for (size_t i = 0; i < len; ++i) {
        ctx.buffer[offset++] = data[i];
        if (offset == 64) {
            md5_transform(ctx, ctx.buffer);
            offset = 0;
        }
    }
}

std::string md5_final(MD5Context& ctx) {
    uint64_t bits = ctx.count * 8;
    uint8_t pad = 0x80;
    md5_update(ctx, &pad, 1);
    pad = 0;
    while (ctx.count % 64 != 56) md5_update(ctx, &pad, 1);
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; ++i) len_bytes[i] = uint8_t(bits >> (i * 8));
    md5_update(ctx, len_bytes, 8);

    char hex[33];
    for (int i = 0; i < 4; ++i) {
        uint32_t v = ctx.state[i];
        snprintf(hex + i*8, 9, "%02x%02x%02x%02x",
                 v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff);
    }
    return std::string(hex, 32);
}

} // anonymous namespace

std::string shell_quote(const std::string& s) {
    // Wrap in single quotes, escaping any embedded single quotes
    std::string result = "'";
    for (char c : s) {
        if (c == '\'')
            result += "'\\''";
        else
            result += c;
    }
    result += "'";
    return result;
}

std::string compute_file_md5(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";

    MD5Context ctx;
    md5_init(ctx);

    uint8_t buf[65536];
    while (f) {
        f.read(reinterpret_cast<char*>(buf), sizeof(buf));
        auto n = f.gcount();
        if (n > 0) md5_update(ctx, buf, static_cast<size_t>(n));
    }
    return md5_final(ctx);
}
