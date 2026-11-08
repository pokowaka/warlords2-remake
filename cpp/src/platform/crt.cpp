#include "platform/crt.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <SDL.h>
#include <SDL_opengl.h>

#include "platform/display.hpp"

namespace crt {

namespace {

// Barrel distortion factors along X and Y at the frame corners.
constexpr double kBowX = 1.0 / 64, kBowY = 1.0 / 48;
constexpr int kGridX = 64, kGridY = 48;       // Mesh subdivisions for the fallback geometry path.
constexpr double kScanline = 0.45;            // Peak scanline darkening between UI rows.
constexpr double kMask = 0.2;                 // Subpixel aperture-grille attenuation.
constexpr int kGlowNear = 70, kGlowFar = 90;  // Halation glow weights (out of 255).
constexpr double kCorner = 0.05,
                 kEdge = 0.012;  // Rounded corner radius and soft rim width in frame heights.

/**
 * @brief Cached auxiliary `SDL_Texture` with its dimensions and invalidation key.
 */
struct Tex {
    SDL_Texture* tex = nullptr;  ///< Underlying SDL texture handle.
    int w = 0;                   ///< Width in pixels.
    int h = 0;                   ///< Height in pixels.
    double key = 0;              ///< Parameter key for invalidating cached content.
};

Tex half, quarter, eighth;  // Downsampled frame pyramid for halation glow.
Tex lines;                  // 1-column texture encoding scanline brightness across UI rows.
Tex stripes;                // 1-row texture encoding RGB aperture-grille stripes across the window.
Tex shade;                  // Vignette and rounded-corner darkening texture.

/**
 * @brief Ensures `slot` holds a texture of `(width, height)` and `key`;
 * returns true when newly created and needing pixel upload.
 */
bool Need(SDL_Renderer* ren, Tex& slot, int width, int height, int access, double key = 0) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (slot.tex && slot.w == width && slot.h == height && slot.key == key) return false;
    if (slot.tex) SDL_DestroyTexture(slot.tex);
    slot.tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, access, width, height);
    slot.w = width;
    slot.h = height;
    slot.key = key;
    if (slot.tex) SDL_SetTextureScaleMode(slot.tex, SDL_ScaleModeLinear);
    return slot.tex != nullptr;
}

/** @brief Uploads ARGB8888 `pixels` into `slot`. */
void Fill(Tex& slot, const std::vector<Uint32>& pixels) {
    if (slot.tex) SDL_UpdateTexture(slot.tex, nullptr, pixels.data(), slot.w * 4);
}

/** @brief Packs a normalized greyscale brightness `val` (`0..1`) into ARGB8888. */
Uint32 Grey(double val) {
    Uint32 byte = static_cast<Uint32>(std::clamp(static_cast<int>(std::lround(val * 255)), 0, 255));
    return 0xff000000U | byte << 16 | byte << 8 | byte;
}

/**
 * @brief Computes the inverse barrel distortion in normalized `-1..1` frame
 * coordinates via fixed-point iteration.
 */
std::pair<double, double> Bow(double can_x, double can_y) {
    double scr_x = can_x, scr_y = can_y;
    for (int i = 0; i < 8; i++) {
        scr_x = can_x / (1 + kBowX * scr_y * scr_y);
        scr_y = can_y / (1 + kBowY * scr_x * scr_x);
    }
    return {scr_x, scr_y};
}

/**
 * @brief Populates the scanline texture for `rows` UI rows at `texels_per_row`
 * vertical resolution and `strength` darkening.
 */
void MakeLines(int rows, int texels_per_row, double strength) {
    std::vector<Uint32> pixels(static_cast<size_t>(rows) * texels_per_row);
    double mid = 0.5 - 0.5 / texels_per_row;
    for (size_t i = 0; i < pixels.size(); i++) {
        double offset = (i % texels_per_row + 0.5) / texels_per_row - mid;
        double beam = std::exp(-(offset * offset) / (0.3 * 0.3));
        pixels[i] = Grey(1 - strength * (1 - beam));
    }
    Fill(lines, pixels);
}

