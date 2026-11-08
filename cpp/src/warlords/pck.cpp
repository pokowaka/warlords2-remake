#include "warlords/pck.hpp"

#include <algorithm>
#include <string>

#include "absl/base/no_destructor.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/span.h"

#include "util/util.hpp"
#include "warlords/bytes.hpp"

namespace w2::pck {

namespace {

using DecodedSheetMap = absl::flat_hash_map<std::string, std::shared_ptr<const Pixels>>;

class PckSheetCache {
  public:
    std::shared_ptr<const Pixels> Find(const std::string& key)
            ABSL_LOCKS_EXCLUDED(pck_cache_mutex_) {
        absl::MutexLock cache_lock(&pck_cache_mutex_);
        auto cached = sheets_.find(key);
        return cached == sheets_.end() ? nullptr : cached->second;
    }

    void Insert(const std::string& key, std::shared_ptr<const Pixels> sheet)
            ABSL_LOCKS_EXCLUDED(pck_cache_mutex_) {
        absl::MutexLock cache_lock(&pck_cache_mutex_);
        sheets_[key] = std::move(sheet);
    }

  private:
    absl::Mutex pck_cache_mutex_;
    DecodedSheetMap sheets_ ABSL_GUARDED_BY(pck_cache_mutex_);
};

PckSheetCache& GlobalPckCache() {
    static absl::NoDestructor<PckSheetCache> cache;
    return *cache;
}

/**
 * @brief Decompresses one bitplane (see the format in pck.hpp).
 * @param data Full `.PCK` file bytes.
 * @param pos Offset where the plane's compressed bytes start.
 * @param plane_size Expected decompressed byte length of the plane.
 * @param path Source file path for error reporting.
 * @param plane_index Bitplane index (0..3) for error reporting.
 * @param out Receives the decompressed plane.
 * @return Offset where the next plane starts, or `absl::InvalidArgumentError` on corruption.
 */
absl::StatusOr<size_t> DecodePlane(absl::Span<const uint8_t> data, size_t pos, size_t plane_size,
                                   const std::filesystem::path& path, int plane_index,
                                   std::vector<uint8_t>& out) {
    out.assign(plane_size, 0);
    size_t written = 0;
    while (written < plane_size) {
        if (pos >= data.size()) {
            return absl::InvalidArgumentError(absl::StrFormat(
                    "Failed to decode .PCK bitplane %d in '%s': compressed stream truncated at "
                    "byte %zu (wrote %zu of %zu plane bytes); verify the .PCK asset is intact.",
                    plane_index, path.string(), pos, written, plane_size));
        }
        int cmd = data[pos];
        if (cmd < 0x80) {
            size_t count = static_cast<size_t>(cmd) + 1;
            for (size_t i = 0; i < count; i++) {
                if (written + i < plane_size) {
                    out[written + i] = static_cast<uint8_t>(U8(data, pos + 1 + i));
                }
            }
            written += count;
            pos += 1 + count;
        } else {
            long dist = 65536 - (cmd * 256 + U8(data, pos + 1));
            int count = U8(data, pos + 2) + 1;
            pos += 3;
            for (int i = 0; i < count; i++) {
                long src = static_cast<long>(written) - dist;
                uint8_t value = src >= 0 ? out[static_cast<size_t>(src)] : 0;
                if (written < plane_size) out[written] = value;
                written++;
            }
        }
    }
    if (written != plane_size) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to decode .PCK bitplane %d in '%s': LZ77 run overran plane boundary "
                "(wrote %zu bytes, expected %zu); verify the .PCK asset is not corrupted.",
                plane_index, path.string(), written, plane_size));
    }
    return pos;
}

/**
 * @brief Compresses one bitplane and appends it to `out`.
 *
 * Bytes go out as literal runs of up to 128; a run of three or more copies
 * of the byte just written goes out as a match one byte back instead.
 */
