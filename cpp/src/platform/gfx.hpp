/**
 * @file
 * @brief Immediate-mode 2D renderer (`gfx::Renderer`) and texture handles (`gfx::Image`)
 * over `SDL_Renderer`.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

struct SDL_Renderer;
struct SDL_Texture;

namespace gfx {

/// Shared slot pointing to the active `SDL_Renderer*` while a `Renderer` is live, or `nullptr`.
using SdlRendererSlot = std::shared_ptr<SDL_Renderer*>;

/// Weak observer handle held by `Image` so texture destruction no-ops after `Renderer::Shutdown()`.
using SdlRendererWeakRef = std::weak_ptr<SDL_Renderer*>;

/// Byte buffer returned when reading RGBA pixels back from a render target.
using ByteBuffer = std::vector<uint8_t>;

/**
 * @brief Normalized RGBA colour (`0..1` per channel).
 */
struct Color {
    float r = 1.0F;  ///< Red channel (`0..1`).
    float g = 1.0F;  ///< Green channel (`0..1`).
    float b = 1.0F;  ///< Blue channel (`0..1`).
    float a = 1.0F;  ///< Alpha channel (`0..1`).
};

/**
 * @brief GPU texture handle and its pixel dimensions.
 *
 * Holds a `SdlRendererWeakRef` back to the `Renderer` that created it; if the
 * owning `Renderer` has already shut down or been destroyed, `~Image()` leaves
 * `tex` untouched because SDL frees textures alongside their renderer.
 */
struct Image {
    SDL_Texture* tex = nullptr;       ///< Underlying SDL texture handle.
    int w = 0;                        ///< Width in pixels.
    int h = 0;                        ///< Height in pixels.
    bool wrap = false;                ///< True when quads larger than the image tile repeatedly.
    SdlRendererWeakRef renderer_ref;  ///< Weak handle to the owning `Renderer`'s live slot.

    Image() = default;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    ~Image();

    /** @brief Returns the image width in pixels. */
    int Width() const { return w; }
    /** @brief Returns the image height in pixels. */
    int Height() const { return h; }
};

/// Shared ownership handle for a decoded GPU `Image`.
using ImageP = std::shared_ptr<Image>;

/**
 * @brief Sub-rectangle within a source `Image`.
 */
struct Quad {
    double x = 0.0;  ///< Source top-left x in pixels.
    double y = 0.0;  ///< Source top-left y in pixels.
    double w = 0.0;  ///< Source width in pixels.
    double h = 0.0;  ///< Source height in pixels.
};

/** @brief Constructs a `Quad` for the source region `(x, y, width, height)`. */
inline Quad NewQuad(double x, double y, double width, double height) {
    return Quad{x, y, width, height};
}

/** @brief Shape drawing mode (filled interior or 1-pixel outline). */
enum Mode { kFill, kLine };

/**
 * @brief Encapsulated LÖVE-style 2D immediate-mode renderer over `SDL_Renderer`.
 *
 * Manages a 2D scale-and-translate transform stack (`Push`/`Pop`/`Origin`/
 * `Translate`/`Scale`), independent scissor clipping in the active transform's
 * coordinate space, active RGBA draw/modulation colour, nearest-neighbour
 * pixel-art texture creation, and target pixel readback.
 *
 * Invariants:
 * - `cur_transform_.sx != 0` and `cur_transform_.sy != 0` for valid scales.
 * - `*alive_slot_ == sdl_renderer_` while active, and `*alive_slot_ == nullptr`
 *   after `Shutdown()` or destruction so surviving `Image` handles never
 *   invoke `SDL_DestroyTexture` on a torn-down renderer.
 *
 * Not thread-safe; called exclusively on the main rendering thread.
 */
class Renderer {
  public:
    /** @brief Constructs a detached renderer with identity transform and white modulation. */
    Renderer();

    /**
     * @brief Constructs a renderer bound to `sdl_renderer`.
     * @param sdl_renderer Underlying SDL renderer handle.
     * @param take_ownership When true, `~Renderer()`/`Shutdown()` calls `SDL_DestroyRenderer`.
     */
    explicit Renderer(SDL_Renderer* sdl_renderer, bool take_ownership = false);

    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /**
     * @brief Binds `sdl_renderer` and configures nearest-neighbour texture sampling.
     * @param sdl_renderer Underlying SDL renderer handle.
     * @param take_ownership When true, `Shutdown()` destroys `sdl_renderer`.
     */
    void Init(SDL_Renderer* sdl_renderer, bool take_ownership = false);

