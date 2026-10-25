#include "runtime/desktop_video.h"
void gow2DesktopKeys();
void gow2DesktopSnapshot();
int gow2DesktopMode();
// GOW-Port: contexto host, selección de frames y dibujo raylib usados por el port.
#include "runtime/gs/gs_host_present.h"
#include "rlgl.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_perf.h"
#include "runtime/ps2_memory.h"
#include "ps2_log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

void ps2WindowDisableKeyMenu(void *windowHandle); // ps2_window_win.cpp

namespace
{
    constexpr uint32_t kWidth = 640u;
    constexpr uint32_t kHeight = 512u;
    constexpr uint32_t kDisplayHeight = 448u;
}


namespace
{
    // GOW-Port: output scaler. The GS renders at its native resolution (512x448); this pass maps that image onto the window
    // at the window's resolution with a 5-tap Catmull-Rom bicubic (GOW_SCALER=sharp, default) instead of the plain
    // bilinear stretch (GOW_SCALER=bilinear), then applies a light contrast-adaptive sharpen. It does not add detail
    // (that needs a scaled GS render target); it removes the blur of the stretch.
    const char *kScalerFragment = R"GLSL(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 srcSize;
uniform float sharpen;
uniform float plain;
out vec4 finalColor;

vec4 tex(vec2 uv) { return texture(texture0, uv); }

void main()
{
    if (plain > 0.5)
    {
        // frame at least as large as its window rectangle: one bilinear tap, no bicubic / sharpen
        finalColor = vec4(texture(texture0, fragTexCoord).rgb, 1.0);
        return;
    }
    vec2 pos = fragTexCoord * srcSize;
    vec2 center = floor(pos - 0.5) + 0.5;
    vec2 f = pos - center;
    vec2 f2 = f * f, f3 = f2 * f;
    vec2 w0 = -0.5 * f3 + f2 - 0.5 * f;
    vec2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    vec2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
    vec2 w3 = 0.5 * f3 - 0.5 * f2;
    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;
    vec2 tc0 = (center - 1.0) / srcSize;
    vec2 tc3 = (center + 2.0) / srcSize;
    vec2 tc12 = (center + offset12) / srcSize;
    vec4 c = tex(vec2(tc12.x, tc0.y)) * (w12.x * w0.y)
           + tex(vec2(tc0.x, tc12.y)) * (w0.x * w12.y)
           + tex(vec2(tc12.x, tc12.y)) * (w12.x * w12.y)
           + tex(vec2(tc3.x, tc12.y)) * (w3.x * w12.y)
           + tex(vec2(tc12.x, tc3.y)) * (w12.x * w3.y);
    float norm = (w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) + (w3.x * w12.y) + (w12.x * w3.y);
    c /= norm;
    // contrast-adaptive sharpen from the four direct neighbours of the nearest source texel
    vec2 px = 1.0 / srcSize;
    vec3 n = tex(fragTexCoord + vec2(0.0, -px.y)).rgb, s = tex(fragTexCoord + vec2(0.0, px.y)).rgb;
    vec3 e = tex(fragTexCoord + vec2(px.x, 0.0)).rgb, w = tex(fragTexCoord + vec2(-px.x, 0.0)).rgb;
    vec3 lo = min(min(n, s), min(e, w)), hi = max(max(n, s), max(e, w));
    vec3 amp = clamp(min(lo, 1.0 - hi) / max(hi, vec3(0.0001)), 0.0, 1.0);
    float peak = -1.0 / mix(8.0, 5.0, sharpen);
    vec3 wgt = sqrt(amp) * peak;
    vec3 sharp = (c.rgb + (n + s + e + w) * wgt) / (1.0 + 4.0 * wgt);
    finalColor = vec4(clamp(sharp, 0.0, 1.0), 1.0);
}
)GLSL";

    Shader g_scaler{};
    int g_scalerSrcSize = -1, g_scalerSharpen = -1, g_scalerPlain = -1;
    bool g_scalerTried = false;

    bool scalerEnabled()
    {
        static const bool on = [] { const char *v = std::getenv("GOW_SCALER"); return !(v && std::strcmp(v, "bilinear") == 0); }();
        return on;
    }

    Shader *scalerShader()
    {
        if (!g_scalerTried)
        {
            g_scalerTried = true;
            if (scalerEnabled())
            {
                g_scaler = LoadShaderFromMemory(nullptr, kScalerFragment);
                if (g_scaler.id != 0u && g_scaler.id != rlGetShaderIdDefault())
                {
                    g_scalerSrcSize = GetShaderLocation(g_scaler, "srcSize");
                    g_scalerSharpen = GetShaderLocation(g_scaler, "sharpen");
                    g_scalerPlain = GetShaderLocation(g_scaler, "plain");
                }
                else
                {
                    std::fprintf(stderr, "[gs-present] scaler shader failed, using bilinear\n");
                    g_scaler = {};
                }
            }
        }
        return g_scaler.id != 0u ? &g_scaler : nullptr;
    }

    // GoW2-Port: performance overlay (F10), after PCSX2's PerformanceMetrics + ImGuiManager::DrawPerformanceOverlay
    // (PCSX2, GPLv3+): the metrics are recomputed every 0.5 s; VPS = guest vertical blanks per second (with a 16 sample
    // average), speed = VPS against the NTSC rate, frame times from the host frames. FPS is PCSX2's internal FPS: game
    // frame flips (a new display buffer, FBP of DISPFB1/2) per second, "[P]"; a game that never flips falls back to the
    // new frames handed to the GS, "[B]" (PCSX2's framebuffer blits). When the game's override names its per-frame
    // function (ps2_perf::setGameFramePc), FPS counts its entries instead, "[G]". Drawn top right with a drop shadow; the speed line
    // turns red below 95 % and green above 105 %, as in PCSX2.
    struct PerfOverlay
    {
        static constexpr double kUpdateInterval = 0.5; // seconds, PCSX2's UPDATE_INTERVAL
        static constexpr double kNtscRate = 59.94;
        static constexpr size_t kAvgSamples = 16;      // PCSX2's FPS_BUFFER_SIZE

        bool visible = [] { const char *v = std::getenv("GOW_SHOW_FPS"); return v && *v == '1'; }();
        uint64_t gameFrames = 0;   // new GS frames since the last update
        uint64_t flips = 0;        // display buffer changes since the last update
        uint64_t lastFbp = ~0ull;
        bool flipMethod = false;
        bool gameFrameMethod = false;   // FPS from the game's per-frame function (ps2_perf::gameFramePc)
        uint64_t lastGameFrameEntries = 0;
        uint64_t hostFrames = 0;
        uint64_t lastVsync = 0;
        bool vsyncKnown = false;
        double frameTimeSum = 0.0, frameTimeMin = 1e9, frameTimeMax = 0.0;
        std::chrono::steady_clock::time_point lastUpdate = std::chrono::steady_clock::now();
        std::array<double, kAvgSamples> vpsHistory{};
        size_t vpsCount = 0, vpsPos = 0;
        // last computed values
        double fps = 0.0, vps = 0.0, avgVps = 0.0, speed = 0.0, hostFps = 0.0, ftMin = 0.0, ftAvg = 0.0, ftMax = 0.0;

        void update(uint64_t vsyncTick, const GSRegisters *regs)
        {
            ++hostFrames;
            if (regs)
            {
                const uint64_t fbp = (regs->dispfb1 & 0x1FFu) | ((regs->dispfb2 & 0x1FFu) << 16);
                if (lastFbp != ~0ull && fbp != lastFbp)
                    ++flips;
                lastFbp = fbp;
            }
            const double frameMs = GetFrameTime() * 1000.0;
            frameTimeSum += frameMs;
            frameTimeMin = std::min(frameTimeMin, frameMs);
            frameTimeMax = std::max(frameTimeMax, frameMs);
            if (!vsyncKnown)
            {
                lastVsync = vsyncTick;
                vsyncKnown = true;
            }
            const auto now = std::chrono::steady_clock::now();
            const double time = std::chrono::duration<double>(now - lastUpdate).count();
            if (time < kUpdateInterval)
                return;
            lastUpdate = now;
            vps = static_cast<double>(vsyncTick - lastVsync) / time;
            lastVsync = vsyncTick;
            flipMethod = flips != 0;
            const uint64_t entries = ps2_perf::gameFrameEntries.load(std::memory_order_relaxed);
            gameFrameMethod = ps2_perf::gameFramePc.load(std::memory_order_relaxed) != 0u;
            const uint64_t gameFunctionFrames = entries - lastGameFrameEntries;
            lastGameFrameEntries = entries;
            fps = static_cast<double>(gameFrameMethod ? gameFunctionFrames : flipMethod ? flips : gameFrames) / time;
            hostFps = static_cast<double>(hostFrames) / time;
            speed = vps / kNtscRate * 100.0;
            ftAvg = frameTimeSum / static_cast<double>(std::max<uint64_t>(hostFrames, 1u));
            ftMin = frameTimeMin;
            ftMax = frameTimeMax;
            // PCSX2's AvgFPS: 16 samples, restarted when the rate jumps by more than 20 %
            if (vpsCount != 0 && avgVps > 0.0 && std::fabs(vps - avgVps) > avgVps * 0.2)
                vpsCount = vpsPos = 0;
            vpsHistory[vpsPos] = vps;
            vpsPos = (vpsPos + 1) % kAvgSamples;
            vpsCount = std::min(vpsCount + 1, kAvgSamples);
            double sum = 0.0;
            for (size_t i = 0; i < vpsCount; ++i)
                sum += vpsHistory[i];
            avgVps = sum / static_cast<double>(vpsCount);
            gameFrames = hostFrames = flips = 0;
            frameTimeSum = frameTimeMax = 0.0;
            frameTimeMin = 1e9;
        }

        void draw() const
        {
            if (!visible)
                return;
            const float fontSize = std::max(20.0f, GetScreenHeight() / 40.0f);
            const float spacing = fontSize / 10.0f, margin = fontSize * 0.5f, shadow = std::ceil(fontSize / 20.0f);
            const Font font = GetFontDefault();
            const Color white{255, 255, 255, 255};
            const Color speedColor = speed < 95.0 ? Color{255, 100, 100, 255} : speed > 105.0 ? Color{100, 255, 100, 255} : white;
            char lines[4][96];
            std::snprintf(lines[0], sizeof(lines[0]), "FPS: %.2f %s", fps, gameFrameMethod ? "[G]" : flipMethod ? "[P]" : "[B]");
            std::snprintf(lines[1], sizeof(lines[1]), "VPS: %.2f (Avg. %.2f)", vps, avgVps);
            std::snprintf(lines[2], sizeof(lines[2]), "Speed: %u%%", static_cast<unsigned>(std::lround(speed)));
            std::snprintf(lines[3], sizeof(lines[3]), "Host: %.0f FPS | Min: %.2fms | Avg: %.2fms | Max: %.2fms", hostFps, ftMin, ftAvg, ftMax);
            float y = margin + fontSize * 0.5f;
            for (int i = 0; i < 4; ++i)
            {
                const Vector2 size = MeasureTextEx(font, lines[i], fontSize, spacing);
                const Vector2 pos{GetScreenWidth() - margin - size.x, y};
                DrawTextEx(font, lines[i], Vector2{pos.x + shadow, pos.y + shadow}, fontSize, spacing, Color{0, 0, 0, 100});
                DrawTextEx(font, lines[i], pos, fontSize, spacing, i == 2 ? speedColor : white);
                y += size.y + spacing;
            }
        }
    };

    PerfOverlay g_perfOverlay;
}