/** @brief Populates the RGB aperture-grille texture with stripes `stripe_w` pixels wide. */
void MakeStripes(int stripe_w) {
    std::vector<Uint32> pixels(stripes.w);
    Uint32 dim = static_cast<Uint32>(std::lround(255 * (1 - kMask)));
    for (int x = 0; x < stripes.w; x++) {
        int chan = x / stripe_w % 3;
        Uint32 red = chan == 0 ? 255 : dim;
        Uint32 green = chan == 1 ? 255 : dim;
        Uint32 blue = chan == 2 ? 255 : dim;
        pixels[x] = 0xff000000U | red << 16 | green << 8 | blue;
    }
    Fill(stripes, pixels);
}

/** @brief Populates the rounded-corner and vignette shading texture for `aspect`. */
void MakeShade(double aspect) {
    std::vector<Uint32> pixels(static_cast<size_t>(shade.w) * shade.h);
    for (int y = 0; y < shade.h; y++) {
        for (int x = 0; x < shade.w; x++) {
            double norm_u = (x + 0.5) / shade.w, norm_v = (y + 0.5) / shade.h;
            double dist_x = std::abs(norm_u * 2 - 1) * aspect - (aspect - kCorner);
            double dist_y = std::abs(norm_v * 2 - 1) - (1 - kCorner);
            double outside = std::hypot(std::max(dist_x, 0.0), std::max(dist_y, 0.0)) +
                             std::min(std::max(dist_x, dist_y), 0.0) - kCorner;
            double rim = std::clamp(-outside / kEdge, 0.0, 1.0);
            rim = rim * rim * (3 - 2 * rim);
            double vig = std::pow(std::max(0.0, 16 * norm_u * (1 - norm_u) * norm_v * (1 - norm_v)),
                                  0.12);
            pixels[static_cast<size_t>(y) * shade.w + x] = Grey(rim * vig);
        }
    }
    Fill(shade, pixels);
}

// --- OpenGL shader path ------------------------------------------------------

/**
 * @brief Dynamically resolved OpenGL entry points used by the CRT shader pass.
 */
struct GLFuns {
    decltype(&glGetIntegerv) get_integerv;
    decltype(&glIsEnabled) is_enabled;
    decltype(&glEnable) enable;
    decltype(&glDisable) disable;
    decltype(&glViewport) viewport;
    decltype(&glBindTexture) bind_texture;
    decltype(&glBegin) begin;
    decltype(&glEnd) end;
    decltype(&glVertex2f) vertex2f;
    PFNGLACTIVETEXTUREPROC active_texture;
    PFNGLCREATESHADERPROC create_shader;
    PFNGLSHADERSOURCEPROC shader_source;
    PFNGLCOMPILESHADERPROC compile_shader;
    PFNGLGETSHADERIVPROC get_shaderiv;
    PFNGLGETSHADERINFOLOGPROC get_shader_info_log;
    PFNGLDELETESHADERPROC delete_shader;
    PFNGLCREATEPROGRAMPROC create_program;
    PFNGLATTACHSHADERPROC attach_shader;
    PFNGLLINKPROGRAMPROC link_program;
    PFNGLGETPROGRAMIVPROC get_programiv;
    PFNGLGETPROGRAMINFOLOGPROC get_program_info_log;
    PFNGLUSEPROGRAMPROC use_program;
    PFNGLGETUNIFORMLOCATIONPROC get_uniform_location;
    PFNGLUNIFORM1IPROC uniform1i;
    PFNGLUNIFORM1FPROC uniform1f;
    PFNGLUNIFORM2FPROC uniform2f;
    PFNGLUNIFORM4FPROC uniform4f;
} gl_fns;

enum class Shader { kUntried, kReady, kNone };
Shader shader = Shader::kUntried;
GLuint program = 0;
struct {
    GLint screen, nearer, farther, height, scale_k, size, frame, rows, row_px, scan, stripe;
} uniforms;

/** @brief Formats `val` as a GLSL floating-point literal (always including a decimal point). */
std::string Lit(double val) {
    char buf[32];
    snprintf(buf, sizeof buf, "%.9g", val);
    std::string str = buf;
    if (str.find_first_of(".e") == std::string::npos) str += ".0";
    return str;
}

const char* vertex = R"(#version 120
void main() { gl_Position = vec4(gl_Vertex.xy, 0.0, 1.0); }
)";

