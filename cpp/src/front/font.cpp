#include "front/font.hpp"

#include <cmath>
#include <cstdint>
#include <set>

#include "front/pckimage.hpp"
#include "util/util.hpp"

absl::StatusOr<Font::SharedFont> Font::Load(gfx::Renderer& renderer,
                                            const std::filesystem::path& data_dir,
                                            const std::string& name,
                                            const w2::pal::Palette& palette, int key_index) {
    auto font = std::make_shared<Font>();
    font->renderer_ = &renderer;
    auto metrics_file = w2::ReadBytes(data_dir / (name + ".FIN"));
    if (!metrics_file.ok()) return metrics_file.status();
    auto metrics = std::make_shared<Metrics>();
    // a byte of the .FIN, 0 past its end
    auto byte_at = [&](size_t offset) {
        return offset < metrics_file->size() ? static_cast<int>((*metrics_file)[offset]) : 0;
    };
    metrics->count = byte_at(0);
    metrics->first = byte_at(1);
    metrics->sheet_width = byte_at(2) * 256 + byte_at(3);  // big-endian, unlike everything else
    font->line_height_ = byte_at(4);
    font->baseline_ = byte_at(5);
    // how wide each glyph's art is, then how far the pen moves on
    for (int i = 0; i < metrics->count; i++) {
        int code = (metrics->first + i) & 255;
        metrics->ink[code] = byte_at(11 + static_cast<size_t>(i));
        metrics->width[code] = byte_at(11 + static_cast<size_t>(metrics->count + i));
    }
    font->m_ = metrics;
    auto decoded_px = w2::pck::Decode(data_dir / (name + ".FNT"));
    if (!decoded_px.ok()) return decoded_px.status();
    font->px_ = std::move(*decoded_px);
    font->palette_ = palette;
    font->key_ = key_index;
    font->image_ =
            pckimage::FromPixels(renderer, *font->px_, palette, {},
                                 key_index >= 0 ? std::set<int>{key_index} : std::set<int>{});
    font->quad_ = std::make_shared<QuadMap>();
    int x = 0;
    int row = 0;
    for (int i = 0; i <= metrics->count - 2; i++) {  // the last entry has no glyph
        int code = (metrics->first + 1 + i) & 255;
        int glyph_width = metrics->ink[code];
        int slot = (glyph_width + 7) / 8 * 8;
        if (x + slot > metrics->sheet_width) {
            x = 0;
            row++;
        }
        if (glyph_width > 0) {
            (*font->quad_)[code] =
                    gfx::NewQuad(x, row * font->line_height_, glyph_width, font->line_height_);
        }
        x += slot;
    }
    font->variants_ = std::make_shared<VariantMap>();
    return font;
}

int Font::Width(const std::string& text) const {
    int width = 0;
    for (unsigned char character : text) width += m_->width[character];
    return width;
}

int Font::Draw(const std::string& text, double x, double y) const {
    double pen_x = x;
    for (unsigned char character : text) {
        auto quad = quad_->find(character);
        if (quad != quad_->end() && renderer_ != nullptr) {
            renderer_->Draw(image_, quad->second, pen_x, y);
        }
        pen_x += m_->width[character];
    }
    return static_cast<int>(pen_x - x);
}

void Font::DrawCentred(const std::string& text, double rect_x, double rect_y, double rect_width,
                       double rect_height) const {
    Draw(text, rect_x + std::floor((rect_width - Width(text)) / 2),
         rect_y + std::floor((rect_height - line_height_) / 2) + 1);
}

const Font& Font::Colours(int glyph, int outline) const {
    if (glyph == 15 && outline == 0) return *this;
    int variant_key = glyph * 16 + outline;
    auto cached = variants_->find(variant_key);
    if (cached != variants_->end()) return *cached->second;
    auto variant = std::make_shared<Font>(*this);
    if (renderer_ != nullptr) {
        variant->image_ =
                pckimage::FromPixels(*renderer_, *px_, palette_, {{15, glyph}, {0, outline}},
                                     key_ >= 0 ? std::set<int>{key_} : std::set<int>{});
    }
    (*variants_)[variant_key] = variant;
    return *variant;
}
