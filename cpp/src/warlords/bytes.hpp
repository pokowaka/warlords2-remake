/**
 * @file
 * @brief Reading the original's little-endian binary records.
 *
 * Offsets are 0-based, as in docs/formats. Reads past the end of the data
 * yield 0, so a short file reads as one padded with zeros.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "absl/types/span.h"

namespace w2 {

/** @brief The unsigned byte at `offset`, or 0 past the end of `bytes`. */
inline int U8(absl::Span<const uint8_t> bytes, size_t offset) {
    return offset < bytes.size() ? bytes[offset] : 0;
}
/** @brief The unsigned little-endian 16-bit word at `offset`. */
inline int U16(absl::Span<const uint8_t> bytes, size_t offset) {
    return U8(bytes, offset) + U8(bytes, offset + 1) * 256;
}
/** @brief The signed (two's complement) little-endian 16-bit word at `offset`. */
inline int I16(absl::Span<const uint8_t> bytes, size_t offset) {
    int value = U16(bytes, offset);
    return value >= 0x8000 ? value - 0x10000 : value;
}

/**
 * @brief A NUL-terminated string in a fixed field, one character a byte.
 * @param bytes The record data.
 * @param offset Where the field starts.
 * @param length The field's width; the string stops at the first NUL or here.
 */
inline std::string Cstr(absl::Span<const uint8_t> bytes, size_t offset, size_t length) {
    std::string text;
    for (size_t i = offset; i < offset + length && i < bytes.size(); i++) {
        if (bytes[i] == 0) break;
        text += static_cast<char>(bytes[i]);
    }
    return text;
}

}  // namespace w2
