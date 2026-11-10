#include "platform/crt.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <SDL.h>
#include <SDL_opengl.h>
#include "absl/strings/str_format.h"

namespace crt {

namespace {

constexpr double kBowX = 1.0 / 64.0, kBowY = 1.0 / 48.0;
constexpr int kGridX = 64, kGridY = 48;
constexpr double kScanline = 0.45, kMask = 0.2;
constexpr int kGlowNear = 70, kGlowFar = 90;
constexpr double kCorner = 0.05, kEdge = 0.012;

enum class ShaderState { kUntried, kReady, kNone };

enum UniformSlot {
    kUniScreen = 0,
    kUniNearer,
    kUniFarther,
    kUniHeight,
    kUniScaleK,
    kUniSize,
    kUniFrame,
    kUniRows,
    kUniRowPx,
    kUniScan,
    kUniStripe,
    kUniformCount,
};

constexpr std::array<const char*, kUniformCount> kUniformNames = {
    "screen", "nearer", "farther", "height", "k",     "size",
    "frame",  "rows",   "rowPx",   "scan",   "stripe"};

/** @brief Packs normalized greyscale brightness `val` (`0..1`) into ARGB8888. */
uint32_t PackGrey(double val) {
    uint32_t byte_val =
            static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(val * 255.0)), 0, 255));
    return 0xff000000U | (byte_val << 16) | (byte_val << 8) | byte_val;
}

/** @brief Computes inverse barrel distortion in normalized `-1..1` frame coordinates. */
CanvasPoint InverseBow(double can_x, double can_y) {
    double scr_x = can_x, scr_y = can_y;
    for (int i = 0; i < 8; i++) {
        scr_x = can_x / (1.0 + kBowX * scr_y * scr_y);
        scr_y = can_y / (1.0 + kBowY * scr_x * scr_x);
    }
    return {scr_x, scr_y};
}

/** @brief Formats `val` as a GLSL floating-point literal (always including a decimal point). */
std::string GlslLiteral(double val) {
    std::string formatted = absl::StrFormat("%.9g", val);
    if (formatted.find_first_of(".e") == std::string::npos) formatted += ".0";
    return formatted;
}

constexpr const char* kVertexShaderSrc =
        "#version 120\nvoid main() { gl_Position = vec4(gl_Vertex.xy, 0.0, 1.0); }\n";

/** @brief Builds the GLSL 1.20 fragment shader source matching the web port's CRT pass. */
std::string BuildFragmentShaderSrc() {
    return R"(#version 120
uniform sampler2D screen, nearer, farther;
uniform float height, rows, rowPx, scan, stripe;
uniform vec2 k, size;
uniform vec4 frame;
const float BOW_X = )" +
           GlslLiteral(kBowX) + ", BOW_Y = " + GlslLiteral(kBowY) +
           ";\nconst float MASK = " + GlslLiteral(kMask) +
           ";\nconst float GLOW_NEAR = " + GlslLiteral(kGlowNear / 255.0) +
           ", GLOW_FAR = " + GlslLiteral(kGlowFar / 255.0) +
           ";\nconst float CORNER = " + GlslLiteral(kCorner) + ", EDGE = " + GlslLiteral(kEdge) +
           R"(;