    /** @brief Detaches (and if owned, destroys) `sdl_renderer_` and invalidates live image slots.
     */
    void Shutdown();

    /** @brief Returns the bound `SDL_Renderer*`, or `nullptr` if detached. */
    SDL_Renderer* SdlRenderer() const { return sdl_renderer_; }

    /** @brief Creates a nearest-filtered `Image` from `width * height` RGBA bytes. */
    ImageP NewImageRgba(int width, int height, const uint8_t* rgba);

    /** @brief Resets scissor, transform stack, and colour state at the start of a frame. */
    void Begin();

    /** @brief Clears the current render target to `(red, green, blue)`. */
    void Clear(float red = 0.0F, float green = 0.0F, float blue = 0.0F);

    /** @brief Pushes the current 2D transform onto the transform stack. */
    void Push();
    /** @brief Restores the most recently pushed 2D transform (or identity if empty). */
    void Pop();
    /** @brief Resets the current 2D transform to the identity transform. */
    void Origin();
    /** @brief Translates the current 2D transform by `(x, y)`. */
    void Translate(double x, double y);
    /** @brief Uniformly scales the current 2D transform by `factor`. */
    void Scale(double factor);
    /** @brief Scales the current 2D transform by `(scale_x, scale_y)`. */
    void Scale(double scale_x, double scale_y);

    /** @brief Maps `(x, y)` through the current transform into device coordinates. */
    void TransformPoint(double x, double y, double& out_x, double& out_y) const;
    /** @brief Maps device coordinates `(x, y)` back through the inverse transform. */
    void InverseTransformPoint(double x, double y, double& out_x, double& out_y) const;

    /** @brief Sets the active drawing/modulation colour from RGBA components (`0..1`). */
    void SetColor(float red, float green, float blue, float alpha = 1.0F);
    /** @brief Sets the active drawing/modulation colour from `col`. */
    void SetColor(const Color& col);
    /** @brief Returns the active drawing/modulation colour. */
    Color GetColor() const { return color_; }

    /** @brief Clips subsequent drawing to `(x, y, width, height)` in current coordinates. */
    void SetScissor(double x, double y, double width, double height);
    /** @brief Disables scissor clipping. */
    void SetScissor();

    /** @brief Draws a filled or outlined rectangle at `(x, y, width, height)`. */
    void Rectangle(Mode mode, double x, double y, double width, double height);
    /** @brief Draws a 1-pixel line from `(from_x, from_y)` to `(to_x, to_y)`. */
    void Line(double from_x, double from_y, double to_x, double to_y);

    /** @brief Draws `img` at `(x, y)`. */
    void Draw(const Image* img, double x, double y);
    /** @brief Draws sub-rectangle `quad` of `img` at `(x, y)`. */
    void Draw(const Image* img, const Quad& quad, double x, double y);
    /** @brief Draws `img` at `(x, y)`. */
    void Draw(const ImageP& img, double x, double y) { Draw(img.get(), x, y); }
    /** @brief Draws sub-rectangle `quad` of `img` at `(x, y)`. */
    void Draw(const ImageP& img, const Quad& quad, double x, double y) {
        Draw(img.get(), quad, x, y);
    }

    /** @brief Reads the current render target back as RGBA rows, setting `width` and `height`. */
    ByteBuffer ReadPixels(int& width, int& height);
    /** @brief Writes the current render target to `file_path` as a `.bmp` image. */
    void SaveShot(const std::filesystem::path& file_path);

  private:
    struct Transform {
        double sx = 1.0;  ///< Horizontal scale factor.
        double sy = 1.0;  ///< Vertical scale factor.
        double tx = 0.0;  ///< Horizontal translation in device pixels.
        double ty = 0.0;  ///< Vertical translation in device pixels.
    };
    using TransformStack = std::vector<Transform>;

    void ApplyColor(SDL_Texture* tex) const;
    void SetDrawColor() const;
    void MapRectToDevice(double x, double y, double width, double height, int& out_x, int& out_y,
                         int& out_w, int& out_h) const;
    void Blit(const Image* img, int src_x, int src_y, int src_w, int src_h, double dx,
              double dy) const;

