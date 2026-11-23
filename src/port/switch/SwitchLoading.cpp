#ifdef __SWITCH__
#include "SwitchLoading.h"
#include "Switch2Debug.h"
#include "SwitchLoadingFont.h"

#include <SDL2/SDL.h>
#include <glad/glad.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
constexpr uint32_t kMinFrameMs = 33;

struct Color {
    float r, g, b, a;
};
constexpr Color kBackground = { 0.043f, 0.051f, 0.071f, 1.0f };
constexpr Color kTitle = { 0.95f, 0.96f, 0.98f, 1.0f };
constexpr Color kTrack = { 0.165f, 0.184f, 0.227f, 1.0f };
constexpr Color kFill = { 0.91f, 0.93f, 0.96f, 1.0f };
constexpr Color kLabel = { 0.66f, 0.69f, 0.74f, 1.0f };

struct Vertex {
    float x, y, u, v;
    Color color;
};

const char* const kVertexShader = R"(#version 410 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
uniform vec2 uScreen;
out vec2 vUv;
out vec4 vColor;
void main() {
    vUv = aUv;
    vColor = aColor;
    gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0, 0.0, 1.0);
})";
const char* const kFragmentShader = R"(#version 410 core
in vec2 vUv;
in vec4 vColor;
uniform sampler2D uAtlas;
out vec4 outColor;
void main() {
    outColor = vec4(vColor.rgb, vColor.a * texture(uAtlas, vUv).r);
})";

const char* sLabel = "Loading";
float sStart = 0.0f;
float sEnd = 0.0f;
float sTarget = 0.0f;
float sShown = 0.0f;
bool sFinished = false;
bool sGlFailed = false;
uint32_t sLastDraw = 0;
uint32_t sLongestGap = 0;
uint32_t sFrames = 0;
GLuint sProgram = 0;
GLuint sVao = 0;
GLuint sVbo = 0;
GLuint sTexture = 0;
GLint sScreenLocation = -1;
GLint sAtlasLocation = -1;
std::vector<Vertex> sVertices;

GLuint CompileStage(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool CreateResources() {
    GLuint vs = CompileStage(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kFragmentShader);
    if (vs == 0 || fs == 0) {
        return false;
    }
    sProgram = glCreateProgram();
    glAttachShader(sProgram, vs);
    glAttachShader(sProgram, fs);
    glLinkProgram(sProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(sProgram, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        glDeleteProgram(sProgram);
        sProgram = 0;
        return false;
    }
    sScreenLocation = glGetUniformLocation(sProgram, "uScreen");
    sAtlasLocation = glGetUniformLocation(sProgram, "uAtlas");

    glGenVertexArrays(1, &sVao);
    glGenBuffers(1, &sVbo);
    glBindVertexArray(sVao);
    glBindBuffer(GL_ARRAY_BUFFER, sVbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, x)));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, u)));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, color)));

    GLint alignment = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &sTexture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kLoadingAtlasWidth, kLoadingAtlasHeight, 0, GL_RED, GL_UNSIGNED_BYTE,
                 kLoadingAtlas);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    return true;
}

void Quad(float x, float y, float w, float h, float ax, float ay, float aw, float ah, Color color) {
    const float u0 = ax / kLoadingAtlasWidth;
    const float v0 = ay / kLoadingAtlasHeight;
    const float u1 = (ax + aw) / kLoadingAtlasWidth;
    const float v1 = (ay + ah) / kLoadingAtlasHeight;
    const Vertex a = { x, y, u0, v0, color };
    const Vertex b = { x + w, y, u1, v0, color };
    const Vertex c = { x + w, y + h, u1, v1, color };
    const Vertex d = { x, y + h, u0, v1, color };
    sVertices.insert(sVertices.end(), { a, b, c, a, c, d });
}

// The atlas has a solid block in its top left corner.
void Rect(float x, float y, float w, float h, Color color) {
    if (w > 0.0f && h > 0.0f) {
        Quad(x, y, w, h, 1.0f, 1.0f, 2.0f, 2.0f, color);
    }
}

const LoadingGlyph* FindGlyph(char c) {
    for (const auto& glyph : kLoadingGlyphs) {
        if (glyph.c == c) {
            return &glyph;
        }
    }
    return nullptr;
}

float TextWidth(const char* text, float scale) {
    float width = 0.0f;
    for (; *text != '\0'; text++) {
        const LoadingGlyph* glyph = FindGlyph(*text);
        width += glyph != nullptr ? glyph->advance * scale : 0.0f;
    }
    return width;
}

// y is the baseline. Returns the x after the text.
float Text(const char* text, float x, float y, float scale, Color color) {
    for (; *text != '\0'; text++) {
        const LoadingGlyph* glyph = FindGlyph(*text);
        if (glyph == nullptr) {
            continue;
        }
        if (glyph->w > 0 && glyph->h > 0) {
            Quad(std::round(x + glyph->left * scale), std::round(y + glyph->top * scale), glyph->w * scale,
                 glyph->h * scale, glyph->x, glyph->y, glyph->w, glyph->h, color);
        }
        x += glyph->advance * scale;
    }
    return x;
}

