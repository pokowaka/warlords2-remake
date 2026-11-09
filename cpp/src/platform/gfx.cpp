#include "platform/gfx.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <SDL.h>

namespace gfx {

namespace {

/** @brief Rounds `val` to the nearest integer device pixel. */
int RoundDevicePx(double val) {
    return static_cast<int>(std::floor(val + 0.5));
}

/** @brief Clamps normalized float channel `val` (`0..1`) to an 8-bit SDL colour component. */
Uint8 ChannelToU8(float val) {
    return static_cast<Uint8>(std::clamp(static_cast<int>(std::lround(val * 255.0F)), 0, 255));
}

}  // namespace

Image::~Image() {
    if (tex == nullptr) return;
    if (auto slot = renderer_ref.lock(); slot && *slot != nullptr) {
        SDL_DestroyTexture(tex);
    }
}

Renderer::Renderer() : alive_slot_(std::make_shared<SDL_Renderer*>(nullptr)) {}

Renderer::Renderer(SDL_Renderer* sdl_renderer, bool take_ownership) : Renderer() {
    Init(sdl_renderer, take_ownership);
}

Renderer::~Renderer() {
    Shutdown();
}

void Renderer::Init(SDL_Renderer* sdl_renderer, bool take_ownership) {
    Shutdown();
    sdl_renderer_ = sdl_renderer;
    owns_sdl_renderer_ = take_ownership;
    alive_slot_ = std::make_shared<SDL_Renderer*>(sdl_renderer_);
    if (sdl_renderer_ != nullptr) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    }
}

void Renderer::Shutdown() {
    if (alive_slot_) {
        *alive_slot_ = nullptr;
    }
    if (owns_sdl_renderer_ && sdl_renderer_ != nullptr) {
        SDL_DestroyRenderer(sdl_renderer_);
    }
    sdl_renderer_ = nullptr;
    owns_sdl_renderer_ = false;
    clipped_ = false;
}

