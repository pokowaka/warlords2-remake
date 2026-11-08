#include "platform/gfx.hpp"

#include <algorithm>
#include <cmath>

#include <SDL.h>

namespace gfx {

namespace {
SDL_Renderer* active_renderer = nullptr;
Color color;
/**
 * @brief 2D scale-and-translate transform (`x' = sx * x + tx` in device pixels).
 */
struct Transform {
    double sx = 1;  ///< Horizontal scale factor.
    double sy = 1;  ///< Vertical scale factor.
    double tx = 0;  ///< Horizontal translation in device pixels.
    double ty = 0;  ///< Vertical translation in device pixels.
};
Transform cur_transform;
std::vector<Transform> stack;
bool clipped = false;
SDL_Rect clip{};

/** @brief Rounds `val` to the nearest integer device pixel. */
inline int Dev(double val) {
    return static_cast<int>(std::floor(val + 0.5));
}

/** @brief Applies the active colour and alpha modulation to `tex`. */
void ApplyColor(SDL_Texture* tex) {
    auto to_u8 = [](float val) {
        return static_cast<Uint8>(std::clamp(static_cast<int>(std::lround(val * 255)), 0, 255));
    };
    SDL_SetTextureColorMod(tex, to_u8(color.r), to_u8(color.g), to_u8(color.b));
    SDL_SetTextureAlphaMod(tex, to_u8(color.a));
}

/** @brief Applies the active draw colour and blend mode to `active_renderer`. */
void SetDrawColor() {
    auto to_u8 = [](float val) {
        return static_cast<Uint8>(std::clamp(static_cast<int>(std::lround(val * 255)), 0, 255));
    };
    SDL_SetRenderDrawColor(active_renderer, to_u8(color.r), to_u8(color.g), to_u8(color.b),
                           to_u8(color.a));
    SDL_SetRenderDrawBlendMode(active_renderer,
                               color.a < 1 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
}

/** @brief Maps `(x, y, width, height)` through `cur_transform` onto integer device pixels. */
SDL_Rect DevRect(double x, double y, double width, double height) {
    double left_x = cur_transform.sx * x + cur_transform.tx;
    double top_y = cur_transform.sy * y + cur_transform.ty;
    double right_x = cur_transform.sx * (x + width) + cur_transform.tx;
    double bot_y = cur_transform.sy * (y + height) + cur_transform.ty;
    int ix1 = Dev(std::min(left_x, right_x)), iy1 = Dev(std::min(top_y, bot_y));
    int ix2 = Dev(std::max(left_x, right_x)), iy2 = Dev(std::max(top_y, bot_y));
    return SDL_Rect{ix1, iy1, ix2 - ix1, iy2 - iy1};
}

/** @brief Copies sub-rectangle `(src_x, src_y, src_w, src_h)` of `img` to `(dx, dy)`. */
void Blit(const Image* img, int src_x, int src_y, int src_w, int src_h, double dx, double dy) {
    if (src_w <= 0 || src_h <= 0) return;
    SDL_Rect src{src_x, src_y, src_w, src_h};
    SDL_Rect dst = DevRect(dx, dy, src_w, src_h);
    if (dst.w <= 0 || dst.h <= 0) return;
    SDL_RenderCopy(active_renderer, img->tex, &src, &dst);
}
}  // namespace

// Once the renderer is shut down its textures are already freed; any Image
// destroyed afterwards (such as a global at process exit) leaves `tex` alone.
Image::~Image() {
    if (tex && active_renderer) SDL_DestroyTexture(tex);
}

void Init(SDL_Renderer* ren) {
    active_renderer = ren;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
}

void Shutdown() {
    active_renderer = nullptr;
}

SDL_Renderer* Renderer() {
    return active_renderer;
}

ImageP NewImageRgba(int width, int height, const uint8_t* rgba) {
    auto img = std::make_shared<Image>();
    img->w = width;
    img->h = height;
    if (width <= 0 || height <= 0 || !active_renderer) return img;
    img->tex = SDL_CreateTexture(active_renderer, SDL_PIXELFORMAT_ABGR8888,
                                 SDL_TEXTUREACCESS_STATIC, width, height);
    if (img->tex) {
        SDL_UpdateTexture(img->tex, nullptr, rgba, width * 4);
        SDL_SetTextureBlendMode(img->tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(img->tex, SDL_ScaleModeNearest);
    }
    return img;
}

void Begin() {
    SetScissor();
    cur_transform = Transform{};
    stack.clear();
    SetColor(1, 1, 1);
}

void Clear(float red, float green, float blue) {
    SDL_RenderSetClipRect(active_renderer, nullptr);
    SDL_SetRenderDrawBlendMode(active_renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(active_renderer, static_cast<Uint8>(red * 255),
                           static_cast<Uint8>(green * 255), static_cast<Uint8>(blue * 255), 255);
    SDL_RenderClear(active_renderer);
    if (clipped) SDL_RenderSetClipRect(active_renderer, &clip);
}

void Push() {
    stack.push_back(cur_transform);
}
void Pop() {
    if (stack.empty()) {
        cur_transform = Transform{};
        return;
    }
    cur_transform = stack.back();
    stack.pop_back();
}
void Origin() {
    cur_transform = Transform{};
}
void Translate(double x, double y) {
    cur_transform.tx += cur_transform.sx * x;
    cur_transform.ty += cur_transform.sy * y;
}
void Scale(double factor) {
    Scale(factor, factor);
}
void Scale(double scale_x, double scale_y) {
    cur_transform.sx *= scale_x;
    cur_transform.sy *= scale_y;
}
void TransformPoint(double x, double y, double& out_x, double& out_y) {
    out_x = cur_transform.sx * x + cur_transform.tx;
    out_y = cur_transform.sy * y + cur_transform.ty;
}
void InverseTransformPoint(double x, double y, double& out_x, double& out_y) {
    out_x = (x - cur_transform.tx) / cur_transform.sx;
    out_y = (y - cur_transform.ty) / cur_transform.sy;
}

void SetColor(float red, float green, float blue, float alpha) {
    color = Color{red, green, blue, alpha};
}
void SetColor(const Color& col) {
    color = col;
}
Color GetColor() {
    return color;
}

void SetScissor(double x, double y, double width, double height) {
    clip = DevRect(x, y, width, height);
    clipped = true;
    SDL_RenderSetClipRect(active_renderer, &clip);
}
void SetScissor() {
    clipped = false;
    if (active_renderer) SDL_RenderSetClipRect(active_renderer, nullptr);
}

void Rectangle(Mode mode, double x, double y, double width, double height) {
    SetDrawColor();
    if (mode == kFill) {
        SDL_Rect rect = DevRect(x, y, width, height);
        if (rect.w > 0 && rect.h > 0) SDL_RenderFillRect(active_renderer, &rect);
        return;
    }
    SDL_Rect edges[4] = {
        DevRect(x - 0.5, y - 0.5, width + 1, 1), DevRect(x - 0.5, y + height - 0.5, width + 1, 1),
        DevRect(x - 0.5, y - 0.5, 1, height + 1), DevRect(x + width - 0.5, y - 0.5, 1, height + 1)};
    for (auto& edge : edges)
        if (edge.w > 0 && edge.h > 0) SDL_RenderFillRect(active_renderer, &edge);
}

void Line(double from_x, double from_y, double to_x, double to_y) {
    if (from_y == to_y) {
        Rectangle(kFill, std::min(from_x, to_x), from_y - 0.5, std::fabs(to_x - from_x), 1);
    } else if (from_x == to_x) {
        Rectangle(kFill, from_x - 0.5, std::min(from_y, to_y), 1, std::fabs(to_y - from_y));
    } else {
        SetDrawColor();
        double dev_x1, dev_y1, dev_x2, dev_y2;
        TransformPoint(from_x, from_y, dev_x1, dev_y1);
        TransformPoint(to_x, to_y, dev_x2, dev_y2);
        SDL_RenderDrawLine(active_renderer, Dev(dev_x1), Dev(dev_y1), Dev(dev_x2), Dev(dev_y2));
    }
}

void Draw(const Image* img, double x, double y) {
    if (!img || !img->tex) return;
    ApplyColor(img->tex);
    Blit(img, 0, 0, img->w, img->h, x, y);
}

void Draw(const Image* img, const Quad& quad, double x, double y) {
    if (!img || !img->tex) return;
    ApplyColor(img->tex);
    int quad_x = static_cast<int>(quad.x), quad_y = static_cast<int>(quad.y),
        quad_w = static_cast<int>(quad.w), quad_h = static_cast<int>(quad.h);
    if (img->wrap && (quad_x + quad_w > img->w || quad_y + quad_h > img->h)) {
        // Tile repeating images when the quad extends past the texture bounds.
        for (int tile_y = 0; tile_y < quad_h; tile_y += img->h) {
            for (int tile_x = 0; tile_x < quad_w; tile_x += img->w) {
                int part_w = std::min(img->w, quad_w - tile_x);
                int part_h = std::min(img->h, quad_h - tile_y);
                Blit(img, 0, 0, part_w, part_h, x + tile_x, y + tile_y);
            }
        }
        return;
    }
    // Clip the source quad to the image bounds.
    int src_x = std::max(0, quad_x), src_y = std::max(0, quad_y);
    int src_w = std::min(img->w, quad_x + quad_w) - src_x;
    int src_h = std::min(img->h, quad_y + quad_h) - src_y;
    Blit(img, src_x, src_y, src_w, src_h, x + (src_x - quad_x), y + (src_y - quad_y));
}

std::vector<uint8_t> ReadPixels(int& width, int& height) {
    if (SDL_Texture* target = SDL_GetRenderTarget(active_renderer))
        SDL_QueryTexture(target, nullptr, nullptr, &width, &height);
    else
        SDL_GetRendererOutputSize(active_renderer, &width, &height);
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    SDL_RenderSetClipRect(active_renderer, nullptr);
    SDL_RenderReadPixels(active_renderer, nullptr, SDL_PIXELFORMAT_ABGR8888, pixels.data(),
                         width * 4);
    if (clipped) SDL_RenderSetClipRect(active_renderer, &clip);
    return pixels;
}

void SaveShot(const std::string& file) {
    int width, height;
    auto pixels = ReadPixels(width, height);
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), width, height, 32,
                                                           width * 4, SDL_PIXELFORMAT_ABGR8888);
    if (surf) {
        SDL_SaveBMP(surf, file.c_str());
        SDL_FreeSurface(surf);
    }
}

}  // namespace gfx