void main() {
  vec2 w = vec2(gl_FragCoord.x, height - gl_FragCoord.y);
  vec2 p = w / k;
  vec2 n = (p - frame.xy) / frame.zw * 2.0 - 1.0;
  if (abs(n.x) > 1.0 || abs(n.y) > 1.0) {
    gl_FragColor = vec4(texture2D(screen, p / size).rgb, 1.0);
    return;
  }
  vec2 c = n * vec2(1.0 + BOW_X * n.y * n.y, 1.0 + BOW_Y * n.x * n.x);
  vec2 u = (c + 1.0) / 2.0;
  vec3 rgb = texture2D(screen, (frame.xy + u * frame.zw) / size).rgb;
  float r = u.y * rows - (0.5 - 0.5 / rowPx), foot = dFdy(r), beam = 0.0;
  for (int i = 0; i < 4; i++) {
    float t = fract(r + foot * (float(i) - 1.5) / 4.0 + 0.5) - 0.5;
    beam += exp(-(t * t) / 0.09) / 4.0;
  }
  rgb *= 1.0 - scan * (1.0 - beam);
  float s = mod(floor((w.x - floor(frame.x * k.x)) / stripe), 3.0);
  rgb *= mix(vec3(1.0 - MASK), vec3(1.0), vec3(float(s == 0.0), float(s == 1.0), float(s == 2.0)));
  rgb += texture2D(nearer, u).rgb * GLOW_NEAR + texture2D(farther, u).rgb * GLOW_FAR;
  float aspect = frame.z / frame.w;
  vec2 q = abs(c) * vec2(aspect, 1.0) - vec2(aspect - CORNER, 1.0 - CORNER);
  float outside = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - CORNER;
  float rim = smoothstep(0.0, 1.0, clamp(-outside / EDGE, 0.0, 1.0));
  float vig = pow(max(0.0, 16.0 * u.x * (1.0 - u.x) * u.y * (1.0 - u.y)), 0.12);
  gl_FragColor = vec4(rgb * rim * vig, 1.0);
}
)";
}

/** @brief Resolves OpenGL entry point `proc_name` into `fn_ptr`. */
template <class FnPtr>
bool LoadGlProc(FnPtr& fn_ptr, const char* proc_name) {
    fn_ptr = reinterpret_cast<FnPtr>(SDL_GL_GetProcAddress(proc_name));
    return fn_ptr != nullptr;
}

/** @brief Draws `verts` indexed by `indices` with `tex`, `mode`, and greyscale `tint`. */
void DrawWarpedMesh(SDL_Renderer* sdl_renderer, SDL_Texture* tex, std::vector<SDL_Vertex>& verts,
                    const std::vector<int>& indices, SDL_BlendMode mode, Uint8 tint = 255) {
    if (tex == nullptr) return;
    for (SDL_Vertex& vert : verts) vert.color = {tint, tint, tint, 255};
    SDL_SetTextureBlendMode(tex, mode);
    SDL_RenderGeometry(sdl_renderer, tex, verts.data(), static_cast<int>(verts.size()),
                       indices.data(), static_cast<int>(indices.size()));
}

}  // namespace

struct CrtShader::GlProgramState {
    ShaderState state = ShaderState::kUntried;
    GLuint program = 0;
    std::array<GLint, kUniformCount> locs{};
    decltype(&glGetIntegerv) get_integerv = nullptr;
    decltype(&glIsEnabled) is_enabled = nullptr;
    decltype(&glEnable) enable = nullptr;
    decltype(&glDisable) disable = nullptr;
    decltype(&glViewport) viewport = nullptr;
    decltype(&glBindTexture) bind_texture = nullptr;
    decltype(&glBegin) begin = nullptr;
    decltype(&glEnd) end = nullptr;
    decltype(&glVertex2f) vertex2f = nullptr;
    PFNGLACTIVETEXTUREPROC active_texture = nullptr;
    PFNGLCREATESHADERPROC create_shader = nullptr;
    PFNGLSHADERSOURCEPROC shader_source = nullptr;
    PFNGLCOMPILESHADERPROC compile_shader = nullptr;
    PFNGLGETSHADERIVPROC get_shaderiv = nullptr;
    PFNGLDELETESHADERPROC delete_shader = nullptr;
    PFNGLCREATEPROGRAMPROC create_program = nullptr;
    PFNGLATTACHSHADERPROC attach_shader = nullptr;
    PFNGLLINKPROGRAMPROC link_program = nullptr;
    PFNGLGETPROGRAMIVPROC get_programiv = nullptr;
    PFNGLDELETEPROGRAMPROC delete_program = nullptr;
    PFNGLUSEPROGRAMPROC use_program = nullptr;
    PFNGLGETUNIFORMLOCATIONPROC get_uniform_location = nullptr;
    PFNGLUNIFORM1IPROC uniform1i = nullptr;
    PFNGLUNIFORM1FPROC uniform1f = nullptr;
    PFNGLUNIFORM2FPROC uniform2f = nullptr;
    PFNGLUNIFORM4FPROC uniform4f = nullptr;

