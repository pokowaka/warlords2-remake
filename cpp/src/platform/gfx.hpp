// LÖVE-style 2D immediate-mode drawing helpers over SDL_Renderer.
//
// As in LÖVE:
//   - Push/Pop save the transform stack only (translation and scale); the
//     scissor rectangle is independent state specified in the active
//     transform's coordinate space.
//   - SetColor tints drawn images as well as filling shapes.
//   - All textures use nearest-neighbour sampling and snap to integer device
//     pixels so pixel art stays crisp.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SDL_Renderer;
struct SDL_Texture;

namespace gfx {

/**
 * @brief Normalized RGBA colour (`0..1` per channel).
 */
struct Color {
    float r = 1;  ///< Red channel (0..1).
    float g = 1;  ///< Green channel (0..1).
    float b = 1;  ///< Blue channel (0..1).
    float a = 1;  ///< Alpha channel (0..1).
};

/**
 * @brief GPU texture handle and its pixel dimensions.
 */
struct Image {
    SDL_Texture* tex = nullptr;  ///< Underlying SDL texture.
    int w = 0;                   ///< Width in pixels.
    int h = 0;                   ///< Height in pixels.
    bool wrap = false;           ///< True when quads larger than the image tile repeatedly.
    Image() = default;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    ~Image();
    /** @brief Returns the image width in pixels. */
    int Width() const { return w; }
    /** @brief Returns the image height in pixels. */
    int Height() const { return h; }
};
using ImageP = std::shared_ptr<Image>;

/**
 * @brief Sub-rectangle within a source `Image`.
 */
struct Quad {
    double x = 0;  ///< Source top-left x in pixels.
    double y = 0;  ///< Source top-left y in pixels.
    double w = 0;  ///< Source width in pixels.
    double h = 0;  ///< Source height in pixels.
};
/** @brief Constructs a `Quad` for the source region `(x, y, width, height)`. */
inline Quad NewQuad(double x, double y, double width, double height) {
    return Quad{x, y, width, height};
}

/** @brief Binds `ren` as the active SDL renderer and configures nearest-neighbour scaling. */
void Init(SDL_Renderer* ren);
/** @brief Detaches the active renderer before it is destroyed so remaining `Image` destructors
 * no-op. */
void Shutdown();
/** @brief Returns the active `SDL_Renderer*`. */
SDL_Renderer* Renderer();

/** @brief Creates an `Image` from `width * height` RGBA bytes. */
ImageP NewImageRgba(int width, int height, const uint8_t* rgba);

/** @brief Resets transform, scissor, and colour state at the start of a frame. */
void Begin();
/** @brief Clears the render target to `(red, green, blue)`. */
void Clear(float red = 0, float green = 0, float blue = 0);

/** @brief Pushes the current 2D transform onto the transform stack. */
void Push();
/** @brief Restores the most recently pushed 2D transform. */
void Pop();
/** @brief Resets the current 2D transform to the identity transform. */
void Origin();
/** @brief Translates the current 2D transform by `(x, y)`. */
void Translate(double x, double y);
/** @brief Uniformly scales the current 2D transform by `factor`. */
void Scale(double factor);
/** @brief Scales the current 2D transform by `(scale_x, scale_y)`. */
void Scale(double scale_x, double scale_y);
/** @brief Maps `(x, y)` through the current transform into device coordinates `(out_x, out_y)`. */
void TransformPoint(double x, double y, double& out_x, double& out_y);
/** @brief Maps device coordinates `(x, y)` back through the inverse transform into `(out_x,
 * out_y)`. */
void InverseTransformPoint(double x, double y, double& out_x, double& out_y);

/** @brief Sets the active drawing/modulation colour from RGBA components (`0..1`). */
void SetColor(float red, float green, float blue, float alpha = 1);
/** @brief Sets the active drawing/modulation colour from `col`. */
void SetColor(const Color& col);
/** @brief Returns the active drawing/modulation colour. */
Color GetColor();

/** @brief Clips subsequent drawing to `(x, y, width, height)` in the current transform's
 * coordinates. */
void SetScissor(double x, double y, double width, double height);
/** @brief Disables scissor clipping. */
void SetScissor();

/** @brief Shape drawing mode (filled interior or 1-pixel outline). */
enum Mode { kFill, kLine };
/** @brief Draws a filled or outlined rectangle at `(x, y, width, height)`. */
void Rectangle(Mode mode, double x, double y, double width, double height);
/** @brief Draws a 1-pixel line from `(from_x, from_y)` to `(to_x, to_y)`. */
void Line(double from_x, double from_y, double to_x, double to_y);

/** @brief Draws `img` at `(x, y)`. */
void Draw(const Image* img, double x, double y);
/** @brief Draws sub-rectangle `quad` of `img` at `(x, y)`. */
void Draw(const Image* img, const Quad& quad, double x, double y);
/** @brief Draws `img` at `(x, y)`. */
inline void Draw(const ImageP& img, double x, double y) {
    Draw(img.get(), x, y);
}
/** @brief Draws sub-rectangle `quad` of `img` at `(x, y)`. */
inline void Draw(const ImageP& img, const Quad& quad, double x, double y) {
    Draw(img.get(), quad, x, y);
}

/** @brief Reads the current render target back as RGBA rows, setting `width` and `height`. */
std::vector<uint8_t> ReadPixels(int& width, int& height);
/** @brief Writes the current render target to `file` as a `.bmp` image. */
void SaveShot(const std::string& file);

}  // namespace gfx