void GSHostPresentation::captureHostContext()
{
    if (started)
        return;
    started = true;
    const char *setting = std::getenv("PS2X_GS_DIRECT_PRESENT");
    // GOW-Port: shared GL textures (no readback/upload) by default; PS2X_GS_DIRECT_PRESENT=0 forces RAM frames.
    captureRequested = !(setting && *setting == '0');
    if (captureRequested)
        GSSharedPresent::CaptureHostContext();
}

void GSHostPresentation::restoreHostContext()
{
    if (captureRequested)
        GSSharedPresent::RestoreHostContext();
}

void GSHostPresentation::createFallback()
{
    if (fallback.id != 0u)
        return;
    Image blank = GenImageColor(kWidth, kHeight, BLANK);
    fallback = LoadTextureFromImage(blank);
    UnloadImage(blank);
    SetTextureFilter(fallback, TEXTURE_FILTER_POINT); // Coincide con la ruta RAM original.
    upload.resize(static_cast<size_t>(kWidth) * kHeight * 4u, 0u);
}

void GSHostPresentation::blankFallback()
{
    if (!uploadedBlank)
    {
        for (size_t offset = 0u; offset < upload.size(); offset += 4u)
        {
            upload[offset] = MAGENTA.r;
            upload[offset + 1u] = MAGENTA.g;
            upload[offset + 2u] = MAGENTA.b;
            upload[offset + 3u] = MAGENTA.a;
        }
        UpdateTexture(fallback, upload.data());
        uploadedBlank = true;
    }
    current.texture = fallback;
    current.width = kWidth;
    current.height = kDisplayHeight;
    current.borrowed = false;
    current.blank = true;
    uploaded = true;
}