/** @brief Builds the GLSL 1.20 fragment shader source for the CRT pass. */
std::string Fragment() {
    return R"(#version 120
uniform sampler2D screen;   // the canvas
uniform sampler2D nearer;   // the frame two halvings down
uniform sampler2D farther;  // and three
uniform float height;       // the window, in its pixels
uniform vec2 k;             // window pixels to a canvas pixel
uniform vec2 size;          // the canvas, in its pixels
uniform vec4 frame;         // the frame on it: x, y, w, h
uniform float rows;         // the frame's UI rows
uniform float rowPx;        // window pixels to a UI row
uniform float scan;         // the scanlines' strength
uniform float stripe;       // a stripe's width, in window pixels
const float BOW_X = )" +
           Lit(kBowX) + R"(, BOW_Y = )" + Lit(kBowY) + R"(;
const float MASK = )" +
           Lit(kMask) + R"(;
const float GLOW_NEAR = )" +
           Lit(kGlowNear / 255.0) + R"(, GLOW_FAR = )" + Lit(kGlowFar / 255.0) + R"(;
const float CORNER = )" +
           Lit(kCorner) + R"(, EDGE = )" + Lit(kEdge) + R"(;

void main() {
  vec2 w = vec2(gl_FragCoord.x, height - gl_FragCoord.y);
  vec2 p = w / k;
  vec2 n = (p - frame.xy) / frame.zw * 2.0 - 1.0;
  if (abs(n.x) > 1.0 || abs(n.y) > 1.0) {
    // off the frame: only black, or the pointer
    gl_FragColor = vec4(texture2D(screen, p / size).rgb, 1.0);
    return;
  }
  vec2 c = n * vec2(1.0 + BOW_X * n.y * n.y, 1.0 + BOW_Y * n.x * n.x);
  vec2 u = (c + 1.0) / 2.0;
  vec3 rgb = texture2D(screen, (frame.xy + u * frame.zw) / size).rgb;

  // the beam lights the top of the row and leaves its foot the darkest,
  // taken as the mean over the pixel's height, so the bow's slant across the
  // rows does not band
  float r = u.y * rows - (0.5 - 0.5 / rowPx), foot = dFdy(r), beam = 0.0;
  for (int i = 0; i < 4; i++) {
    float t = fract(r + foot * (float(i) - 1.5) / 4.0 + 0.5) - 0.5;
    beam += exp(-(t * t) / 0.09) / 4.0;
  }
  rgb *= 1.0 - scan * (1.0 - beam);

  // the aperture grille
  float s = mod(floor((w.x - floor(frame.x * k.x)) / stripe), 3.0);
  rgb *= mix(vec3(1.0 - MASK), vec3(1.0), vec3(float(s == 0.0), float(s == 1.0), float(s == 2.0)));

  rgb += texture2D(nearer, u).rgb * GLOW_NEAR + texture2D(farther, u).rgb * GLOW_FAR;

  // dark at the glass's rounded rim, and a little towards it everywhere
  float aspect = frame.z / frame.w;
  vec2 q = abs(c) * vec2(aspect, 1.0) - vec2(aspect - CORNER, 1.0 - CORNER);
  float outside = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - CORNER;
  float rim = smoothstep(0.0, 1.0, clamp(-outside / EDGE, 0.0, 1.0));
  float vig = pow(max(0.0, 16.0 * u.x * (1.0 - u.x) * u.y * (1.0 - u.y)), 0.12);
  gl_FragColor = vec4(rgb * rim * vig, 1.0);
}
)";
}

/** @brief Loads OpenGL function `name` into `fn_ptr`; returns true on success. */
template <class F>
bool Load(F& fn_ptr, const char* name) {
    fn_ptr = reinterpret_cast<F>(SDL_GL_GetProcAddress(name));
    return fn_ptr != nullptr;
}

/** @brief Compiles a GLSL shader of `type` from `src`, or returns 0 on failure. */
GLuint Compile(GLenum type, const char* src) {
    GLuint shd = gl_fns.create_shader(type);
    gl_fns.shader_source(shd, 1, &src, nullptr);
    gl_fns.compile_shader(shd);
    GLint compiled = 0;
    gl_fns.get_shaderiv(shd, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[1024] = "";
        gl_fns.get_shader_info_log(shd, sizeof log, nullptr, log);
        fprintf(stderr, "CRT shader: %s\n", log);
        gl_fns.delete_shader(shd);
        return 0;
    }
    return shd;
}