ImageP Renderer::NewImageRgba(int width, int height, const uint8_t* rgba) {
    auto img = std::make_shared<Image>();
    img->w = width;
    img->h = height;
    img->renderer_ref = alive_slot_;
    if (width <= 0 || height <= 0 || sdl_renderer_ == nullptr || rgba == nullptr) return img;
    img->tex = SDL_CreateTexture(sdl_renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC,
                                 width, height);
    if (img->tex != nullptr) {
        SDL_UpdateTexture(img->tex, nullptr, rgba, width * 4);
        SDL_SetTextureBlendMode(img->tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(img->tex, SDL_ScaleModeNearest);
    }
    return img;
}

void Renderer::Begin() {
    SetScissor();
    cur_transform_ = Transform{};
    transform_stack_.clear();
    SetColor(1.0F, 1.0F, 1.0F, 1.0F);
}

void Renderer::Clear(float red, float green, float blue) {
    if (sdl_renderer_ == nullptr) return;
    SDL_RenderSetClipRect(sdl_renderer_, nullptr);
    SDL_SetRenderDrawBlendMode(sdl_renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(sdl_renderer_, static_cast<Uint8>(red * 255.0F),
                           static_cast<Uint8>(green * 255.0F), static_cast<Uint8>(blue * 255.0F),
                           255);
    SDL_RenderClear(sdl_renderer_);
    if (clipped_) {
        SDL_Rect clip_rect{clip_x_, clip_y_, clip_w_, clip_h_};
        SDL_RenderSetClipRect(sdl_renderer_, &clip_rect);
    }
}

void Renderer::Push() {
    transform_stack_.push_back(cur_transform_);
}

void Renderer::Pop() {
    if (transform_stack_.empty()) {
        cur_transform_ = Transform{};
        return;
    }
    cur_transform_ = transform_stack_.back();
    transform_stack_.pop_back();
}

void Renderer::Origin() {
    cur_transform_ = Transform{};
}

void Renderer::Translate(double x, double y) {
    cur_transform_.tx += cur_transform_.sx * x;
    cur_transform_.ty += cur_transform_.sy * y;
}

void Renderer::Scale(double factor) {
    Scale(factor, factor);
}

void Renderer::Scale(double scale_x, double scale_y) {
    cur_transform_.sx *= scale_x;
    cur_transform_.sy *= scale_y;
}

void Renderer::TransformPoint(double x, double y, double& out_x, double& out_y) const {
    out_x = cur_transform_.sx * x + cur_transform_.tx;
    out_y = cur_transform_.sy * y + cur_transform_.ty;
}

void Renderer::InverseTransformPoint(double x, double y, double& out_x, double& out_y) const {
    out_x = (x - cur_transform_.tx) / cur_transform_.sx;
    out_y = (y - cur_transform_.ty) / cur_transform_.sy;
}

void Renderer::SetColor(float red, float green, float blue, float alpha) {
    color_ = Color{red, green, blue, alpha};
}

void Renderer::SetColor(const Color& col) {
    color_ = col;
}

void Renderer::SetScissor(double x, double y, double width, double height) {
    MapRectToDevice(x, y, width, height, clip_x_, clip_y_, clip_w_, clip_h_);
    clipped_ = true;
    if (sdl_renderer_ != nullptr) {
        SDL_Rect clip_rect{clip_x_, clip_y_, clip_w_, clip_h_};
        SDL_RenderSetClipRect(sdl_renderer_, &clip_rect);
    }
}

void Renderer::SetScissor() {
    clipped_ = false;
    if (sdl_renderer_ != nullptr) {
        SDL_RenderSetClipRect(sdl_renderer_, nullptr);
    }
}

void Renderer::ApplyColor(SDL_Texture* tex) const {
    SDL_SetTextureColorMod(tex, ChannelToU8(color_.r), ChannelToU8(color_.g),
                           ChannelToU8(color_.b));
    SDL_SetTextureAlphaMod(tex, ChannelToU8(color_.a));
}

void Renderer::SetDrawColor() const {
    if (sdl_renderer_ == nullptr) return;
    SDL_SetRenderDrawColor(sdl_renderer_, ChannelToU8(color_.r), ChannelToU8(color_.g),
                           ChannelToU8(color_.b), ChannelToU8(color_.a));
    SDL_SetRenderDrawBlendMode(sdl_renderer_,
                               color_.a < 1.0F ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
}

void Renderer::MapRectToDevice(double x, double y, double width, double height, int& out_x,
                               int& out_y, int& out_w, int& out_h) const {
    double left_x = cur_transform_.sx * x + cur_transform_.tx;
    double top_y = cur_transform_.sy * y + cur_transform_.ty;
    double right_x = cur_transform_.sx * (x + width) + cur_transform_.tx;
    double bot_y = cur_transform_.sy * (y + height) + cur_transform_.ty;
    int ix1 = RoundDevicePx(std::min(left_x, right_x));
    int iy1 = RoundDevicePx(std::min(top_y, bot_y));
    int ix2 = RoundDevicePx(std::max(left_x, right_x));
    int iy2 = RoundDevicePx(std::max(top_y, bot_y));
    out_x = ix1;
    out_y = iy1;
    out_w = ix2 - ix1;
    out_h = iy2 - iy1;
}

void Renderer::Blit(const Image* img, int src_x, int src_y, int src_w, int src_h, double dx,
                    double dy) const {
    if (sdl_renderer_ == nullptr || src_w <= 0 || src_h <= 0) return;
    SDL_Rect src{src_x, src_y, src_w, src_h};
    SDL_Rect dst{};
    MapRectToDevice(dx, dy, src_w, src_h, dst.x, dst.y, dst.w, dst.h);
    if (dst.w <= 0 || dst.h <= 0) return;
    SDL_RenderCopy(sdl_renderer_, img->tex, &src, &dst);
}

void Renderer::Rectangle(Mode mode, double x, double y, double width, double height) {
    if (sdl_renderer_ == nullptr) return;
    SetDrawColor();
    if (mode == kFill) {
        SDL_Rect rect{};
        MapRectToDevice(x, y, width, height, rect.x, rect.y, rect.w, rect.h);
        if (rect.w > 0 && rect.h > 0) SDL_RenderFillRect(sdl_renderer_, &rect);
        return;
    }
    SDL_Rect edges[4]{};
    MapRectToDevice(x - 0.5, y - 0.5, width + 1.0, 1.0, edges[0].x, edges[0].y, edges[0].w,
                    edges[0].h);
    MapRectToDevice(x - 0.5, y + height - 0.5, width + 1.0, 1.0, edges[1].x, edges[1].y, edges[1].w,
                    edges[1].h);
    MapRectToDevice(x - 0.5, y - 0.5, 1.0, height + 1.0, edges[2].x, edges[2].y, edges[2].w,
                    edges[2].h);
    MapRectToDevice(x + width - 0.5, y - 0.5, 1.0, height + 1.0, edges[3].x, edges[3].y, edges[3].w,
                    edges[3].h);
    for (const auto& edge : edges) {
        if (edge.w > 0 && edge.h > 0) SDL_RenderFillRect(sdl_renderer_, &edge);
    }
}

void Renderer::Line(double from_x, double from_y, double to_x, double to_y) {
    if (sdl_renderer_ == nullptr) return;
    if (from_y == to_y) {
        Rectangle(kFill, std::min(from_x, to_x), from_y - 0.5, std::fabs(to_x - from_x), 1.0);
    } else if (from_x == to_x) {
        Rectangle(kFill, from_x - 0.5, std::min(from_y, to_y), 1.0, std::fabs(to_y - from_y));
    } else {
        SetDrawColor();
        double dev_x1 = 0.0;
        double dev_y1 = 0.0;
        double dev_x2 = 0.0;
        double dev_y2 = 0.0;
        TransformPoint(from_x, from_y, dev_x1, dev_y1);
        TransformPoint(to_x, to_y, dev_x2, dev_y2);
        SDL_RenderDrawLine(sdl_renderer_, RoundDevicePx(dev_x1), RoundDevicePx(dev_y1),
                           RoundDevicePx(dev_x2), RoundDevicePx(dev_y2));
    }
}

void Renderer::Draw(const Image* img, double x, double y) {
    if (img == nullptr || img->tex == nullptr || sdl_renderer_ == nullptr) return;
    ApplyColor(img->tex);
    Blit(img, 0, 0, img->w, img->h, x, y);
}

void Renderer::Draw(const Image* img, const Quad& quad, double x, double y) {
    if (img == nullptr || img->tex == nullptr || sdl_renderer_ == nullptr) return;
    ApplyColor(img->tex);
    int quad_x = static_cast<int>(quad.x);
    int quad_y = static_cast<int>(quad.y);
    int quad_w = static_cast<int>(quad.w);
    int quad_h = static_cast<int>(quad.h);
    if (img->wrap && (quad_x + quad_w > img->w || quad_y + quad_h > img->h)) {
        for (int tile_y = 0; tile_y < quad_h; tile_y += img->h) {
            for (int tile_x = 0; tile_x < quad_w; tile_x += img->w) {
                int part_w = std::min(img->w, quad_w - tile_x);
                int part_h = std::min(img->h, quad_h - tile_y);
                Blit(img, 0, 0, part_w, part_h, x + tile_x, y + tile_y);
            }
        }
        return;
    }
    int src_x = std::max(0, quad_x);
    int src_y = std::max(0, quad_y);
    int src_w = std::min(img->w, quad_x + quad_w) - src_x;
    int src_h = std::min(img->h, quad_y + quad_h) - src_y;
    Blit(img, src_x, src_y, src_w, src_h, x + (src_x - quad_x), y + (src_y - quad_y));
}

ByteBuffer Renderer::ReadPixels(int& width, int& height) {
    width = 0;
    height = 0;
    if (sdl_renderer_ == nullptr) return {};
    if (SDL_Texture* target = SDL_GetRenderTarget(sdl_renderer_)) {
        SDL_QueryTexture(target, nullptr, nullptr, &width, &height);
    } else {
        SDL_GetRendererOutputSize(sdl_renderer_, &width, &height);
    }
    if (width <= 0 || height <= 0) return {};
    ByteBuffer pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4UZ);
    SDL_RenderSetClipRect(sdl_renderer_, nullptr);
    SDL_RenderReadPixels(sdl_renderer_, nullptr, SDL_PIXELFORMAT_ABGR8888, pixels.data(),
                         width * 4);
    if (clipped_) {
        SDL_Rect clip_rect{clip_x_, clip_y_, clip_w_, clip_h_};
        SDL_RenderSetClipRect(sdl_renderer_, &clip_rect);
    }
    return pixels;
}

void Renderer::SaveShot(const std::filesystem::path& file_path) {
    int width = 0;
    int height = 0;
    ByteBuffer pixels = ReadPixels(width, height);
    if (pixels.empty()) return;
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), width, height, 32,
                                                           width * 4, SDL_PIXELFORMAT_ABGR8888);
    if (surf != nullptr) {
        std::string path_str = file_path.string();
        SDL_SaveBMP(surf, path_str.c_str());
        SDL_FreeSurface(surf);
    }
}

}  // namespace gfx