void BuildScreen(int width, int height, uint32_t now) {
    sVertices.clear();
    const float ui = height / 1080.0f;
    const float titleW = kLoadingTitle[2] * ui;
    const float titleH = kLoadingTitle[3] * ui;
    const float barW = std::min(width * 0.46f, 860.0f * ui);
    const float barH = std::max(3.0f, std::round(6.0f * ui));
    const float barX = std::round((width - barW) / 2.0f);
    const float barY = std::round(height * 0.54f);
    const float textScale = 0.82f * ui;
    const float baseline = barY + barH + std::round(42.0f * ui);

    Quad(std::round((width - titleW) / 2.0f), std::round(barY - titleH - 58.0f * ui), titleW, titleH, kLoadingTitle[0],
         kLoadingTitle[1], kLoadingTitle[2], kLoadingTitle[3], kTitle);
    Rect(barX, barY, barW, barH, kTrack);
    Rect(barX, barY, std::round(barW * sShown), barH, kFill);

    float x = Text(sLabel, barX, baseline, textScale, kLabel);
    // The dots show it is working. The bar only ever shows real progress.
    const float phase = (now % 1200) / 1200.0f * 3.0f;
    for (int i = 0; i < 3; i++) {
        const float distance = std::min(std::fabs(phase - i), 3.0f - std::fabs(phase - i));
        Color dot = kLabel;
        dot.a = 0.25f + 0.75f * std::max(0.0f, 1.0f - distance);
        x = Text(".", x, baseline, textScale, dot);
    }

    char percent[8];
    snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(sTarget * 100.0f));
    Text(percent, barX + barW - TextWidth(percent, textScale), baseline, textScale, kLabel);
}

void Draw(uint32_t now) {
    SDL_Window* window = SDL_GL_GetCurrentWindow();
    if (window == nullptr) {
        return;
    }
    int width = 0;
    int height = 0;
    SDL_GL_GetDrawableSize(window, &width, &height);
    if (width <= 0 || height <= 0) {
        return;
    }

    GLint framebuffer, program, vao, arrayBuffer, activeTexture, texture0, blendSrcRgb, blendDstRgb, blendSrcAlpha,
        blendDstAlpha;
    GLint viewport[4];
    GLint scissorBox[4];
    GLfloat clearColor[4];
    GLboolean colorMask[4];
    GLboolean depthMask;
    const GLboolean blendOn = glIsEnabled(GL_BLEND);
    const GLboolean depthOn = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cullOn = glIsEnabled(GL_CULL_FACE);
    const GLboolean scissorOn = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean stencilOn = glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);

    if (sProgram == 0 && !sGlFailed) {
        sGlFailed = !CreateResources();
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    glClearColor(kBackground.r, kBackground.g, kBackground.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!sGlFailed) {
        BuildScreen(width, height, now);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(sProgram);
        glUniform2f(sScreenLocation, static_cast<float>(width), static_cast<float>(height));
        glUniform1i(sAtlasLocation, 0);
        glBindVertexArray(sVao);
        glBindBuffer(GL_ARRAY_BUFFER, sVbo);
        glBindTexture(GL_TEXTURE_2D, sTexture);
        glBufferData(GL_ARRAY_BUFFER, sVertices.size() * sizeof(Vertex), sVertices.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(sVertices.size()));
    }

    SDL_GL_SwapWindow(window);
    sFrames++;

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, arrayBuffer);
    glUseProgram(program);
    glBindTexture(GL_TEXTURE_2D, texture0);
    glActiveTexture(activeTexture);
    glBlendFuncSeparate(blendSrcRgb, blendDstRgb, blendSrcAlpha, blendDstAlpha);
    blendOn ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    depthOn ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    cullOn ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    scissorOn ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
    stencilOn ? glEnable(GL_STENCIL_TEST) : glDisable(GL_STENCIL_TEST);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
    glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    glDepthMask(depthMask);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
}
} // namespace

extern "C" void SwitchLoading_Update(int done, int total) {
    // The GL context is only current on the main thread.
    if (sFinished || glad_glClear == nullptr || SDL_GL_GetCurrentContext() == nullptr) {
        return;
    }
    const float inner = total > 0 ? std::min(1.0f, static_cast<float>(done) / total) : 0.0f;
    sTarget = std::max(sTarget, sStart + (sEnd - sStart) * inner);
    const uint32_t now = SDL_GetTicks();
    if (sLastDraw != 0 && now - sLastDraw < kMinFrameMs) {
        return;
    }
    if (sLastDraw != 0) {
        sLongestGap = std::max(sLongestGap, now - sLastDraw);
    }
    sLastDraw = now;
    sShown += (sTarget - sShown) * 0.4f;
    Draw(now);
}

extern "C" void SwitchLoading_Stage(const char* label, float start, float end) {
    sLabel = label;
    sStart = start;
    sEnd = end;
    SwitchLoading_Update(0, 1);
}

extern "C" void SwitchLoading_Finish(void) {
    if (sFinished) {
        return;
    }
    sStart = sEnd = 1.0f;
    sTarget = 1.0f;
    sShown = 1.0f;
    sLastDraw = 0;
    SwitchLoading_Update(1, 1);
    sFinished = true;
    if (SDL_GL_GetCurrentContext() != nullptr) {
        if (sProgram != 0) {
            glDeleteProgram(sProgram);
        }
        if (sVbo != 0) {
            glDeleteBuffers(1, &sVbo);
        }
        if (sVao != 0) {
            glDeleteVertexArrays(1, &sVao);
        }
        if (sTexture != 0) {
            glDeleteTextures(1, &sTexture);
        }
    }
    std::vector<Vertex>().swap(sVertices);
    char line[96];
    snprintf(line, sizeof(line), "B loading screen frames=%u longest-gap-ms=%u", sFrames, sLongestGap);
    Switch2Debug_Log(line);
}
#endif