/** @brief Resolves OpenGL functions and links the CRT shader program. */
bool MakeShader() {
    bool loaded = Load(gl_fns.get_integerv, "glGetIntegerv") &&
                  Load(gl_fns.is_enabled, "glIsEnabled") && Load(gl_fns.enable, "glEnable") &&
                  Load(gl_fns.disable, "glDisable") && Load(gl_fns.viewport, "glViewport") &&
                  Load(gl_fns.bind_texture, "glBindTexture") && Load(gl_fns.begin, "glBegin") &&
                  Load(gl_fns.end, "glEnd") && Load(gl_fns.vertex2f, "glVertex2f") &&
                  Load(gl_fns.active_texture, "glActiveTexture") &&
                  Load(gl_fns.create_shader, "glCreateShader") &&
                  Load(gl_fns.shader_source, "glShaderSource") &&
                  Load(gl_fns.compile_shader, "glCompileShader") &&
                  Load(gl_fns.get_shaderiv, "glGetShaderiv") &&
                  Load(gl_fns.get_shader_info_log, "glGetShaderInfoLog") &&
                  Load(gl_fns.delete_shader, "glDeleteShader") &&
                  Load(gl_fns.create_program, "glCreateProgram") &&
                  Load(gl_fns.attach_shader, "glAttachShader") &&
                  Load(gl_fns.link_program, "glLinkProgram") &&
                  Load(gl_fns.get_programiv, "glGetProgramiv") &&
                  Load(gl_fns.get_program_info_log, "glGetProgramInfoLog") &&
                  Load(gl_fns.use_program, "glUseProgram") &&
                  Load(gl_fns.get_uniform_location, "glGetUniformLocation") &&
                  Load(gl_fns.uniform1i, "glUniform1i") && Load(gl_fns.uniform1f, "glUniform1f") &&
                  Load(gl_fns.uniform2f, "glUniform2f") && Load(gl_fns.uniform4f, "glUniform4f");
    if (!loaded) return false;
    std::string frag = Fragment();
    GLuint vert_shd = Compile(GL_VERTEX_SHADER, vertex);
    GLuint frag_shd = Compile(GL_FRAGMENT_SHADER, frag.c_str());
    if (!vert_shd || !frag_shd) return false;
    program = gl_fns.create_program();
    gl_fns.attach_shader(program, vert_shd);
    gl_fns.attach_shader(program, frag_shd);
    gl_fns.link_program(program);
    gl_fns.delete_shader(vert_shd);
    gl_fns.delete_shader(frag_shd);
    GLint linked = 0;
    gl_fns.get_programiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024] = "";
        gl_fns.get_program_info_log(program, sizeof log, nullptr, log);
        fprintf(stderr, "CRT shader: %s\n", log);
        return false;
    }
    uniforms.screen = gl_fns.get_uniform_location(program, "screen");
    uniforms.nearer = gl_fns.get_uniform_location(program, "nearer");
    uniforms.farther = gl_fns.get_uniform_location(program, "farther");
    uniforms.height = gl_fns.get_uniform_location(program, "height");
    uniforms.scale_k = gl_fns.get_uniform_location(program, "k");
    uniforms.size = gl_fns.get_uniform_location(program, "size");
    uniforms.frame = gl_fns.get_uniform_location(program, "frame");
    uniforms.rows = gl_fns.get_uniform_location(program, "rows");
    uniforms.row_px = gl_fns.get_uniform_location(program, "rowPx");
    uniforms.scan = gl_fns.get_uniform_location(program, "scan");
    uniforms.stripe = gl_fns.get_uniform_location(program, "stripe");
    return true;
}

/** @brief Queries the underlying `GL_TEXTURE_2D` handle of `tex`, or returns 0 if non-2D. */
GLuint Name(SDL_Texture* tex) {
    float tex_w = 0, tex_h = 0;
    if (SDL_GL_BindTexture(tex, &tex_w, &tex_h) != 0) return 0;
    GLint gl_id = 0;
    gl_fns.get_integerv(GL_TEXTURE_BINDING_2D, &gl_id);
    SDL_GL_UnbindTexture(tex);
    return tex_w == 1 && tex_h == 1 ? static_cast<GLuint>(gl_id) : 0;
}

/**
 * @brief Per-frame display parameters passed to the CRT shader.
 */