    SDL_Renderer* sdl_renderer_ = nullptr;  ///< Bound SDL renderer handle.
    bool owns_sdl_renderer_ = false;        ///< True when `Shutdown()` destroys `sdl_renderer_`.
    SdlRendererSlot alive_slot_;            ///< Live renderer slot observed by `Image` weak refs.
    Color color_{};                         ///< Active fill and texture modulation colour.
    Transform cur_transform_{};             ///< Active 2D scale-and-translate transform.
    TransformStack transform_stack_;        ///< Stack of saved transforms from `Push()`.
    bool clipped_ = false;                  ///< True while a scissor clip rect is active.
    int clip_x_ = 0;                        ///< Scissor clip left coordinate in device pixels.
    int clip_y_ = 0;                        ///< Scissor clip top coordinate in device pixels.
    int clip_w_ = 0;                        ///< Scissor clip width in device pixels.
    int clip_h_ = 0;                        ///< Scissor clip height in device pixels.
};

/** @brief Returns the process-default `Renderer` instance. */
Renderer& DefaultRenderer();
/** @brief Binds `sdl_renderer` on the default `Renderer`. */
void Init(SDL_Renderer* sdl_renderer);
/** @brief Detaches the default `Renderer`. */
void Shutdown();
/** @brief Returns the active `SDL_Renderer*` on the default `Renderer`. */
SDL_Renderer* ActiveSdlRenderer();
/** @brief Creates an `Image` from RGBA bytes on the default `Renderer`. */
ImageP NewImageRgba(int width, int height, const uint8_t* rgba);
/** @brief Resets frame state on the default `Renderer`. */
void Begin();
/** @brief Clears the render target on the default `Renderer`. */
void Clear(float red = 0.0F, float green = 0.0F, float blue = 0.0F);
/** @brief Pushes the transform on the default `Renderer`. */
void Push();
/** @brief Pops the transform on the default `Renderer`. */
void Pop();
/** @brief Resets the transform on the default `Renderer`. */
void Origin();
/** @brief Translates the transform on the default `Renderer`. */
void Translate(double x, double y);
/** @brief Uniformly scales the transform on the default `Renderer`. */
void Scale(double factor);
/** @brief Scales the transform by `(scale_x, scale_y)` on the default `Renderer`. */
void Scale(double scale_x, double scale_y);
/** @brief Transforms `(x, y)` into device coordinates on the default `Renderer`. */
void TransformPoint(double x, double y, double& out_x, double& out_y);
/** @brief Inverse-transforms `(x, y)` from device coordinates on the default `Renderer`. */
void InverseTransformPoint(double x, double y, double& out_x, double& out_y);
/** @brief Sets the active colour on the default `Renderer`. */
void SetColor(float red, float green, float blue, float alpha = 1.0F);
/** @brief Sets the active colour on the default `Renderer`. */
void SetColor(const Color& col);
/** @brief Gets the active colour on the default `Renderer`. */
Color GetColor();
/** @brief Sets the scissor clip rectangle on the default `Renderer`. */
void SetScissor(double x, double y, double width, double height);
/** @brief Clears the scissor clip rectangle on the default `Renderer`. */
void SetScissor();
/** @brief Draws a rectangle on the default `Renderer`. */
void Rectangle(Mode mode, double x, double y, double width, double height);
/** @brief Draws a line on the default `Renderer`. */
void Line(double from_x, double from_y, double to_x, double to_y);
/** @brief Draws `img` at `(x, y)` on the default `Renderer`. */
void Draw(const Image* img, double x, double y);
/** @brief Draws `quad` of `img` at `(x, y)` on the default `Renderer`. */
void Draw(const Image* img, const Quad& quad, double x, double y);
/** @brief Draws `img` at `(x, y)` on the default `Renderer`. */
inline void Draw(const ImageP& img, double x, double y) {
    Draw(img.get(), x, y);
}
/** @brief Draws `quad` of `img` at `(x, y)` on the default `Renderer`. */
inline void Draw(const ImageP& img, const Quad& quad, double x, double y) {
    Draw(img.get(), quad, x, y);
}
/** @brief Reads RGBA pixels from the default `Renderer`. */
ByteBuffer ReadPixels(int& width, int& height);
/** @brief Writes a `.bmp` screenshot from the default `Renderer`. */
void SaveShot(const std::filesystem::path& file_path);

}  // namespace gfx

namespace w2 {
using gfx::Renderer;
}  // namespace w2