    GLuint CompileStage(GLenum stage_type, const char* src) const {
        GLuint shd = create_shader(stage_type);
        shader_source(shd, 1, &src, nullptr);
        compile_shader(shd);
        GLint compiled = 0;
        get_shaderiv(shd, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            delete_shader(shd);
            return 0;
        }
        return shd;
    }

    bool InitProgram() {
        bool procs_ok =
                LoadGlProc(get_integerv, "glGetIntegerv") &&
                LoadGlProc(is_enabled, "glIsEnabled") && LoadGlProc(enable, "glEnable") &&
                LoadGlProc(disable, "glDisable") && LoadGlProc(viewport, "glViewport") &&
                LoadGlProc(bind_texture, "glBindTexture") && LoadGlProc(begin, "glBegin") &&
                LoadGlProc(end, "glEnd") && LoadGlProc(vertex2f, "glVertex2f") &&
                LoadGlProc(active_texture, "glActiveTexture") &&
                LoadGlProc(create_shader, "glCreateShader") &&
                LoadGlProc(shader_source, "glShaderSource") &&
                LoadGlProc(compile_shader, "glCompileShader") &&
                LoadGlProc(get_shaderiv, "glGetShaderiv") &&
                LoadGlProc(delete_shader, "glDeleteShader") &&
                LoadGlProc(create_program, "glCreateProgram") &&
                LoadGlProc(attach_shader, "glAttachShader") &&
                LoadGlProc(link_program, "glLinkProgram") &&
                LoadGlProc(get_programiv, "glGetProgramiv") &&
                LoadGlProc(delete_program, "glDeleteProgram") &&
                LoadGlProc(use_program, "glUseProgram") &&
                LoadGlProc(get_uniform_location, "glGetUniformLocation") &&
                LoadGlProc(uniform1i, "glUniform1i") && LoadGlProc(uniform1f, "glUniform1f") &&
                LoadGlProc(uniform2f, "glUniform2f") && LoadGlProc(uniform4f, "glUniform4f");
        if (!procs_ok) return false;
        std::string frag_src = BuildFragmentShaderSrc();
        GLuint vert_shd = CompileStage(GL_VERTEX_SHADER, kVertexShaderSrc);
        GLuint frag_shd = CompileStage(GL_FRAGMENT_SHADER, frag_src.c_str());
        if (!vert_shd || !frag_shd) {
            if (vert_shd) delete_shader(vert_shd);
            if (frag_shd) delete_shader(frag_shd);
            return false;
        }
        program = create_program();
        attach_shader(program, vert_shd);
        attach_shader(program, frag_shd);
        link_program(program);
        delete_shader(vert_shd);
        delete_shader(frag_shd);
        GLint linked = 0;
        get_programiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            delete_program(program);
            program = 0;
            return false;
        }
        for (size_t i = 0; i < kUniformCount; i++) {
            locs[i] = get_uniform_location(program, kUniformNames[i]);
        }
        return true;
    }

    GLuint QueryGlTexture2D(SDL_Texture* tex) const {
        float tex_w = 0.0F, tex_h = 0.0F;
        if (SDL_GL_BindTexture(tex, &tex_w, &tex_h) != 0) return 0;
        GLint gl_id = 0;
        get_integerv(GL_TEXTURE_BINDING_2D, &gl_id);
        SDL_GL_UnbindTexture(tex);
        return tex_w == 1.0F && tex_h == 1.0F ? static_cast<GLuint>(gl_id) : 0;
    }
};

CrtShader::CrtShader() : gl_state_(std::make_unique<GlProgramState>()) {}
CrtShader::~CrtShader() {
    Shutdown();
}

void CrtShader::Shutdown() {
    for (TexSlot* slot : {&half_, &quarter_, &eighth_, &lines_, &stripes_, &shade_}) {
        if (slot->tex != nullptr) SDL_DestroyTexture(slot->tex);
        *slot = TexSlot{};
    }
    if (gl_state_ && gl_state_->program != 0 && gl_state_->delete_program != nullptr) {
        gl_state_->delete_program(gl_state_->program);
    }
    gl_state_ = std::make_unique<GlProgramState>();
}