struct Look {
    int out_w, out_h;         ///< Physical window output size in pixels.
    double ratio_x, ratio_y;  ///< Window pixels per canvas pixel.
    double row_px;            ///< Window pixels per UI row.
    double strength;          ///< Scanline darkening strength.
    double stripe;            ///< Aperture-grille stripe width in window pixels.
};

/**
 * @brief Renders the CRT pass via the OpenGL shader if available, preserving
 * and restoring SDL's OpenGL state. Returns false when the fallback mesh is needed.
 */
bool ThroughShader(SDL_Renderer* ren, SDL_Texture* canvas, const Look& look) {
    if (shader == Shader::kNone) return false;
    if (shader == Shader::kUntried) {
        SDL_RendererInfo info{};
        bool is_gl = SDL_GetRendererInfo(ren, &info) == 0 && info.name &&
                     strcmp(info.name, "opengl") == 0;
        shader = is_gl && MakeShader() ? Shader::kReady : Shader::kNone;
        if (shader == Shader::kNone) return false;
    }
    const auto& disp = display::d;
    SDL_RenderFlush(ren);

    // Save SDL's current OpenGL state.
    GLint active = 0, prog = 0, view[4] = {};
    GLint bound[3] = {};
    GLboolean enabled[3] = {};
    gl_fns.get_integerv(GL_ACTIVE_TEXTURE, &active);
    for (int i = 0; i < 3; i++) {
        gl_fns.active_texture(GL_TEXTURE0 + i);
        gl_fns.get_integerv(GL_TEXTURE_BINDING_2D, &bound[i]);
        enabled[i] = gl_fns.is_enabled(GL_TEXTURE_2D);
    }
    gl_fns.active_texture(GL_TEXTURE0);
    gl_fns.get_integerv(GL_CURRENT_PROGRAM, &prog);
    gl_fns.get_integerv(GL_VIEWPORT, view);
    GLboolean blend = gl_fns.is_enabled(GL_BLEND), scissor = gl_fns.is_enabled(GL_SCISSOR_TEST);

    GLuint tex[3] = {Name(canvas), Name(quarter.tex), Name(eighth.tex)};
    bool ready = tex[0] && tex[1] && tex[2];
    if (ready) {
        for (int i = 0; i < 3; i++) {
            gl_fns.active_texture(GL_TEXTURE0 + i);
            gl_fns.bind_texture(GL_TEXTURE_2D, tex[i]);
        }
        gl_fns.use_program(program);
        gl_fns.uniform1i(uniforms.screen, 0);
        gl_fns.uniform1i(uniforms.nearer, 1);
        gl_fns.uniform1i(uniforms.farther, 2);
        gl_fns.uniform1f(uniforms.height, static_cast<float>(look.out_h));
        gl_fns.uniform2f(uniforms.scale_k, static_cast<float>(look.ratio_x),
                         static_cast<float>(look.ratio_y));
        gl_fns.uniform2f(uniforms.size, static_cast<float>(disp.pw), static_cast<float>(disp.ph));
        gl_fns.uniform4f(uniforms.frame, static_cast<float>(disp.ox), static_cast<float>(disp.oy),
                         static_cast<float>(disp.w * disp.scale),
                         static_cast<float>(disp.h * disp.scale));
        gl_fns.uniform1f(uniforms.rows, static_cast<float>(disp.h));
        gl_fns.uniform1f(uniforms.row_px, static_cast<float>(look.row_px));
        gl_fns.uniform1f(uniforms.scan, static_cast<float>(look.strength));
        gl_fns.uniform1f(uniforms.stripe, static_cast<float>(look.stripe));
        gl_fns.viewport(0, 0, look.out_w, look.out_h);
        gl_fns.disable(GL_BLEND);
        gl_fns.disable(GL_SCISSOR_TEST);
        // Fullscreen triangle covering the window.
        gl_fns.begin(GL_TRIANGLES);
        gl_fns.vertex2f(-1, -1);
        gl_fns.vertex2f(3, -1);
        gl_fns.vertex2f(-1, 3);
        gl_fns.end();
    }

    // Restore SDL's OpenGL state.
    for (int i = 2; i >= 0; i--) {
        gl_fns.active_texture(GL_TEXTURE0 + i);
        gl_fns.bind_texture(GL_TEXTURE_2D, bound[i]);
        (enabled[i] ? gl_fns.enable : gl_fns.disable)(GL_TEXTURE_2D);
    }
    gl_fns.active_texture(active);
    gl_fns.use_program(prog);
    gl_fns.viewport(view[0], view[1], view[2], view[3]);
    (blend ? gl_fns.enable : gl_fns.disable)(GL_BLEND);
    (scissor ? gl_fns.enable : gl_fns.disable)(GL_SCISSOR_TEST);
    if (!ready) shader = Shader::kNone;
    return ready;
}

}  // namespace

