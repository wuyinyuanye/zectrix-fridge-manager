#ifndef ZECTRIX_NFC_SHOPPING_H_
#define ZECTRIX_NFC_SHOPPING_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "fridge_store.h"

namespace nfc_shopping {

inline std::string EncodeBase64Url(const std::string& input) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    size_t i = 0;
    const size_t n = input.size();
    while (i + 2 < n) {
        const uint32_t v = (static_cast<uint32_t>(static_cast<uint8_t>(input[i])) << 16) |
                           (static_cast<uint32_t>(static_cast<uint8_t>(input[i + 1])) << 8) |
                           static_cast<uint32_t>(static_cast<uint8_t>(input[i + 2]));
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back(kTable[v & 0x3F]);
        i += 3;
    }
    if (i < n) {
        const uint32_t b0 = static_cast<uint8_t>(input[i]);
        const uint32_t b1 = (i + 1 < n) ? static_cast<uint8_t>(input[i + 1]) : 0;
        out.push_back(kTable[(b0 >> 2) & 0x3F]);
        out.push_back(kTable[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0F)]);
        if (i + 1 < n) {
            out.push_back(kTable[(b1 & 0x0F) << 2]);
        }
    }
    return out;
}

// Compute the exact Type-2 NDEF TLV byte length that ZectrixNfc::WriteUriNdef(uri) will write.
inline size_t EstimateType2UriTlvBytes(const std::string& uri) {
    size_t suffix_len = uri.size();
    if (uri.rfind("https://www.", 0) == 0) suffix_len -= 12;
    else if (uri.rfind("http://www.", 0) == 0) suffix_len -= 11;
    else if (uri.rfind("https://", 0) == 0) suffix_len -= 8;
    else if (uri.rfind("http://", 0) == 0) suffix_len -= 7;

    const size_t payload_len = 1 + suffix_len;
    const size_t ndef_len = (payload_len <= 0xFF ? 4 : 7) + payload_len;
    const size_t tlv_hdr = (ndef_len <= 0xFE) ? 2 : 4;
    return tlv_hdr + ndef_len + 1;  // +1 for 0xFE terminator
}

// Pack unchecked shopping items into a self-contained fragment URL:
//   <base_url>#<base64url(utf8 "name\tqty\n...")>
// Fragment (#...) is decoded purely client-side in the phone browser and is NEVER sent to any server.
// Automatically caps items to guarantee the resulting Type-2 NDEF TLV fits inside `capacity_bytes` (880B).
inline std::string BuildNfcShoppingUrl(const std::string& base_url,
                                       const std::vector<ShoppingItem>& list,
                                       size_t capacity_bytes = 880) {
    std::string prefix = base_url;
    if (prefix.empty()) return "";
    const size_t hash_pos = prefix.find('#');
    if (hash_pos != std::string::npos) prefix.resize(hash_pos);
    prefix.push_back('#');

    if (EstimateType2UriTlvBytes(prefix) > capacity_bytes) {
        return "";
    }

    std::string raw;
    size_t packed = 0;
    size_t total_unchecked = 0;
    for (const auto& it : list) {
        if (!it.checked) total_unchecked++;
    }

    for (const auto& it : list) {
        if (it.checked) continue;
        std::string line = it.name;
        if (!it.qty.empty() && it.qty != "1份") {
            line.push_back('\t');
            line += it.qty;
        }
        std::string candidate = raw;
        if (!candidate.empty()) candidate.push_back('\n');
        candidate += line;
        const std::string cand_url = prefix + EncodeBase64Url(candidate);
        if (EstimateType2UriTlvBytes(cand_url) <= capacity_bytes) {
            raw = std::move(candidate);
            packed++;
        } else {
            break;
        }
    }

    // If truncated due to 880B EEPROM limit, try appending a compact "+N样" indicator
    if (packed < total_unchecked) {
        std::string tail = raw;
        if (!tail.empty()) tail.push_back('\n');
        tail += "其余 ";
        tail += std::to_string(total_unchecked - packed);
        tail += " 样请查看设备";
        const std::string tail_url = prefix + EncodeBase64Url(tail);
        if (EstimateType2UriTlvBytes(tail_url) <= capacity_bytes) {
            raw = std::move(tail);
        }
    }

    return prefix + EncodeBase64Url(raw);
}

}  // namespace nfc_shopping

#endif  // ZECTRIX_NFC_SHOPPING_H_