bool CrtShader::EnsureTexture(SDL_Renderer* sdl_renderer, TexSlot& slot, int width, int height,
                              int access, double key) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (slot.tex != nullptr && slot.width == width && slot.height == height && slot.key == key) {
        return false;
    }
    if (slot.tex != nullptr) SDL_DestroyTexture(slot.tex);
    slot.tex = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888, access, width, height);
    slot.width = width;
    slot.height = height;
    slot.key = key;
    if (slot.tex != nullptr) SDL_SetTextureScaleMode(slot.tex, SDL_ScaleModeLinear);
    return slot.tex != nullptr;
}

void CrtShader::UploadPixels(TexSlot& slot, const uint32_t* pixels) const {
    if (slot.tex != nullptr) SDL_UpdateTexture(slot.tex, nullptr, pixels, slot.width * 4);
}

void CrtShader::BuildLines(int rows, int texels_per_row, double strength) {
    std::vector<uint32_t> pixels(static_cast<size_t>(rows) * static_cast<size_t>(texels_per_row));
    double mid = 0.5 - 0.5 / texels_per_row;
    for (size_t i = 0; i < pixels.size(); i++) {
        double offset = (i % texels_per_row + 0.5) / texels_per_row - mid;
        double beam = std::exp(-(offset * offset) / (0.3 * 0.3));
        pixels[i] = PackGrey(1.0 - strength * (1.0 - beam));
    }
    UploadPixels(lines_, pixels.data());
}

void CrtShader::BuildStripes(int stripe_w) {
    std::vector<uint32_t> pixels(static_cast<size_t>(stripes_.width));
    uint32_t dim = static_cast<uint32_t>(std::lround(255.0 * (1.0 - kMask)));
    for (int x = 0; x < stripes_.width; x++) {
        int chan = (x / stripe_w) % 3;
        uint32_t red = chan == 0 ? 255U : dim;
        uint32_t green = chan == 1 ? 255U : dim;
        uint32_t blue = chan == 2 ? 255U : dim;
        pixels[x] = 0xff000000U | (red << 16) | (green << 8) | blue;
    }
    UploadPixels(stripes_, pixels.data());
}

void CrtShader::BuildShade(double aspect) {
    std::vector<uint32_t> pixels(static_cast<size_t>(shade_.width) *
                                 static_cast<size_t>(shade_.height));
    for (int y = 0; y < shade_.height; y++) {
        for (int x = 0; x < shade_.width; x++) {
            double norm_u = (x + 0.5) / shade_.width, norm_v = (y + 0.5) / shade_.height;
            double dist_x = std::abs(norm_u * 2.0 - 1.0) * aspect - (aspect - kCorner);
            double dist_y = std::abs(norm_v * 2.0 - 1.0) - (1.0 - kCorner);
            double outside = std::hypot(std::max(dist_x, 0.0), std::max(dist_y, 0.0)) +
                             std::min(std::max(dist_x, dist_y), 0.0) - kCorner;
            double rim = std::clamp(-outside / kEdge, 0.0, 1.0);
            rim = rim * rim * (3.0 - 2.0 * rim);
            double vig = std::pow(
                    std::max(0.0, 16.0 * norm_u * (1.0 - norm_u) * norm_v * (1.0 - norm_v)), 0.12);
            pixels[static_cast<size_t>(y) * shade_.width + x] = PackGrey(rim * vig);
        }
    }
    UploadPixels(shade_, pixels.data());
}

