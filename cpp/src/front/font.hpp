/**
 * @file
 * @brief The original's proportional fonts: TEXT, CHANCE17 and CHANCE36.
 *
 * A .FNT is a .PCK image -- a sheet of glyphs -- and the .FIN beside it holds
 * the metrics (see docs/formats/font.md). Glyphs are packed left to right
 * into rows `line_height` tall, each taking a slot rounded up to a multiple
 * of 8. Text is bytes, one character a byte, as the game's files give it.
 */
#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include "absl/status/statusor.h"

#include "platform/gfx.hpp"
#include "warlords/pal.hpp"
#include "warlords/pck.hpp"

/**
 * @brief One of the original's bitmap fonts, ready to draw.
 *
 * Copies share the metrics, the glyph sheet and the cache of recoloured
 * variants, so a Font is cheap to pass around.
 */
class Font {
  public:
    /** @brief Loads DATA/<name>.FIN and .FNT, keying out palette index `key_index` (-1 for none).
     */
    static absl::StatusOr<std::shared_ptr<Font>> Load(const std::filesystem::path& data_dir,
                                                      const std::string& name,
                                                      const w2::pal::Palette& palette,
                                                      int key_index);
    /** @brief How far the pen moves drawing `text`, in pixels. */
    int Width(const std::string& text) const;
    /** @brief Draws `text` with its top left at (x, y); returns the width drawn. */
    int Draw(const std::string& text, double x, double y) const;
    /** @brief Draws centred in a rect, the way the original centres a control's text. */
    void DrawCentred(const std::string& text, double rect_x, double rect_y, double rect_width,
                     double rect_height) const;
    /**
     * @brief The same font in other colours, as the original asks for it.
     *
     * The sheet's glyph colour (15) becomes `glyph` and its outline (0)
     * `outline`. Each pair is made once and kept.
     */
    const Font& Colours(int glyph, int outline) const;
    int line_height = 0;  ///< Pixels from one line's top to the next's.
    int baseline = 0;     ///< Pixels from a line's top to its baseline.

  private:
    /** @brief What the .FIN says about the glyphs. */
    struct Metrics {
        int count = 0;                 ///< Entries in the .FIN; the last has no glyph.
        int first = 0;                 ///< The character code of the first entry.
        int sheet_width = 0;           ///< The glyph sheet's width, where a row wraps.
        std::array<int, 256> ink{};    ///< How wide each character's art is.
        std::array<int, 256> width{};  ///< How far the pen moves past each character.
    };
    std::shared_ptr<Metrics> m_;                      ///< The glyphs' metrics.
    std::shared_ptr<std::map<int, gfx::Quad>> quad_;  ///< Each character's cell on the sheet.
    gfx::ImageP image_;                               ///< The glyph sheet in this font's colours.
    std::shared_ptr<const w2::pck::Pixels> px_;  ///< The sheet's palette indices, for recolouring.
    w2::pal::Palette palette_;                   ///< The palette the sheet is drawn in.
    int key_ = -1;                               ///< The index keyed out, -1 for none.
    /** Recoloured copies by glyph * 16 + outline, shared by every copy. */
    mutable std::shared_ptr<std::map<int, std::shared_ptr<Font>>> variants_;
};
/** A loaded font, shared. */
using FontP = std::shared_ptr<Font>;