std::pair<double, double> Unwarp(double x, double y) {
    const auto& disp = display::d;
    double frame_w = disp.w * disp.scale, frame_h = disp.h * disp.scale;
    if (frame_w <= 0 || frame_h <= 0) return {x, y};
    double norm_x = (x - disp.ox) / frame_w * 2 - 1;
    double norm_y = (y - disp.oy) / frame_h * 2 - 1;
    double can_x = norm_x * (1 + kBowX * norm_y * norm_y);
    double can_y = norm_y * (1 + kBowY * norm_x * norm_x);
    return {disp.ox + (can_x + 1) / 2 * frame_w, disp.oy + (can_y + 1) / 2 * frame_h};
}

void Present(SDL_Renderer* ren, SDL_Texture* canvas) {
    const auto& disp = display::d;
    int frame_x = disp.ox, frame_y = disp.oy;
    int frame_w = disp.w * disp.scale, frame_h = disp.h * disp.scale;
    SDL_SetTextureBlendMode(canvas, SDL_BLENDMODE_NONE);

    // Downsample the frame three times for the halation glow pyramid.
    SDL_Rect frame{frame_x, frame_y, frame_w, frame_h};
    Need(ren, half, frame_w / 2, frame_h / 2, SDL_TEXTUREACCESS_TARGET);
    Need(ren, quarter, frame_w / 4, frame_h / 4, SDL_TEXTUREACCESS_TARGET);
    Need(ren, eighth, frame_w / 8, frame_h / 8, SDL_TEXTUREACCESS_TARGET);
    bool glow = half.tex && quarter.tex && eighth.tex;
    if (glow) {
        SDL_RenderSetClipRect(ren, nullptr);
        SDL_SetRenderTarget(ren, half.tex);
        SDL_RenderCopy(ren, canvas, &frame, nullptr);
        SDL_SetTextureBlendMode(half.tex, SDL_BLENDMODE_NONE);
        SDL_SetRenderTarget(ren, quarter.tex);
        SDL_RenderCopy(ren, half.tex, nullptr, nullptr);
        SDL_SetTextureBlendMode(quarter.tex, SDL_BLENDMODE_NONE);
        SDL_SetRenderTarget(ren, eighth.tex);
        SDL_RenderCopy(ren, quarter.tex, nullptr, nullptr);
    }

    SDL_SetRenderTarget(ren, nullptr);
    SDL_RenderSetClipRect(ren, nullptr);
    int out_w = 0, out_h = 0;
    SDL_GetRendererOutputSize(ren, &out_w, &out_h);
    double ratio_x = static_cast<double>(out_w) / disp.pw;
    double ratio_y = static_cast<double>(out_h) / disp.ph;
    double row_px = disp.scale * ratio_y;
    // Fade scanlines when a UI row spans fewer than ~3 fractional window pixels.
    bool whole = row_px >= 2 && std::abs(row_px - std::round(row_px)) < 0.01;
    double strength = kScanline * (whole ? 1 : std::clamp((row_px - 1.5) / 1.5, 0.0, 1.0));
    int stripe_w = std::max(1, static_cast<int>(std::lround(disp.dpi * disp.shrink)));
    if (glow && ThroughShader(ren, canvas,
                              {out_w, out_h, ratio_x, ratio_y, row_px, strength,
                               static_cast<double>(stripe_w)}))
        return;

    // Fallback geometry path: draw border area, clear the frame region, and
    // render the bowed mesh with overlay passes.
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, canvas, nullptr, nullptr);
    SDL_Rect on_window{static_cast<int>(std::floor(frame_x * ratio_x)),
                       static_cast<int>(std::floor(frame_y * ratio_y)),
                       static_cast<int>(std::ceil(frame_w * ratio_x)) + 1,
                       static_cast<int>(std::ceil(frame_h * ratio_y)) + 1};
    SDL_RenderFillRect(ren, &on_window);

    std::vector<SDL_Vertex> on_canvas, on_frame, on_lines;
    std::vector<int> indices;
    int row_texels = 2;
    {
        SDL_RendererInfo info{};
        SDL_GetRendererInfo(ren, &info);
        int max_h = info.max_texture_height > 0 ? info.max_texture_height : 8192;
        row_texels = std::clamp(static_cast<int>(std::lround(row_px)), 2, 8);
        row_texels = std::max(2, std::min(row_texels, max_h / std::max(1, disp.h)));
    }
    for (int j = 0; j <= kGridY; j++) {
        for (int i = 0; i <= kGridX; i++) {
            double norm_u = static_cast<double>(i) / kGridX;
            double norm_v = static_cast<double>(j) / kGridY;
            auto [scr_x, scr_y] = Bow(norm_u * 2 - 1, norm_v * 2 - 1);
            SDL_Vertex vert{};
            vert.position = {static_cast<float>((frame_x + (scr_x + 1) / 2 * frame_w) * ratio_x),
                             static_cast<float>((frame_y + (scr_y + 1) / 2 * frame_h) * ratio_y)};
            vert.color = {255, 255, 255, 255};
            vert.tex_coord = {static_cast<float>((frame_x + norm_u * frame_w) / disp.pw),
                              static_cast<float>((frame_y + norm_v * frame_h) / disp.ph)};
            on_canvas.push_back(vert);
            vert.tex_coord = {static_cast<float>(norm_u), static_cast<float>(norm_v)};
            on_frame.push_back(vert);
            vert.tex_coord = {0.5F, static_cast<float>(norm_v)};
            on_lines.push_back(vert);
        }
    }
    for (int j = 0; j < kGridY; j++) {
        for (int i = 0; i < kGridX; i++) {
            int top_l = j * (kGridX + 1) + i, top_r = top_l + 1;
            int bot_l = top_l + kGridX + 1, bot_r = bot_l + 1;
            indices.insert(indices.end(), {top_l, top_r, bot_l, top_r, bot_r, bot_l});
        }
    }
    auto draw_mesh = [&](SDL_Texture* tex, std::vector<SDL_Vertex>& verts, SDL_BlendMode mode,
                         Uint8 tint = 255) {
        if (!tex) return;
        for (SDL_Vertex& vert : verts) vert.color = {tint, tint, tint, 255};
        SDL_SetTextureBlendMode(tex, mode);
        SDL_RenderGeometry(ren, tex, verts.data(), static_cast<int>(verts.size()), indices.data(),
                           static_cast<int>(indices.size()));
    };

    draw_mesh(canvas, on_canvas, SDL_BLENDMODE_NONE);

    if (Need(ren, lines, 1, disp.h * row_texels, SDL_TEXTUREACCESS_STATIC,
             row_texels * 1000 + strength))
        MakeLines(disp.h, row_texels, strength);
    draw_mesh(lines.tex, on_lines, SDL_BLENDMODE_MOD);

    if (Need(ren, stripes, out_w, 1, SDL_TEXTUREACCESS_STATIC, stripe_w)) {
        SDL_SetTextureScaleMode(stripes.tex, SDL_ScaleModeNearest);
        MakeStripes(stripe_w);
    }
    if (stripes.tex) {
        SDL_Rect src{0, 0, std::min(on_window.w, stripes.w), 1}, dst = on_window;
        dst.w = src.w;
        SDL_SetTextureBlendMode(stripes.tex, SDL_BLENDMODE_MOD);
        SDL_RenderCopy(ren, stripes.tex, &src, &dst);
    }

    if (glow) {
        draw_mesh(quarter.tex, on_frame, SDL_BLENDMODE_ADD, kGlowNear);
        draw_mesh(eighth.tex, on_frame, SDL_BLENDMODE_ADD, kGlowFar);
    }

    double aspect = frame_h > 0 ? static_cast<double>(frame_w) / frame_h : 4.0 / 3;
    if (Need(ren, shade, 256, std::clamp(static_cast<int>(std::lround(256 / aspect)), 64, 512),
             SDL_TEXTUREACCESS_STATIC, aspect))
        MakeShade(aspect);
    draw_mesh(shade.tex, on_frame, SDL_BLENDMODE_MOD);
}

}  // namespace crt