bool CrtShader::RenderThroughGlShader(SDL_Renderer* sdl_renderer, SDL_Texture* canvas,
                                      const FrameGeometry& geom, int out_w, int out_h,
                                      double ratio_x, double ratio_y, double row_px,
                                      double strength, double stripe_w) {
    if (gl_state_->state == ShaderState::kNone) return false;
    if (gl_state_->state == ShaderState::kUntried) {
        SDL_RendererInfo info{};
        bool is_gl = SDL_GetRendererInfo(sdl_renderer, &info) == 0 && info.name != nullptr &&
                     std::strcmp(info.name, "opengl") == 0;
        gl_state_->state =
                is_gl && gl_state_->InitProgram() ? ShaderState::kReady : ShaderState::kNone;
        if (gl_state_->state == ShaderState::kNone) return false;
    }
    SDL_RenderFlush(sdl_renderer);

    GLint active_unit = 0, prev_prog = 0, prev_view[4] = {}, bound_tex[3] = {};
    GLboolean tex_enabled[3] = {};
    gl_state_->get_integerv(GL_ACTIVE_TEXTURE, &active_unit);
    for (int i = 0; i < 3; i++) {
        gl_state_->active_texture(GL_TEXTURE0 + i);
        gl_state_->get_integerv(GL_TEXTURE_BINDING_2D, &bound_tex[i]);
        tex_enabled[i] = gl_state_->is_enabled(GL_TEXTURE_2D);
    }
    gl_state_->active_texture(GL_TEXTURE0);
    gl_state_->get_integerv(GL_CURRENT_PROGRAM, &prev_prog);
    gl_state_->get_integerv(GL_VIEWPORT, prev_view);
    GLboolean blend_on = gl_state_->is_enabled(GL_BLEND);
    GLboolean scissor_on = gl_state_->is_enabled(GL_SCISSOR_TEST);

    GLuint gl_tex[3] = {gl_state_->QueryGlTexture2D(canvas),
                        gl_state_->QueryGlTexture2D(quarter_.tex),
                        gl_state_->QueryGlTexture2D(eighth_.tex)};
    bool ready = gl_tex[0] != 0 && gl_tex[1] != 0 && gl_tex[2] != 0;
    if (ready) {
        for (int i = 0; i < 3; i++) {
            gl_state_->active_texture(GL_TEXTURE0 + i);
            gl_state_->bind_texture(GL_TEXTURE_2D, gl_tex[i]);
        }
        const auto& locs = gl_state_->locs;
        gl_state_->use_program(gl_state_->program);
        gl_state_->uniform1i(locs[kUniScreen], 0);
        gl_state_->uniform1i(locs[kUniNearer], 1);
        gl_state_->uniform1i(locs[kUniFarther], 2);
        gl_state_->uniform1f(locs[kUniHeight], static_cast<float>(out_h));
        gl_state_->uniform2f(locs[kUniScaleK], static_cast<float>(ratio_x),
                             static_cast<float>(ratio_y));
        gl_state_->uniform2f(locs[kUniSize], static_cast<float>(geom.canvas_w),
                             static_cast<float>(geom.canvas_h));
        gl_state_->uniform4f(locs[kUniFrame], static_cast<float>(geom.frame_x),
                             static_cast<float>(geom.frame_y),
                             static_cast<float>(geom.frame_ui_w * geom.scale),
                             static_cast<float>(geom.frame_ui_h * geom.scale));
        gl_state_->uniform1f(locs[kUniRows], static_cast<float>(geom.frame_ui_h));
        gl_state_->uniform1f(locs[kUniRowPx], static_cast<float>(row_px));
        gl_state_->uniform1f(locs[kUniScan], static_cast<float>(strength));
        gl_state_->uniform1f(locs[kUniStripe], static_cast<float>(stripe_w));
        gl_state_->viewport(0, 0, out_w, out_h);
        gl_state_->disable(GL_BLEND);
        gl_state_->disable(GL_SCISSOR_TEST);
        gl_state_->begin(GL_TRIANGLES);
        gl_state_->vertex2f(-1.0F, -1.0F);
        gl_state_->vertex2f(3.0F, -1.0F);
        gl_state_->vertex2f(-1.0F, 3.0F);
        gl_state_->end();
    }

    for (int i = 2; i >= 0; i--) {
        gl_state_->active_texture(GL_TEXTURE0 + i);
        gl_state_->bind_texture(GL_TEXTURE_2D, bound_tex[i]);
        (tex_enabled[i] ? gl_state_->enable : gl_state_->disable)(GL_TEXTURE_2D);
    }
    gl_state_->active_texture(active_unit);
    gl_state_->use_program(prev_prog);
    gl_state_->viewport(prev_view[0], prev_view[1], prev_view[2], prev_view[3]);
    (blend_on ? gl_state_->enable : gl_state_->disable)(GL_BLEND);
    (scissor_on ? gl_state_->enable : gl_state_->disable)(GL_SCISSOR_TEST);
    if (!ready) gl_state_->state = ShaderState::kNone;
    return ready;
}