GSHostPresentation::View GSHostPresentation::prepare(GS &gs, uint64_t vsyncTick)
{
    createFallback();
    const bool shared = gs.usesSharedPresentation() && GSSharedPresent::Active();
    if (!modeKnown || shared != sharedMode)
    {
        std::fprintf(stderr, "[gs-present] host mode: %s\n", shared ? "shared texture" : "RAM frames");
        std::fflush(stderr);
        modeKnown = true;
        sharedMode = shared;
        hasLatched = uploaded = uploadedBlank = false;
        lastSharedSequence = 0u;
        current = {};
    }
    const bool needsLatch = !hasLatched || vsyncTick != lastTick;
    if (needsLatch)
    {
        gs.latchHostPresentationFrame();
        lastTick = vsyncTick;
        hasLatched = true;
    }
    current.shared = shared;
    if (shared)
    {
        // GOW-Port: la llamada anterior ya terminó EndDrawing, incluido el batch.
        // Acquire puede conservar el slot previo; nunca descargar píxeles aquí.
        GSSharedPresent::Frame frame;
        if (GSSharedPresent::Acquire(frame))
        {
            current.frame = frame;
            if (frame.sequence != lastSharedSequence)
            {
                gs.recordAcquiredSharedFrame(frame);
                lastSharedSequence = frame.sequence;
                ++g_perfOverlay.gameFrames;
            }
            if (!frame.blank && frame.texture != 0u && frame.width != 0u && frame.height != 0u)
            {
                current.texture = Texture2D{frame.texture, static_cast<int>(frame.width), static_cast<int>(frame.height),
                                            1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                current.width = frame.width;
                current.height = frame.height;
                current.borrowed = true;
                current.blank = false;
                return current;
            }
        }
        blankFallback(); // PMODE apagado conserva el placeholder magenta de RAM.
        return current;
    }
    if (!needsLatch && uploaded)
        return current;

    scratch.clear();
    uint32_t width = 0u, height = 0u, displayFbp = 0u, sourceFbp = 0u;
    bool preferred = false;
    if (!gs.copyLatchedHostPresentationFrame(scratch, width, height, &displayFbp, &sourceFbp, &preferred))
    {
        blankFallback();
        lastWidth = current.width;
        lastHeight = current.height;
        return current;
    }
    PS2_IF_AGRESSIVE_LOGS({
        if (uploadDebugCount < 128u || displayFbp != lastDisplayFbp || sourceFbp != lastSourceFbp ||
            preferred != lastPreferred || width != lastWidth || height != lastHeight)
            std::cout << "[frame:upload] idx=" << uploadDebugCount << " tick=" << vsyncTick
                      << " displayFbp=" << displayFbp << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(preferred ? 1u : 0u) << std::endl;
        ++uploadDebugCount;
    });
    ++g_perfOverlay.gameFrames; // RAM frames: one per latched vertical blank that produced a frame
    lastDisplayFbp = displayFbp;
    lastSourceFbp = sourceFbp;
    lastPreferred = preferred;
    lastWidth = width;
    lastHeight = height;
    if (width > kWidth || height > kHeight)
    {
        // GOW-Port: render-target renderer frames (internal resolution) are uploaded as they are, in a texture of their size.
        if (width > fallbackWidth || height > fallbackHeight)
        {
            UnloadTexture(fallback);
            fallbackWidth = std::max(fallbackWidth, width);
            fallbackHeight = std::max(fallbackHeight, height);
            Image bigBlank = GenImageColor(static_cast<int>(fallbackWidth), static_cast<int>(fallbackHeight), BLANK);
            fallback = LoadTextureFromImage(bigBlank);
            UnloadImage(bigBlank);
            SetTextureFilter(fallback, TEXTURE_FILTER_POINT);
        }
        UpdateTextureRec(fallback, Rectangle{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)}, scratch.data());
        uploaded = true;
        uploadedBlank = false;
        current.texture = fallback;
        current.width = width;
        current.height = height;
        current.borrowed = current.blank = false;
        current.frame = {};
        return current;
    }
    std::fill(upload.begin(), upload.end(), 0u);
    const uint32_t copyWidth = std::min(width, kWidth);
    const uint32_t copyHeight = std::min(height, kHeight);
    const size_t sourceRow = static_cast<size_t>(width) * 4u;
    const size_t destinationRow = static_cast<size_t>(kWidth) * 4u;
    const size_t copyRow = static_cast<size_t>(copyWidth) * 4u;
    for (uint32_t y = 0u; y < copyHeight; ++y)
    {
        const size_t sourceOffset = static_cast<size_t>(y) * sourceRow;
        const size_t destinationOffset = static_cast<size_t>(y) * destinationRow;
        if (sourceOffset + copyRow > scratch.size() || destinationOffset + copyRow > upload.size())
            break;
        std::memcpy(upload.data() + destinationOffset, scratch.data() + sourceOffset, copyRow);
    }
    UpdateTexture(fallback, upload.data());
    uploaded = true;
    uploadedBlank = false;
    current.texture = fallback;
    current.width = width;
    current.height = height;
    current.borrowed = current.blank = false;
    current.frame = {};
    return current;
}

