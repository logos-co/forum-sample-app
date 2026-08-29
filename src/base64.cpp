#include "base64.h"

#include <array>

std::string base64Decode(const std::string& input) {
    static const auto table = [] {
        std::array<int, 256> t{};
        t.fill(-1);
        const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) t[static_cast<unsigned char>(chars[i])] = i;
        return t;
    }();

    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : input) {
        if (c == '=') break;
        if (table[c] == -1) continue;
        val = (val << 6) + table[c];
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}
