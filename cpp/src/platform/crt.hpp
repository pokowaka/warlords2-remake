/**
 * @file
 * @brief CRT monitor post-processing pass (`crt::CrtShader`): barrel distortion,
 * scanlines, RGB aperture-grille stripes, halation glow, and corner vignetting.
 */
#pragma once

#include <memory>
#include <utility>

struct SDL_Renderer;
struct SDL_Texture;

namespace crt {

/// 2D point `(x, y)` in offscreen canvas device pixels.
using CanvasPoint = std::pair<double, double>;

/**
 * @brief Per-frame display geometry supplied to `CrtShader::Present` and `CrtShader::Unwarp`.
 */
struct FrameGeometry {
    int canvas_w = 640;    ///< Offscreen canvas width in device pixels.
    int canvas_h = 480;    ///< Offscreen canvas height in device pixels.
    int frame_x = 0;       ///< Active UI frame left offset on the canvas in device pixels.
    int frame_y = 0;       ///< Active UI frame top offset on the canvas in device pixels.
    int frame_ui_w = 640;  ///< Active UI frame width in logical UI pixels.
    int frame_ui_h = 480;  ///< Active UI frame height in logical UI pixels.
    int scale = 1;         ///< Device pixels per logical UI pixel.
    double dpi = 1.0;      ///< Device pixels per window point.
    double shrink = 1.0;   ///< Ratio of physical window pixels to offscreen canvas pixels.
};

/**
 * @brief Encapsulated CRT post-processor owning OpenGL shader and auxiliary texture state.
 *
 * Renders an offscreen `SDL_Texture` onto the window through a GLSL 1.20
 * fragment shader when SDL uses its OpenGL backend, falling back automatically
 * to a subdivided `SDL_RenderGeometry` bowed mesh with cached scanline,
 * aperture-grille, glow-pyramid, and vignette overlay textures on software or
 * non-OpenGL renderers.
 *
 * Invariants:
 * - Auxiliary `TexSlot` textures are created on demand for the active
 *   `SDL_Renderer*` and released on `Shutdown()` / destruction.
 *
 * Not thread-safe; called on the main rendering thread.
 */
class CrtShader {
  public:
    CrtShader();
    ~CrtShader();

    CrtShader(const CrtShader&) = delete;
    CrtShader& operator=(const CrtShader&) = delete;

    /**
     * @brief Releases all cached SDL textures and compiled OpenGL program handles.
     *
     * Call before destroying the owning `SDL_Renderer*` so texture teardown
     * runs while the renderer context is still valid.
     */
    void Shutdown();

    /**
     * @brief Renders `canvas` onto `sdl_renderer`'s default target through the CRT simulation.
     * @param sdl_renderer Active SDL renderer whose default target is the window.
     * @param canvas Offscreen target texture containing the rendered UI frame.
     * @param geom Current canvas, frame, and DPI scaling metrics.
     */
    void Present(SDL_Renderer* sdl_renderer, SDL_Texture* canvas, const FrameGeometry& geom);

    /**
     * @brief Maps a physical window-space point `(x, y)` (in canvas device pixels) to its
     * unwarped point on `canvas`.
     */
    CanvasPoint Unwarp(const FrameGeometry& geom, double x, double y) const;

  private:
    struct TexSlot {
        SDL_Texture* tex = nullptr;  ///< Cached SDL texture handle.
        int width = 0;               ///< Texture width in pixels.
        int height = 0;              ///< Texture height in pixels.
        double key = 0.0;            ///< Invalidation key for procedural content.
    };
    struct GlProgramState;

    bool EnsureTexture(SDL_Renderer* sdl_renderer, TexSlot& slot, int width, int height, int access,
                       double key = 0.0);
    void UploadPixels(TexSlot& slot, const uint32_t* pixels) const;
    void BuildLines(int rows, int texels_per_row, double strength);
    void BuildStripes(int stripe_w);
    void BuildShade(double aspect);
    bool RenderThroughGlShader(SDL_Renderer* sdl_renderer, SDL_Texture* canvas,
                               const FrameGeometry& geom, int out_w, int out_h, double ratio_x,
                               double ratio_y, double row_px, double strength, double stripe_w);

    TexSlot half_{};     ///< Half-resolution downsample target for halation glow.
    TexSlot quarter_{};  ///< Quarter-resolution downsample target for halation glow.
    TexSlot eighth_{};   ///< Eighth-resolution downsample target for halation glow.
    TexSlot lines_{};    ///< 1-column texture encoding scanline attenuation across UI rows.
    TexSlot stripes_{};  ///< 1-row texture encoding RGB aperture-grille stripes.
    TexSlot shade_{};    ///< Vignette and rounded-corner rim darkening texture.
    std::unique_ptr<GlProgramState> gl_state_;  ///< Lazily initialized OpenGL shader state.
};

}  // namespace crt

namespace w2 {
using crt::CrtShader;
}  // namespace w2