GSHostPresentation::View GSHostPresentation::renderFrame(GS &gs, uint64_t vsyncTick, const std::function<void()> &overlay)
{
    // GoW2-Port: F12 toggles borderless full screen / window (does not change the monitor's mode), F10 the performance
    // overlay (GOW_SHOW_FPS=1 shows it from the start)
    static const bool keyMenuDisabled = [] { ps2WindowDisableKeyMenu(GetWindowHandle()); return true; }();
    (void)keyMenuDisabled;
    gow2DesktopKeys();
    if (IsKeyPressed(KEY_F10))
        g_perfOverlay.visible = !g_perfOverlay.visible;
    View selected;
    {
        ps2_perf::Scope perf(ps2_perf::Bucket::Upload);
        selected = prepare(gs, vsyncTick);
    }
    g_perfOverlay.update(vsyncTick, gs.privilegedRegisters());
    BeginDrawing();
    ClearBackground(BLACK);
    const float sourceWidth = static_cast<float>(std::max<uint32_t>(1u, selected.width));
    const float sourceHeight = static_cast<float>(std::max<uint32_t>(1u, selected.height));
    SetTextureFilter(selected.texture, TEXTURE_FILTER_BILINEAR);
    desktop_video::draw(selected.texture, selected.width, selected.height, false, gow2DesktopMode());
    Shader *scaler = nullptr;
    if (overlay)
        overlay();
    g_perfOverlay.draw(); // last: above the debug UI
    gow2DesktopSnapshot();
    // GOW-Port: GOW_HOST_SHOT=<n>: at host frame n write the window's back buffer (what is about to be shown) to
    // host_shot.png in the working directory, plus the frame / window sizes, to check presentation without a desktop grab.
    static const long hostShotAt = [] { const char *v = std::getenv("GOW_HOST_SHOT"); return v ? std::atol(v) : -1L; }();
    static long hostFrame = 0;
    if (hostShotAt >= 0 && hostFrame++ == hostShotAt)
    {
        rlDrawRenderBatchActive();
        Image screen = LoadImageFromScreen();
        ExportImage(screen, "host_shot.png");
        UnloadImage(screen);
        std::fprintf(stderr, "[gs-present] host_shot frame=%ux%u tex=%u window=%dx%d shared=%d scaler=%d\n", selected.width, selected.height,
                     selected.texture.id, GetScreenWidth(), GetScreenHeight(), selected.shared ? 1 : 0, scaler ? 1 : 0);
    }
    {
        ps2_perf::Scope perf(ps2_perf::Bucket::HostWait);
        EndDrawing();
    }
    return selected;
}

void GSHostPresentation::shutdown(GS &gs)
{
    if (!started && fallback.id == 0u)
        return;
    // GOW-Port: nullptr instala un backend nulo; drena y destruye el worker/contexto compartido.
    gs.setRasterBackend(nullptr);
    restoreHostContext();
    if (fallback.id != 0u)
        UnloadTexture(fallback); // Nunca descargar la Texture2D prestada del productor.
    fallback = {};
    if (captureRequested)
        GSSharedPresent::ShutdownHost();
    started = captureRequested = hasLatched = uploaded = uploadedBlank = modeKnown = sharedMode = false;
    lastTick = lastSharedSequence = 0u;
    current = {};
    scratch.clear();
    upload.clear();
}
