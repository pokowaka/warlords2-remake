#include "front/font.hpp"

#include <cmath>
#include <stdexcept>

#include "front/pckimage.hpp"
#include "util/util.hpp"

std::shared_ptr<Font> Font::Load(const std::string& data_dir, const std::string& name,
                                 const w2::pal::Palette& palette, int key_index) {
    auto f = std::make_shared<Font>();
    auto s = w2::ReadFile(data_dir + "/" + name + ".FIN");
    if (!s) throw std::runtime_error("cannot open font metrics: " + name);
    auto m = std::make_shared<Metrics>();
    auto b = [&](size_t i) {
        return i < s->size() ? static_cast<int>(static_cast<uint8_t>((*s)[i])) : 0;
    };
    m->count = b(0);
    m->first = b(1);
    m->sheet_width = b(2) * 256 + b(3);  // big-endian, unlike everything else
    f->line_height = b(4);
    f->baseline = b(5);
    // how wide each glyph's art is, then how far the pen moves on (78a8:03ea)
    for (int i = 0; i < m->count; i++) {
        int c = (m->first + i) & 255;
        m->ink[c] = b(11 + i);
        m->width[c] = b(11 + m->count + i);
    }
    f->m_ = m;
    f->px_ = w2::pck::Decode(data_dir + "/" + name + ".FNT");
    f->palette_ = palette;
    f->key_ = key_index;
    f->image_ = pckimage::FromPixels(*f->px_, palette, {},
                                     key_index >= 0 ? std::set<int>{key_index} : std::set<int>{});
    f->quad_ = std::make_shared<std::map<int, gfx::Quad>>();
    int x = 0, row = 0;
    for (int i = 0; i <= m->count - 2; i++) {  // the last entry has no glyph
        int c = (m->first + 1 + i) & 255;
        int gw = m->ink[c];
        int slot = (gw + 7) / 8 * 8;
        if (x + slot > m->sheet_width) {
            x = 0;
            row++;
        }
        if (gw > 0) (*f->quad_)[c] = gfx::NewQuad(x, row * f->line_height, gw, f->line_height);
        x += slot;
    }
    f->variants_ = std::make_shared<std::map<int, std::shared_ptr<Font>>>();
    return f;
}

int Font::Width(const std::string& text) const {
    int n = 0;
    for (unsigned char c : text) n += m_->width[c];
    return n;
}

int Font::Draw(const std::string& text, double x0, double y0) const {
    double cx = x0;
    for (unsigned char c : text) {
        auto q = quad_->find(c);
        if (q != quad_->end()) gfx::Draw(image_, q->second, cx, y0);
        cx += m_->width[c];
    }
    return static_cast<int>(cx - x0);
}

void Font::DrawCentred(const std::string& text, double rx, double ry, double rw, double rh) const {
    Draw(text, rx + std::floor((rw - Width(text)) / 2),
         ry + std::floor((rh - line_height) / 2) + 1);
}

const Font& Font::Colours(int glyph, int outline) const {
    if (glyph == 15 && outline == 0) return *this;
    int k = glyph * 16 + outline;
    auto it = variants_->find(k);
    if (it != variants_->end()) return *it->second;
    auto v = std::make_shared<Font>(*this);
    v->image_ = pckimage::FromPixels(*px_, palette_, {{15, glyph}, {0, outline}},
                                     key_ >= 0 ? std::set<int>{key_} : std::set<int>{});
    (*variants_)[k] = v;
    return *v;
}
