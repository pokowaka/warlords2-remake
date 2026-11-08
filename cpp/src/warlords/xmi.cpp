#include "warlords/xmi.hpp"

#include <algorithm>
#include <string_view>

namespace w2::xmi {

namespace {

/** @brief The unsigned big-endian 32-bit word at `offset` (IFF chunk sizes). */
uint32_t U32be(absl::Span<const uint8_t> bytes, size_t offset) {
    return (static_cast<uint32_t>(bytes[offset]) << 24) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<uint32_t>(bytes[offset + 3]);
}

bool ChunkIdIs(absl::Span<const uint8_t> bytes, size_t pos, std::string_view expected) {
    return bytes[pos] == static_cast<uint8_t>(expected[0]) &&
           bytes[pos + 1] == static_cast<uint8_t>(expected[1]) &&
           bytes[pos + 2] == static_cast<uint8_t>(expected[2]) &&
           bytes[pos + 3] == static_cast<uint8_t>(expected[3]);
}

/**
 * @brief Finds the EVNT chunk of the first sequence.
 *
 * Walks the IFF chunks in bytes [from, end), descending into FORM and CAT
 * containers.
 *
 * @param start Receives where the EVNT chunk's data starts.
 * @param len Receives the EVNT chunk's length.
 * @return Whether an EVNT chunk was found.
 */
bool Walk(absl::Span<const uint8_t> bytes, size_t from, size_t end, size_t& start, size_t& len) {
    size_t pos = from;
    while (pos + 8 <= end) {
        size_t size = U32be(bytes, pos + 4);
        if (ChunkIdIs(bytes, pos, "FORM") || ChunkIdIs(bytes, pos, "CAT ")) {
            if (Walk(bytes, pos + 12, std::min(end, pos + 8 + size), start, len)) return true;
        } else if (ChunkIdIs(bytes, pos, "EVNT")) {
            start = pos + 8;
            len = size;
            return true;
        }
        pos += 8 + size + size % 2;
    }
    return false;
}

/** @brief An event as read, before it is put in time order. */
struct Raw {
    int tick;           ///< When it happens.
    int order;          ///< Its place in the file, from 1; -1 for a note-off.
    uint8_t status;     ///< MIDI status byte.
    uint8_t data1;      ///< First data byte.
    uint8_t data2;      ///< Second data byte, 0 when there is none.
    size_t file_index;  ///< Its index in the read list, the sort's tie-breaker.
};

}  // namespace

std::optional<Sequence> Parse(absl::Span<const uint8_t> bytes) {
    size_t start = 0;
    size_t len = 0;
    if (!Walk(bytes, 0, bytes.size(), start, len)) return std::nullopt;
    size_t stop = std::min(bytes.size(), start + len);
    std::vector<Raw> raw;
    size_t pos = start;
    int tick = 0;
    int order = 0;
    int finish = 0;
    auto byte = [&](size_t offset) -> uint8_t { return offset < bytes.size() ? bytes[offset] : 0; };
    // a MIDI variable-length number: seven bits a byte, high bit set on all but the last
    auto varlen = [&]() {
        int value = 0;
        uint8_t next;
        do {
            next = byte(pos++);
            value = value * 128 + (next & 0x7f);
        } while (next >= 0x80 && pos < stop);
        return value;
    };
    while (pos < stop) {
        uint8_t status = byte(pos);
        if (status < 0x80) {
            tick += status;
            pos++;
            continue;
        }
        pos++;
        uint8_t high_nibble = status & 0xf0;
        if (status == 0xff) {
            uint8_t kind = byte(pos++);
            int length = varlen();
            pos += length;
            if (kind == 0x2f) break;
        } else if (status == 0xf0 || status == 0xf7) {
            pos += varlen();
        } else if (high_nibble == 0x90) {
            uint8_t key = byte(pos), vel = byte(pos + 1);
            pos += 2;
            int dur = varlen();
            raw.push_back({tick, ++order, status, key, vel, 0});
            // the note's end sorts before anything else at its tick
            raw.push_back({tick + dur, -1, static_cast<uint8_t>(0x80 | (status & 15)), key, 0, 0});
            finish = std::max(finish, tick + dur);
        } else if (high_nibble == 0xc0 || high_nibble == 0xd0) {
            raw.push_back({tick, ++order, status, byte(pos), 0, 0});
            pos++;
        } else {
            raw.push_back({tick, ++order, status, byte(pos), byte(pos + 1), 0});
            pos += 2;
        }
    }
    finish = std::max(finish, tick);
    // a stable sort: by tick, note-offs first, then the file's own order
    for (size_t i = 0; i < raw.size(); i++) raw[i].file_index = i;
    std::sort(raw.begin(), raw.end(), [](const Raw& lhs, const Raw& rhs) {
        if (lhs.tick != rhs.tick) return lhs.tick < rhs.tick;
        if ((lhs.order < 0) != (rhs.order < 0)) return lhs.order < 0;
        return lhs.file_index < rhs.file_index;
    });
    Sequence seq;
    seq.length = finish;
    seq.events.reserve(raw.size());
    for (auto& event : raw)
        seq.events.push_back({event.tick, event.status, event.data1, event.data2});
    return seq;
}

}  // namespace w2::xmi
