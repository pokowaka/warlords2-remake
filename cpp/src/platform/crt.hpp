// Optional CRT monitor simulation (View > CRT screen): barrel distortion,
// per-row scanlines, RGB aperture-grille stripes, halation glow, and rounded
// corner vignetting.
//
// When SDL uses an OpenGL renderer, the effect runs as a single GLSL 1.20
// fragment shader matching the web port (`web/src/crt.js`). On non-OpenGL or
// software renderers, it falls back to an `SDL_RenderGeometry` warped mesh
// with scanline, stripe, glow, and vignette overlay textures.
#pragma once

#include <utility>

struct SDL_Renderer;
struct SDL_Texture;

namespace crt {

/** @brief Renders `canvas` onto the window through the CRT simulation and leaves the window bound.
 */
void Present(SDL_Renderer* ren, SDL_Texture* canvas);
/** @brief Maps a window point `(x, y)` (in device pixels) to its unwarped canvas point. */
std::pair<double, double> Unwarp(double x, double y);

}  // namespace crt