CanvasPoint CrtShader::Unwarp(const FrameGeometry& geom, double x, double y) const {
    double frame_w = static_cast<double>(geom.frame_ui_w) * geom.scale;
    double frame_h = static_cast<double>(geom.frame_ui_h) * geom.scale;
    if (frame_w <= 0.0 || frame_h <= 0.0) return {x, y};
    double norm_x = (x - geom.frame_x) / frame_w * 2.0 - 1.0;
    double norm_y = (y - geom.frame_y) / frame_h * 2.0 - 1.0;
    double can_x = norm_x * (1.0 + kBowX * norm_y * norm_y);
    double can_y = norm_y * (1.0 + kBowY * norm_x * norm_x);
    return {geom.frame_x + (can_x + 1.0) / 2.0 * frame_w,
            geom.frame_y + (can_y + 1.0) / 2.0 * frame_h};
}

void CrtShader::Present(SDL_Renderer* sdl_renderer, SDL_Texture* canvas,
                        const FrameGeometry& geom) {
    int frame_x = geom.frame_x, frame_y = geom.frame_y;
    int frame_w = geom.frame_ui_w * geom.scale, frame_h = geom.frame_ui_h * geom.scale;
    SDL_SetTextureBlendMode(canvas, SDL_BLENDMODE_NONE);

    SDL_Rect frame{frame_x, frame_y, frame_w, frame_h};
    EnsureTexture(sdl_renderer, half_, frame_w / 2, frame_h / 2, SDL_TEXTUREACCESS_TARGET);
    EnsureTexture(sdl_renderer, quarter_, frame_w / 4, frame_h / 4, SDL_TEXTUREACCESS_TARGET);
    EnsureTexture(sdl_renderer, eighth_, frame_w / 8, frame_h / 8, SDL_TEXTUREACCESS_TARGET);
    bool glow = half_.tex != nullptr && quarter_.tex != nullptr && eighth_.tex != nullptr;
    if (glow) {
        SDL_RenderSetClipRect(sdl_renderer, nullptr);
        SDL_SetRenderTarget(sdl_renderer, half_.tex);
        SDL_RenderCopy(sdl_renderer, canvas, &frame, nullptr);
        SDL_SetTextureBlendMode(half_.tex, SDL_BLENDMODE_NONE);
        SDL_SetRenderTarget(sdl_renderer, quarter_.tex);
        SDL_RenderCopy(sdl_renderer, half_.tex, nullptr, nullptr);
        SDL_SetTextureBlendMode(quarter_.tex, SDL_BLENDMODE_NONE);
        SDL_SetRenderTarget(sdl_renderer, eighth_.tex);
        SDL_RenderCopy(sdl_renderer, quarter_.tex, nullptr, nullptr);
    }

    SDL_SetRenderTarget(sdl_renderer, nullptr);
    SDL_RenderSetClipRect(sdl_renderer, nullptr);
    int out_w = 0, out_h = 0;
    SDL_GetRendererOutputSize(sdl_renderer, &out_w, &out_h);
    double ratio_x = static_cast<double>(out_w) / std::max(1, geom.canvas_w);
    double ratio_y = static_cast<double>(out_h) / std::max(1, geom.canvas_h);
    double row_px = geom.scale * ratio_y;
    bool whole = row_px >= 2.0 && std::abs(row_px - std::round(row_px)) < 0.01;
    double strength = kScanline * (whole ? 1.0 : std::clamp((row_px - 1.5) / 1.5, 0.0, 1.0));
    int stripe_w = std::max(1, static_cast<int>(std::lround(geom.dpi * geom.shrink)));
    if (glow && RenderThroughGlShader(sdl_renderer, canvas, geom, out_w, out_h, ratio_x, ratio_y,
                                      row_px, strength, static_cast<double>(stripe_w))) {
        return;
    }

    SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);
    SDL_SetRenderDrawBlendMode(sdl_renderer, SDL_BLENDMODE_NONE);
    SDL_RenderClear(sdl_renderer);
    SDL_RenderCopy(sdl_renderer, canvas, nullptr, nullptr);
    SDL_Rect on_window{static_cast<int>(std::floor(frame_x * ratio_x)),
                       static_cast<int>(std::floor(frame_y * ratio_y)),
                       static_cast<int>(std::ceil(frame_w * ratio_x)) + 1,
                       static_cast<int>(std::ceil(frame_h * ratio_y)) + 1};
    SDL_RenderFillRect(sdl_renderer, &on_window);

    std::vector<SDL_Vertex> on_canvas, on_frame, on_lines;
    std::vector<int> indices;
    SDL_RendererInfo info{};
    SDL_GetRendererInfo(sdl_renderer, &info);
    int max_h = info.max_texture_height > 0 ? info.max_texture_height : 8192;
    int row_texels = std::clamp(static_cast<int>(std::lround(row_px)), 2, 8);
    row_texels = std::max(2, std::min(row_texels, max_h / std::max(1, geom.frame_ui_h)));

    for (int j = 0; j <= kGridY; j++) {
        for (int i = 0; i <= kGridX; i++) {
            double norm_u = static_cast<double>(i) / kGridX;
            double norm_v = static_cast<double>(j) / kGridY;
            auto [scr_x, scr_y] = InverseBow(norm_u * 2.0 - 1.0, norm_v * 2.0 - 1.0);
            SDL_Vertex vert{};
            vert.position = {
                static_cast<float>((frame_x + (scr_x + 1.0) / 2.0 * frame_w) * ratio_x),
                static_cast<float>((frame_y + (scr_y + 1.0) / 2.0 * frame_h) * ratio_y)};
            vert.color = {255, 255, 255, 255};
            vert.tex_coord = {static_cast<float>((frame_x + norm_u * frame_w) / geom.canvas_w),
                              static_cast<float>((frame_y + norm_v * frame_h) / geom.canvas_h)};
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

    DrawWarpedMesh(sdl_renderer, canvas, on_canvas, indices, SDL_BLENDMODE_NONE);

    if (EnsureTexture(sdl_renderer, lines_, 1, geom.frame_ui_h * row_texels,
                      SDL_TEXTUREACCESS_STATIC, row_texels * 1000.0 + strength)) {
        BuildLines(geom.frame_ui_h, row_texels, strength);
    }
    DrawWarpedMesh(sdl_renderer, lines_.tex, on_lines, indices, SDL_BLENDMODE_MOD);

    if (EnsureTexture(sdl_renderer, stripes_, out_w, 1, SDL_TEXTUREACCESS_STATIC, stripe_w)) {
        SDL_SetTextureScaleMode(stripes_.tex, SDL_ScaleModeNearest);
        BuildStripes(stripe_w);
    }
    if (stripes_.tex != nullptr) {
        SDL_Rect src{0, 0, std::min(on_window.w, stripes_.width), 1}, dst = on_window;
        dst.w = src.w;
        SDL_SetTextureBlendMode(stripes_.tex, SDL_BLENDMODE_MOD);
        SDL_RenderCopy(sdl_renderer, stripes_.tex, &src, &dst);
    }

    if (glow) {
        DrawWarpedMesh(sdl_renderer, quarter_.tex, on_frame, indices, SDL_BLENDMODE_ADD, kGlowNear);
        DrawWarpedMesh(sdl_renderer, eighth_.tex, on_frame, indices, SDL_BLENDMODE_ADD, kGlowFar);
    }

    double aspect = frame_h > 0 ? static_cast<double>(frame_w) / frame_h : 4.0 / 3.0;
    int shade_h = std::clamp(static_cast<int>(std::lround(256.0 / aspect)), 64, 512);
    if (EnsureTexture(sdl_renderer, shade_, 256, shade_h, SDL_TEXTUREACCESS_STATIC, aspect)) {
        BuildShade(aspect);
    }
    DrawWarpedMesh(sdl_renderer, shade_.tex, on_frame, indices, SDL_BLENDMODE_MOD);
}

}  // namespace crt