void EncodePlane(absl::Span<const uint8_t> plane, std::vector<uint8_t>& out) {
    size_t pos = 0;
    size_t size = plane.size();
    std::vector<uint8_t> literals;
    auto flush = [&]() {
        for (size_t at = 0; at < literals.size(); at += 128) {
            size_t chunk = std::min<size_t>(128, literals.size() - at);
            out.push_back(static_cast<uint8_t>(chunk - 1));
            out.insert(out.end(), literals.begin() + static_cast<ptrdiff_t>(at),
                       literals.begin() + static_cast<ptrdiff_t>(at + chunk));
        }
        literals.clear();
    };
    while (pos < size) {
        size_t run = 0;
        if (pos > 0) {
            while (pos + run < size && run < 256 && plane[pos + run] == plane[pos - 1]) run++;
        }
        if (run >= 3) {
            flush();
            out.push_back(0xff);  // offset 0xffff: one byte back
            out.push_back(0xff);
            out.push_back(static_cast<uint8_t>(run - 1));
            pos += run;
        } else {
            literals.push_back(plane[pos++]);
        }
    }
    flush();
}

}  // namespace

absl::StatusOr<std::shared_ptr<const Pixels>> Decode(const std::filesystem::path& path) {
    std::string cache_key = Upper(path.string());
    if (auto cached = GlobalPckCache().Find(cache_key)) return cached;

    auto data = ReadBytes(path);
    if (!data.ok()) return data.status();
    absl::Span<const uint8_t> bytes(*data);
    int version = U16(bytes, 0);
    if (version != 1) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to decode .PCK image '%s': expected header version 1, got %d; verify the "
                "file is a Warlords II 4bpp .PCK sheet.",
                path.string(), version));
    }
    auto pixels = std::make_shared<Pixels>();
    pixels->w = U16(bytes, 2);
    pixels->h = U16(bytes, 4);
    size_t rowbytes = static_cast<size_t>(pixels->w) / 8;
    size_t plane_size = rowbytes * static_cast<size_t>(pixels->h);
    pixels->px.assign(static_cast<size_t>(pixels->w) * pixels->h, 0);
    size_t pos = 6;
    std::vector<uint8_t> plane;
    for (int plane_index = 0; plane_index < 4; plane_index++) {
        auto next_pos = DecodePlane(bytes, pos, plane_size, path, plane_index, plane);
        if (!next_pos.ok()) return next_pos.status();
        pos = *next_pos;
        uint8_t bit = static_cast<uint8_t>(1 << plane_index);
        for (int y = 0; y < pixels->h; y++) {
            for (size_t byte_x = 0; byte_x < rowbytes; byte_x++) {
                uint8_t plane_byte = plane[static_cast<size_t>(y) * rowbytes + byte_x];
                if (!plane_byte) continue;
                size_t row_start = static_cast<size_t>(y) * pixels->w + byte_x * 8;
                for (int bit_index = 0; bit_index < 8; bit_index++) {
                    if (plane_byte & (0x80 >> bit_index)) pixels->px[row_start + bit_index] |= bit;
                }
            }
        }
    }
    GlobalPckCache().Insert(cache_key, pixels);
    return pixels;
}

absl::StatusOr<std::vector<uint8_t>> Encode(const Pixels& pixels) {
    if (pixels.w <= 0 || pixels.w % 8 != 0 || pixels.h < 0) {
        return absl::InvalidArgumentError(absl::StrFormat(
                "Failed to encode .PCK image (%dx%d): width must be a positive multiple of 8 and "
                "height non-negative; pad or crop the pixel sheet before encoding.",
                pixels.w, pixels.h));
    }
    std::vector<uint8_t> out;
    auto put16 = [&](int value) {
        out.push_back(static_cast<uint8_t>(value & 0xff));
        out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    };
    put16(1);
    put16(pixels.w);
    put16(pixels.h);
    size_t rowbytes = static_cast<size_t>(pixels.w) / 8;
    for (int plane_index = 0; plane_index < 4; plane_index++) {
        std::vector<uint8_t> plane(rowbytes * static_cast<size_t>(pixels.h), 0);
        for (int y = 0; y < pixels.h; y++) {
            for (int x = 0; x < pixels.w; x++) {
                if (pixels.At(x, y) & (1 << plane_index)) {
                    plane[static_cast<size_t>(y) * rowbytes + static_cast<size_t>(x) / 8] |=
                            static_cast<uint8_t>(0x80 >> (x % 8));
                }
            }
        }
        EncodePlane(plane, out);
    }
    return out;
}

}  // namespace w2::pck
