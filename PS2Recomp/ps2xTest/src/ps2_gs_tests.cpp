#include "MiniTest.h"
#include "runtime/ps2_memory.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "Stubs/Helpers/Support.h"
#include "Stubs/GS.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

using namespace ps2_syscalls;

namespace
{
    std::atomic<uint32_t> g_gsSyncCallbackHits{0u};
    std::atomic<uint32_t> g_gsSyncCallbackLastTick{0u};
    std::atomic<int32_t> g_gsSyncFirstField{-1};
    std::atomic<int32_t> g_gsSyncSecondField{-1};
    std::atomic<uint32_t> g_gsSyncCallbackSp{0u};
    std::atomic<uint32_t> g_gsSyncCallbackGp{0u};
    std::atomic<uint32_t> g_gsSyncCallbackPrevious{0u};

    constexpr uint32_t kGsSyncWait0Pc = 0x0011F000u;
    constexpr uint32_t kGsSyncResume0Pc = 0x0011F010u;
    constexpr uint32_t kGsSyncWait1Pc = 0x0011F020u;
    constexpr uint32_t kGsSyncResume1Pc = 0x0011F030u;
    constexpr uint32_t kGsCallbackMainPc = 0x0011F040u;
    constexpr uint32_t kGsCallbackResumePc = 0x0011F050u;
    constexpr uint32_t kGsCallbackPc = 0x00120000u;
    constexpr uint32_t kGsCallbackGp = 0x0036A7F0u;
    constexpr uint32_t kGsCallbackCallerSp = 0x00123450u;

    static_assert(sizeof(GsImageMem) == 12, "GsImageMem size mismatch");

    void setRegU32(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    void setRegU64(R5900Context &ctx, int reg, uint64_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    uint32_t getRegU32Test(const R5900Context &ctx, int reg)
    {
        return ::getRegU32(&ctx, reg);
    }

    uint64_t getReturnU64(const R5900Context &ctx)
    {
        const uint64_t lo = static_cast<uint64_t>(getRegU32Test(ctx, 2));
        const uint64_t hi = static_cast<uint64_t>(getRegU32Test(ctx, 3));
        return lo | (hi << 32);
    }

    uint64_t makeGifTag(uint16_t nloop, uint8_t flg, uint8_t nreg, bool eop = true)
    {
        uint64_t tag = static_cast<uint64_t>(nloop & 0x7FFFu);
        if (eop)
            tag |= (1ull << 15);
        tag |= (static_cast<uint64_t>(flg & 0x3u) << 58);
        tag |= (static_cast<uint64_t>(nreg & 0xFu) << 60);
        return tag;
    }

    void appendU64(std::vector<uint8_t> &dst, uint64_t value)
    {
        const size_t pos = dst.size();
        dst.resize(pos + sizeof(uint64_t));
        std::memcpy(dst.data() + pos, &value, sizeof(uint64_t));
    }

    void appendGifAd(std::vector<uint8_t> &dst, uint64_t value, uint64_t reg)
    {
        appendU64(dst, value);
        appendU64(dst, reg);
    }

    template <typename Predicate>
    bool waitUntil(Predicate pred, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (pred())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        return pred();
    }

    void testGsSyncVCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        g_gsSyncCallbackLastTick.store(getRegU32(ctx, 4), std::memory_order_relaxed);
        g_gsSyncCallbackSp.store(getRegU32(ctx, 29), std::memory_order_relaxed);
        g_gsSyncCallbackGp.store(getRegU32(ctx, 28), std::memory_order_relaxed);
        g_gsSyncCallbackHits.fetch_add(1u, std::memory_order_relaxed);
        ctx->pc = 0u;
    }

    void testGsSyncWait0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = kGsSyncResume0Pc;
        ps2_stubs::sceGsSyncV(rdram, ctx, runtime);
    }

    void testGsSyncResume0(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        g_gsSyncFirstField.store(static_cast<int32_t>(getRegU32(ctx, 2)), std::memory_order_release);
        ctx->pc = kGsSyncWait1Pc;
    }

    void testGsSyncWait1(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = kGsSyncResume1Pc;
        ps2_stubs::sceGsSyncV(rdram, ctx, runtime);
    }

    void testGsSyncResume1(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_gsSyncSecondField.store(static_cast<int32_t>(getRegU32(ctx, 2)), std::memory_order_release);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void testGsCallbackMain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setRegU32(*ctx, 4, kGsCallbackPc);
        setRegU32(*ctx, 28, kGsCallbackGp);
        setRegU32(*ctx, 29, kGsCallbackCallerSp);
        ctx->pc = kGsCallbackResumePc;
        ps2_stubs::sceGsSyncVCallback(rdram, ctx, runtime);
        g_gsSyncCallbackPrevious.store(getRegU32(ctx, 2), std::memory_order_release);
        ps2_syscalls::WaitVSyncTick(rdram, ctx, runtime, -1);
    }

    void testGsCallbackResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void writeGsImageTest(uint8_t *rdram, uint32_t addr, const GsImageMem &image)
    {
        std::memcpy(rdram + addr, &image, sizeof(image));
    }

    void writeGsImageTest(std::vector<uint8_t> &rdram, uint32_t addr, const GsImageMem &image)
    {
        writeGsImageTest(rdram.data(), addr, image);
    }

    void writePSMT4Texel(std::vector<uint8_t> &vram, uint32_t tbp, uint32_t tbw, uint32_t x, uint32_t y, uint8_t index)
    {
        const uint32_t nibbleAddr = GSPSMT4::addrPSMT4(tbp, tbw, x, y);
        const uint32_t byteOff = nibbleAddr >> 1;
        uint8_t &packed = vram[byteOff];
        if ((nibbleAddr & 1u) != 0u)
        {
            packed = static_cast<uint8_t>((packed & 0x0Fu) | ((index & 0x0Fu) << 4));
        }
        else
        {
            packed = static_cast<uint8_t>((packed & 0xF0u) | (index & 0x0Fu));
        }
    }

    uint32_t referenceAddrPSMT4(uint32_t block, uint32_t width, uint32_t x, uint32_t y)
    {
        static constexpr uint8_t kBlockTable4[8][4] = {
            {0, 2, 8, 10},
            {1, 3, 9, 11},
            {4, 6, 12, 14},
            {5, 7, 13, 15},
            {16, 18, 24, 26},
            {17, 19, 25, 27},
            {20, 22, 28, 30},
            {21, 23, 29, 31},
        };

        static constexpr uint16_t kColumnTable4[16][32] = {
            {0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110},
            {16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126},
            {65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47},
            {81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63},
            {192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174},
            {208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190},
            {129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239},
            {145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255},
            {256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362, 260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366},
            {272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378, 276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382},
            {321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299, 325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303},
            {337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315, 341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319},
            {448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426, 452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430},
            {464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442, 468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446},
            {385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491, 389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495},
            {401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507, 405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511},
        };

        const uint32_t pagesPerRow = ((width >> 1u) != 0u) ? (width >> 1u) : 1u;
        const uint32_t page = (block >> 5u) + (y >> 7u) * pagesPerRow + (x >> 7u);
        const uint32_t blockId = (block & 0x1Fu) + kBlockTable4[(y >> 4u) & 7u][(x >> 5u) & 3u];
        const uint32_t pageOffset = (blockId >> 5u) << 14u;
        const uint32_t localBlock = blockId & 0x1Fu;
        return (page << 14u) + pageOffset + localBlock * 512u + kColumnTable4[y & 0x0Fu][x & 0x1Fu];
    }

    void writeReferencePSMT4Texel(std::vector<uint8_t> &vram, uint32_t tbp, uint32_t tbw, uint32_t x, uint32_t y, uint8_t index)
    {
        const uint32_t nibbleAddr = referenceAddrPSMT4(tbp, tbw, x, y);
        const uint32_t byteOff = nibbleAddr >> 1;
        uint8_t &packed = vram[byteOff];
        if ((nibbleAddr & 1u) != 0u)
        {
            packed = static_cast<uint8_t>((packed & 0x0Fu) | ((index & 0x0Fu) << 4));
        }
        else
        {
            packed = static_cast<uint8_t>((packed & 0xF0u) | (index & 0x0Fu));
        }
    }

    uint32_t referenceAddrPSMT8(uint32_t block, uint32_t width, uint32_t x, uint32_t y)
    {
        static constexpr uint8_t kBlockTable8[4][8] = {
            {0, 1, 4, 5, 16, 17, 20, 21},
            {2, 3, 6, 7, 18, 19, 22, 23},
            {8, 9, 12, 13, 24, 25, 28, 29},
            {10, 11, 14, 15, 26, 27, 30, 31},
        };

        static constexpr uint8_t kColumnTable8[16][16] = {
            {0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54},
            {8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62},
            {33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23},
            {41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31},
            {96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86},
            {104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94},
            {65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119},
            {73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127},
            {128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182},
            {136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190},
            {161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151},
            {169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159},
            {224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214},
            {232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222},
            {193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247},
            {201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255},
        };

        const uint32_t pagesPerRow = ((width >> 1u) != 0u) ? (width >> 1u) : 1u;
        const uint32_t page = (block >> 5u) + (y >> 6u) * pagesPerRow + (x >> 7u);
        const uint32_t blockId = (block & 0x1Fu) + kBlockTable8[(y >> 4u) & 3u][(x >> 4u) & 7u];
        const uint32_t pageOffset = (blockId >> 5u) << 13u;
        const uint32_t localBlock = blockId & 0x1Fu;
        return (page << 13u) + pageOffset + localBlock * 256u + kColumnTable8[y & 0x0Fu][x & 0x0Fu];
    }

    uint32_t referenceAddrPSMCT32(uint32_t block, uint32_t width, uint32_t x, uint32_t y)
    {
        static constexpr uint8_t kBlockTable32[4][8] = {
            {0, 1, 4, 5, 16, 17, 20, 21},
            {2, 3, 6, 7, 18, 19, 22, 23},
            {8, 9, 12, 13, 24, 25, 28, 29},
            {10, 11, 14, 15, 26, 27, 30, 31},
        };

        static constexpr uint8_t kColumnTable32[8][8] = {
            {0, 1, 4, 5, 8, 9, 12, 13},
            {2, 3, 6, 7, 10, 11, 14, 15},
            {16, 17, 20, 21, 24, 25, 28, 29},
            {18, 19, 22, 23, 26, 27, 30, 31},
            {32, 33, 36, 37, 40, 41, 44, 45},
            {34, 35, 38, 39, 42, 43, 46, 47},
            {48, 49, 52, 53, 56, 57, 60, 61},
            {50, 51, 54, 55, 58, 59, 62, 63},
        };

        const uint32_t pagesPerRow = (width != 0u) ? width : 1u;
        const uint32_t page = (block >> 5u) + (y >> 5u) * pagesPerRow + (x >> 6u);
        const uint32_t blockId = (block & 0x1Fu) + kBlockTable32[(y >> 3u) & 3u][(x >> 3u) & 7u];
        const uint32_t pageOffset = (blockId >> 5u) << 13u;
        const uint32_t localBlock = blockId & 0x1Fu;
        return (page << 13u) + pageOffset + localBlock * 256u +
               static_cast<uint32_t>(kColumnTable32[y & 0x7u][x & 0x7u]) * 4u;
    }

    void writeReferencePSMCT32Pixel(std::vector<uint8_t> &vram,
                                    uint32_t fbp,
                                    uint32_t fbw,
                                    uint32_t x,
                                    uint32_t y,
                                    uint32_t pixel)
    {
        const uint32_t off = referenceAddrPSMCT32(fbp, (fbw != 0u) ? fbw : 1u, x, y);
        std::memcpy(vram.data() + off, &pixel, sizeof(pixel));
    }

    uint32_t readReferencePSMCT32Pixel(const std::vector<uint8_t> &vram,
                                       uint32_t fbp,
                                       uint32_t fbw,
                                       uint32_t x,
                                       uint32_t y)
    {
        const uint32_t off = referenceAddrPSMCT32(fbp, (fbw != 0u) ? fbw : 1u, x, y);
        uint32_t pixel = 0u;
        std::memcpy(&pixel, vram.data() + off, sizeof(pixel));
        return pixel;
    }

    uint32_t frameBaseToBlock(uint32_t fbp)
    {
        return fbp << 5u;
    }

    void writeReferenceFramePSMCT32Pixel(std::vector<uint8_t> &vram,
                                         uint32_t fbp,
                                         uint32_t fbw,
                                         uint32_t x,
                                         uint32_t y,
                                         uint32_t pixel)
    {
        writeReferencePSMCT32Pixel(vram, frameBaseToBlock(fbp), fbw, x, y, pixel);
    }

    uint32_t readReferenceFramePSMCT32Pixel(const std::vector<uint8_t> &vram,
                                            uint32_t fbp,
                                            uint32_t fbw,
                                            uint32_t x,
                                            uint32_t y)
    {
        return readReferencePSMCT32Pixel(vram, frameBaseToBlock(fbp), fbw, x, y);
    }

    void expectGuestHeapReusable(TestCase &t, PS2Runtime &runtime, const char *message)
    {
        const uint32_t expectedBase = runtime.guestHeapBase();
        const uint32_t probe = runtime.guestMalloc(16u, 16u);
        t.Equals(probe, expectedBase, message);
        runtime.guestFree(probe);
    }

    struct GsPixelTestResult
    {
        uint32_t framebuffer = 0u;
        uint32_t depth = 0u;
    };

    GsPixelTestResult drawGsPixelForTests(uint8_t framePsm,
                                          uint64_t testReg,
                                          bool zmask,
                                          uint32_t initialFramebuffer,
                                          uint32_t initialDepth,
                                          uint8_t sourceAlpha)
    {
        constexpr uint32_t kFrameBlock = 0u;
        constexpr uint32_t kDepthBlock = 32u;
        constexpr uint32_t kSourceDepth = 0x22222222u;

        std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
        GS gs;
        gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

        gs.WriteVram(framePsm, kFrameBlock, 1u, 0u, 0u, initialFramebuffer);
        gs.WriteVram(GS_PSM_Z32, kDepthBlock, 1u, 0u, 0u, initialDepth);

        const uint64_t frame =
            (1ull << 16) |
            (static_cast<uint64_t>(framePsm) << 24);
        const uint64_t zbuf =
            1ull |
            (static_cast<uint64_t>(zmask ? 1u : 0u) << 32);
        const uint64_t rgbaq =
            (0x12ull << 0) |
            (0x34ull << 8) |
            (0x56ull << 16) |
            (static_cast<uint64_t>(sourceAlpha) << 24) |
            (0x3F800000ull << 32);

        gs.writeRegister(GS_REG_FRAME_1, frame);
        gs.writeRegister(GS_REG_ZBUF_1, zbuf);
        gs.writeRegister(GS_REG_SCISSOR_1, 0ull);
        gs.writeRegister(GS_REG_TEST_1, testReg);
        gs.writeRegister(GS_REG_PRIM, static_cast<uint64_t>(GS_PRIM_POINT));
        gs.writeRegister(GS_REG_RGBAQ, rgbaq);
        gs.writeRegister(GS_REG_XYZ2, static_cast<uint64_t>(kSourceDepth) << 32);

        return {
            gs.ReadVram(framePsm, kFrameBlock, 1u, 0u, 0u),
            gs.ReadVram(GS_PSM_Z32, kDepthBlock, 1u, 0u, 0u),
        };
    }
}

void register_ps2_gs_tests()
{
    MiniTest::Case("PS2GS", [](TestCase &tc)
    {
        tc.Run("GS CSR/IMR support coherent 64-bit and 32-bit access", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kGsCsr = 0x12001000u;
            constexpr uint32_t kGsImr = 0x12001010u;

            const uint64_t csrPattern = 0xA1B2C3D4E5F60718ull;
            mem.write64(kGsCsr, csrPattern);
            t.Equals(mem.read64(kGsCsr), csrPattern, "64-bit CSR read should match prior 64-bit write");
            t.Equals(mem.read32(kGsCsr), static_cast<uint32_t>(csrPattern & 0xFFFFFFFFull), "CSR low dword read should match");
            t.Equals(mem.read32(kGsCsr + 4u), static_cast<uint32_t>(csrPattern >> 32), "CSR high dword read should match");

            mem.write32(kGsCsr, 0x11223344u);
            t.Equals(mem.read64(kGsCsr), 0xA1B2C3D411223344ull, "32-bit low write should preserve CSR high dword");

            mem.write32(kGsCsr + 4u, 0x55667788u);
            t.Equals(mem.read64(kGsCsr), 0x5566778811223344ull, "32-bit high write should preserve CSR low dword");

            const uint64_t imrPattern = 0x0123456789ABCDEFull;
            mem.write64(kGsImr, imrPattern);
            t.Equals(mem.read64(kGsImr), imrPattern, "IMR 64-bit read should match prior write");
            t.Equals(mem.read32(kGsImr), 0x89ABCDEFu, "IMR low dword should match");
            t.Equals(mem.read32(kGsImr + 4u), 0x01234567u, "IMR high dword should match");
        });

        tc.Run("unknown GS privileged offsets are no-op and read as zero", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kKnownBusdir = 0x12001040u;
            constexpr uint32_t kUnknown = 0x12001008u; // inside GS priv range, but not mapped by gsRegPtr.

            mem.write64(kKnownBusdir, 0xCAFEBABE12345678ull);
            const uint64_t before = mem.read64(kKnownBusdir);
            mem.write32(kUnknown, 0xDEADBEEFu);
            t.Equals(mem.read32(kUnknown), 0u, "unknown GS offset should read as zero");
            t.Equals(mem.read64(kKnownBusdir), before, "unknown GS writes should not corrupt mapped GS registers");
        });

        tc.Run("GS writeIORegister increments GS write counter", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kGsPmode = 0x12000000u;
            constexpr uint32_t kGsImr = 0x12001010u;

            const uint64_t countBefore = mem.gsWriteCount();
            t.IsTrue(mem.writeIORegister(kGsPmode, 0x11u), "writeIORegister PMODE should succeed");
            t.IsTrue(mem.writeIORegister(kGsImr, 0x22u), "writeIORegister IMR should succeed");
            t.Equals(mem.gsWriteCount(), countBefore + 2ull, "GS IO writes should increment GS write counter");

            t.Equals(mem.readIORegister(kGsPmode), 0x11u, "writeIORegister PMODE value should be readable");
            t.Equals(mem.readIORegister(kGsImr), 0x22u, "writeIORegister IMR value should be readable");
        });

        tc.Run("GsPutIMR and GsGetIMR roundtrip old and new values", [](TestCase &t)
        {
            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "runtime memory initialize should succeed");
            runtime.memory().gs().imr = 0xAAAABBBBCCCCDDDDull;

            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);
            R5900Context ctx{};

            setRegU64(ctx, 4, 0x3333444411112222ull);
            GsPutIMR(rdram.data(), &ctx, &runtime);

            const uint64_t oldImr = getReturnU64(ctx);
            t.Equals(oldImr, 0xAAAABBBBCCCCDDDDull, "GsPutIMR should return previous IMR");
            t.Equals(runtime.memory().gs().imr, 0x3333444411112222ull, "GsPutIMR should update GS IMR");

            std::memset(&ctx, 0, sizeof(ctx));
            GsGetIMR(rdram.data(), &ctx, &runtime);
            const uint64_t currentImr = getReturnU64(ctx);
            t.Equals(currentImr, 0x3333444411112222ull, "GsGetIMR should return current GS IMR");
        });

        tc.Run("GsSetCrt updates SMODE2 for host presentation mode", [](TestCase &t)
        {
            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "runtime memory initialize should succeed");

            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);
            R5900Context ctx{};
            setRegU32(ctx, 4, 1u); // interlaced
            setRegU32(ctx, 5, 0u); // NTSC
            setRegU32(ctx, 6, 0u); // field mode

            runtime.memory().gs().pmode = 0u;
            runtime.memory().gs().smode2 = 0u;
            GsSetCrt(rdram.data(), &ctx, &runtime);

            t.Equals(runtime.memory().gs().smode2, 0x1ull,
                     "GsSetCrt should publish interlaced field mode through SMODE2");
            t.Equals(runtime.memory().gs().pmode & 0x3ull, 0x1ull,
                     "GsSetCrt should leave CRT1 enabled for presentation");
            t.Equals(getRegU32Test(ctx, 2), 0u,
                     "GsSetCrt should return success");
        });

        tc.Run("sceGsSetDefDBuffDc seeds display envs and swap applies the selected page", [](TestCase &t)
        {
            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "runtime memory initialize should succeed");

            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);
            constexpr uint32_t kEnvAddr = 0x4000u;
            constexpr uint32_t kDispEnvSize = 40u;
            constexpr uint32_t kDBuffSize = 0x330u;
            constexpr uint32_t kDispFbOffset = 16u;
            constexpr uint32_t kDisplayOffset = 24u;
            constexpr uint32_t kDraw01Offset = 0x60u;
            constexpr uint32_t kFrame1Offset = kDraw01Offset + 0x00u;
            constexpr uint32_t kFrame1AddrOffset = kDraw01Offset + 0x08u;
            constexpr uint32_t kXYOffset1Offset = kDraw01Offset + 0x20u;
            constexpr uint32_t kXYOffset1AddrOffset = kDraw01Offset + 0x28u;

            R5900Context ctx{};
            setRegU32(ctx, 4, kEnvAddr);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 640u);
            setRegU32(ctx, 7, 448u);
            std::memset(rdram.data() + kEnvAddr, 0xCD, kDBuffSize);
            ps2_stubs::sceGsSetDefDBuffDc(rdram.data(), &ctx, &runtime);

            uint64_t dispfb0 = 0u;
            uint64_t display0 = 0u;
            uint64_t frame10 = 0u;
            uint64_t frame10Addr = 0u;
            uint64_t xyoffset10 = 0u;
            uint64_t xyoffset10Addr = 0u;
            std::memcpy(&dispfb0, rdram.data() + kEnvAddr + kDispFbOffset, sizeof(dispfb0));
            std::memcpy(&display0, rdram.data() + kEnvAddr + kDisplayOffset, sizeof(display0));
            std::memcpy(&frame10, rdram.data() + kEnvAddr + kFrame1Offset, sizeof(frame10));
            std::memcpy(&frame10Addr, rdram.data() + kEnvAddr + kFrame1AddrOffset, sizeof(frame10Addr));
            std::memcpy(&xyoffset10, rdram.data() + kEnvAddr + kXYOffset1Offset, sizeof(xyoffset10));
            std::memcpy(&xyoffset10Addr, rdram.data() + kEnvAddr + kXYOffset1AddrOffset, sizeof(xyoffset10Addr));

            t.Equals((dispfb0 >> 9) & 0x3Fu, 10ull, "dbuff display env should seed FBW from width");
            t.Equals((display0 >> 32) & 0x0FFFull, 639ull, "dbuff display env should seed DW from width");
            t.Equals((display0 >> 44) & 0x07FFull, 447ull, "dbuff display env should seed DH from height");
            t.Equals((frame10 >> 16) & 0x3Full, 10ull, "dbuff draw env should seed FRAME FBW from width");
            t.Equals(frame10Addr, 0x4Cull, "dbuff draw env should seed FRAME_1 register id");
            t.Equals(xyoffset10 & 0xFFFFull, 0x6C00ull, "dbuff draw env should seed OFX in 12.4 fixed point");
            t.Equals((xyoffset10 >> 32) & 0xFFFFull, 0x7200ull, "dbuff draw env should seed OFY in 12.4 fixed point");
            t.Equals(xyoffset10Addr, 0x18ull, "dbuff draw env should seed XYOFFSET_1 register id");

            dispfb0 = (dispfb0 & ~0x1FFull) | 150ull;
            std::memcpy(rdram.data() + kEnvAddr + kDispFbOffset, &dispfb0, sizeof(dispfb0));

            uint64_t dispfb1 = dispfb0;
            dispfb1 = (dispfb1 & ~0x1FFull) | 151ull;
            std::memcpy(rdram.data() + kEnvAddr + kDispEnvSize + kDispFbOffset, &dispfb1, sizeof(dispfb1));

            std::memset(&ctx, 0, sizeof(ctx));
            setRegU32(ctx, 4, kEnvAddr);
            setRegU32(ctx, 5, 1u);
            ps2_stubs::sceGsSwapDBuffDc(rdram.data(), &ctx, &runtime);

            t.Equals(runtime.memory().gs().dispfb1 & 0x1FFull, 151ull,
                     "sceGsSwapDBuffDc should program GS to the selected display page");
            t.Equals((runtime.memory().gs().display1 >> 32) & 0x0FFFull, 639ull,
                     "sceGsSwapDBuffDc should preserve the display width from the seeded env");
        });











        tc.Run("fullscreen display copy tracks the preferred presentation source frame", [](TestCase &t)
        {
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            constexpr uint64_t kFrame2 =
                150ull |
                (10ull << 16) |
                (static_cast<uint64_t>(GS_PSM_CT32) << 24);
            constexpr uint64_t kScissor2 =
                (0ull << 0) |
                (639ull << 16) |
                (0ull << 32) |
                (479ull << 48);
            constexpr uint64_t kXYOffset2 =
                (static_cast<uint64_t>(1728u << 4) << 0) |
                (static_cast<uint64_t>(1808u << 4) << 32);
            constexpr uint64_t kAlpha2 = 0x6000000064ull;
            constexpr uint64_t kTest2 = 0x30000ull;
            constexpr uint64_t kTex0_2 =
                (0ull << 0) |
                (10ull << 14) |
                (static_cast<uint64_t>(GS_PSM_CT32) << 20) |
                (10ull << 26) |
                (9ull << 30) |
                (1ull << 34);
            constexpr uint64_t kPrimCopy =
                static_cast<uint64_t>(GS_PRIM_SPRITE) |
                (1ull << 4) |
                (1ull << 6) |
                (1ull << 8) |
                (1ull << 9);
            constexpr uint64_t kXyz0 =
                (static_cast<uint64_t>(1728u << 4) << 0) |
                (static_cast<uint64_t>(1808u << 4) << 16) |
                (256ull << 32);
            constexpr uint64_t kXyz1 =
                (static_cast<uint64_t>(2368u << 4) << 0) |
                (static_cast<uint64_t>(2288u << 4) << 16) |
                (256ull << 32);
            constexpr uint64_t kUv0 =
                (8ull << 0) |
                (8ull << 16);
            constexpr uint64_t kUv1 =
                ((8ull + (640ull * 16ull)) << 0) |
                ((8ull + (480ull * 16ull)) << 16);

            gs.writeRegister(GS_REG_FRAME_2, kFrame2);
            gs.writeRegister(GS_REG_SCISSOR_2, kScissor2);
            gs.writeRegister(GS_REG_XYOFFSET_2, kXYOffset2);
            gs.writeRegister(GS_REG_ALPHA_2, kAlpha2);
            gs.writeRegister(GS_REG_TEST_2, kTest2);
            gs.writeRegister(GS_REG_TEX0_2, kTex0_2);
            gs.writeRegister(GS_REG_PRIM, kPrimCopy);
            gs.writeRegister(GS_REG_RGBAQ, 0x80808080ull);
            gs.writeRegister(GS_REG_UV, kUv0);
            gs.writeRegister(GS_REG_XYZ2, kXyz0);
            gs.writeRegister(GS_REG_UV, kUv1);
            gs.writeRegister(GS_REG_XYZ2, kXyz1);

            GSFrameReg preferredSource{};
            uint32_t preferredDestFbp = 0u;
            t.IsTrue(gs.getPreferredDisplaySource(preferredSource, preferredDestFbp),
                     "fullscreen display-copy sprites should record their source frame for host presentation");
            t.Equals(preferredDestFbp, 150u,
                     "preferred presentation tracking should target the copied display page");
            t.Equals(preferredSource.fbp, 0u,
                     "preferred presentation tracking should expose the copy source frame base");
            t.Equals(preferredSource.fbw, 10u,
                     "preferred presentation tracking should expose the copy source width");
            t.Equals(static_cast<uint32_t>(preferredSource.psm), static_cast<uint32_t>(GS_PSM_CT32),
                     "preferred presentation tracking should expose the copy source format");

            gs.writeRegister(GS_REG_PRIM, static_cast<uint64_t>(GS_PRIM_POINT) | (1ull << 9));
            gs.writeRegister(GS_REG_RGBAQ, 0xFFFFFFFFull);
            gs.writeRegister(GS_REG_XYZ2, kXyz0);

            t.IsFalse(gs.getPreferredDisplaySource(preferredSource, preferredDestFbp),
                      "non-copy primitives that touch the display target should invalidate the preferred presentation source");
        });









        tc.Run("GIF PACKED A+D writes DISPFB1 and DISPLAY1 privileged registers", [](TestCase &t)
        {
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GSRegisters regs{};
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), &regs);

            std::vector<uint8_t> packet;
            appendU64(packet, makeGifTag(2u, GIF_FMT_PACKED, 1u, true));
            appendU64(packet, 0x0Eull); // REGS[0] = A+D

            const uint64_t dispfb1 = 0x0123456789ABCDEFull;
            const uint64_t display1 = 0x1111222233334444ull;
            appendU64(packet, dispfb1);
            appendU64(packet, 0x59ull); // DISPFB1
            appendU64(packet, display1);
            appendU64(packet, 0x5Aull); // DISPLAY1

            gs.processGIFPacket(packet.data(), static_cast<uint32_t>(packet.size()));

            t.Equals(regs.dispfb1, dispfb1, "A+D should write GS DISPFB1");
            t.Equals(regs.display1, display1, "A+D should write GS DISPLAY1");
        });

        tc.Run("GIF PACKED A+D writes DISPFB2 and DISPLAY2 privileged registers", [](TestCase &t)
        {
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GSRegisters regs{};
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), &regs);

            std::vector<uint8_t> packet;
            appendU64(packet, makeGifTag(2u, GIF_FMT_PACKED, 1u, true));
            appendU64(packet, 0x0Eull); // REGS[0] = A+D

            const uint64_t dispfb2 = 0x2222333344445555ull;
            const uint64_t display2 = 0x6666777788889999ull;
            appendU64(packet, dispfb2);
            appendU64(packet, 0x5Bull); // DISPFB2
            appendU64(packet, display2);
            appendU64(packet, 0x5Cull); // DISPLAY2

            gs.processGIFPacket(packet.data(), static_cast<uint32_t>(packet.size()));

            t.Equals(regs.dispfb2, dispfb2, "A+D should write GS DISPFB2");
            t.Equals(regs.display2, display2, "A+D should write GS DISPLAY2");
        });

        tc.Run("reserved PSM 0x3F uses null VRAM handlers", [](TestCase &t)
        {
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0xA5u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            t.Equals(gs.ReadVram(0x3Fu, 0u, 1u, 0u, 0u), 0u,
                     "reserved PSM reads should use the null handler");

            gs.WriteVram(0x3Fu, 0u, 1u, 0u, 0u, 0x0005180Bu);
            t.Equals(static_cast<uint32_t>(vram[0]), 0xA5u,
                     "reserved PSM writes should leave VRAM unchanged");
        });

        tc.Run("PSMT4 address mapping matches GS manual layout", [](TestCase &t)
        {
            constexpr uint32_t kBaseBlock = 0u;
            constexpr uint32_t kWidth = 2u; // One 128x128 PSMT4 page.

            t.Equals(GSPSMT4::addrPSMT4(kBaseBlock, kWidth, 0u, 0u), 0u,
                     "PSMT4 origin should map to nibble offset 0");
            t.Equals(GSPSMT4::addrPSMT4(kBaseBlock, kWidth, 1u, 0u), 8u,
                     "PSMT4 x=1 should advance to the next packed nibble group");
            t.Equals(GSPSMT4::addrPSMT4(kBaseBlock, kWidth, 0u, 1u), 16u,
                     "PSMT4 second source row should follow the manual's row packing");
            t.Equals(GSPSMT4::addrPSMT4(kBaseBlock, kWidth, 0u, 2u), 65u,
                     "PSMT4 third source row should include the manual's odd-row permutation");
            t.Equals(GSPSMT4::addrPSMT4(kBaseBlock, kWidth, 0u, 3u), 81u,
                     "PSMT4 fourth source row should stay in the first block's manual column layout");
            t.Equals(GSPSMT4::addrPSMT4(kBaseBlock, kWidth, 31u, 15u), 511u,
                     "PSMT4 final texel in the first 32x16 block should land at the end of the block");
            t.Equals(GSPSMT4::addrPSMT4(kBaseBlock, kWidth, 32u, 0u), 1024u,
                     "PSMT4 x=32 should advance to the next swizzled block in the page");
            t.Equals(GSPSMT4::addrPSMT4(kBaseBlock, kWidth, 32u, 16u), 1536u,
                     "PSMT4 x=32,y=16 should follow the manual's second block-row permutation");
            t.Equals(GSPSMT4::addrPSMT4(kBaseBlock, kWidth, 64u, 0u), 4096u,
                     "PSMT4 x=64 should advance to the third swizzled block column in the page");
            t.Equals(GSPSMT4::addrPSMT4(kBaseBlock, kWidth, 96u, 112u), 15872u,
                     "PSMT4 bottom-right block origin should match the manual's page permutation");
            t.Equals(GSPSMT4::addrPSMT4(kBaseBlock, kWidth, 127u, 127u), 16383u,
                     "PSMT4 final texel in a 128x128 page should land at the end of the page");
            t.Equals(GSPSMT4::addrPSMT4(kBaseBlock, kWidth, 128u, 0u), 16384u,
                     "PSMT4 x=128 should advance to the next page of nibble addresses");
        });

        tc.Run("PSMT4 large atlases keep manual page layout across 512x512 textures", [](TestCase &t)
        {
            constexpr uint32_t kBaseBlock = 64u;
            constexpr uint32_t kWidth = 8u; // 512 pixel-wide T4 atlas, like Veronica font pages.
            constexpr uint32_t kCoords[][2] = {
                {0u, 0u},
                {31u, 15u},
                {32u, 0u},
                {95u, 31u},
                {127u, 127u},
                {128u, 0u},
                {255u, 127u},
                {256u, 0u},
                {383u, 127u},
                {384u, 128u},
                {511u, 511u},
            };

            for (const auto &coord : kCoords)
            {
                const uint32_t x = coord[0];
                const uint32_t y = coord[1];
                t.Equals(GSPSMT4::addrPSMT4(kBaseBlock, kWidth, x, y),
                         referenceAddrPSMT4(kBaseBlock, kWidth, x, y),
                         "PSMT4 512x512 atlas mapping should match the GS manual for every sampled page boundary");
            }
        });


        tc.Run("PSMT8 address mapping matches Veronica Conv8to32 layout", [](TestCase &t)
        {
            constexpr uint32_t kBaseBlock = 0u;
            constexpr uint32_t kWidth = 2u; // One 128x64 PSMT8 page.

            t.Equals(GSPSMT8::addrPSMT8(kBaseBlock, kWidth, 0u, 0u), 0u,
                     "PSMT8 origin should map to byte offset 0");
            t.Equals(GSPSMT8::addrPSMT8(kBaseBlock, kWidth, 1u, 0u), 4u,
                     "PSMT8 x=1 should follow Veronica's Conv8to32 byte interleave");
            t.Equals(GSPSMT8::addrPSMT8(kBaseBlock, kWidth, 0u, 1u), 8u,
                     "PSMT8 second source row should land on the next Conv8to32 row stride");
            t.Equals(GSPSMT8::addrPSMT8(kBaseBlock, kWidth, 0u, 2u), 33u,
                     "PSMT8 third source row should preserve Veronica's odd-row shuffle");
            t.Equals(GSPSMT8::addrPSMT8(kBaseBlock, kWidth, 0u, 3u), 41u,
                     "PSMT8 fourth source row should preserve Veronica's alternating block rows");
            t.Equals(GSPSMT8::addrPSMT8(kBaseBlock, kWidth, 15u, 15u), 255u,
                     "PSMT8 final texel in the first 16x16 block should end at byte 255");
            t.Equals(GSPSMT8::addrPSMT8(kBaseBlock, kWidth, 16u, 0u), 256u,
                     "PSMT8 x=16 should advance to the next 16x16 block");
            t.Equals(GSPSMT8::addrPSMT8(kBaseBlock, kWidth, 16u, 16u), 768u,
                     "PSMT8 x=16,y=16 should include both block-column and block-row offsets");
            t.Equals(GSPSMT8::addrPSMT8(kBaseBlock, kWidth, 32u, 0u), 1024u,
                     "PSMT8 x=32 should advance to the third block column in the page");
            t.Equals(GSPSMT8::addrPSMT8(kBaseBlock, kWidth, 64u, 0u), 4096u,
                     "PSMT8 x=64 should advance to the second page half");
            t.Equals(GSPSMT8::addrPSMT8(kBaseBlock, kWidth, 96u, 48u), 7680u,
                     "PSMT8 lower-right interior block should follow Veronica's page permutation");
            t.Equals(GSPSMT8::addrPSMT8(kBaseBlock, kWidth, 127u, 63u), 8191u,
                     "PSMT8 final texel in a 128x64 page should land at the final byte");
        });



        tc.Run("GS SIGNAL and FINISH set CSR bits that clear by CSR write-one acknowledge", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            GS gs;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());

            const uint64_t signalValue = (0xFFFFFFFFull << 32) | 0x11223344ull;
            gs.writeRegister(GS_REG_SIGNAL, signalValue);
            gs.writeRegister(GS_REG_FINISH, 0u);

            t.IsTrue((mem.gs().csr & 0x1ull) != 0ull, "SIGNAL should raise CSR.SIGNAL");
            t.IsTrue((mem.gs().csr & 0x2ull) != 0ull, "FINISH should raise CSR.FINISH");
            t.Equals(static_cast<uint32_t>(mem.gs().siglblid & 0xFFFFFFFFull), 0x11223344u, "SIGNAL should update SIGLBLID low dword");

            mem.write64(0x12001000u, 0x1ull);
            t.IsTrue((mem.gs().csr & 0x1ull) == 0ull, "writing CSR bit0 should acknowledge SIGNAL");
            t.IsTrue((mem.gs().csr & 0x2ull) != 0ull, "acknowledging SIGNAL should not clear FINISH");

            mem.write32(0x12001000u, 0x2u);
            t.IsTrue((mem.gs().csr & 0x2ull) == 0ull, "writing CSR bit1 should acknowledge FINISH");
        });































        tc.Run("sceGifPkRefLoadImage seeds A+D GIFtag nloop once (no double-count)", [](TestCase &t)
        {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);
            PS2Runtime runtime;
            R5900Context ctx{};

            constexpr uint32_t stateAddr = 0x1000u;
            constexpr uint32_t baseAddr = 0x2000u;
            constexpr uint32_t spAddr = 0x8000u;

            setRegU32(ctx, 4, stateAddr);
            setRegU32(ctx, 5, baseAddr);
            ps2_stubs::sceGifPkInit(rdram.data(), &ctx, &runtime);

            setRegU32(ctx, 29, spAddr);
            const uint32_t width = 16u;
            const uint32_t height = 1u;
            std::memcpy(rdram.data() + spAddr, &width, sizeof(width));
            std::memcpy(rdram.data() + spAddr + 8u, &height, sizeof(height));

            const uint32_t dbp = 0x3fc0u;
            const uint32_t dpsm = 0u;
            const uint32_t dbw = 1u;
            const uint32_t dataAddr = 0u;
            const uint32_t dsax = 0u;
            const uint32_t dsay = 0u;

            setRegU32(ctx, 4, stateAddr);
            setRegU32(ctx, 5, dbp);
            setRegU32(ctx, 6, dpsm);
            setRegU32(ctx, 7, dbw);
            setRegU32(ctx, 8, dataAddr);
            setRegU32(ctx, 9, 0u); // qwcRemaining: setup only, no image body
            setRegU32(ctx, 10, dsax);
            setRegU32(ctx, 11, dsay);
            ps2_stubs::sceGifPkRefLoadImage(rdram.data(), &ctx, &runtime);

            uint64_t tagLo = 0u;
            uint64_t tagHi = 0u;
            std::memcpy(&tagLo, rdram.data() + baseAddr + 16u, sizeof(tagLo));
            std::memcpy(&tagHi, rdram.data() + baseAddr + 24u, sizeof(tagHi));
            t.Equals(tagLo, static_cast<uint64_t>(0x1000000000000004ULL),
                      "header GIFtag lo must be nloop=4 nreg=1 A+D eop=0 (double-count would give ...0008)");
            t.Equals(tagHi, static_cast<uint64_t>(0xEULL),
                      "header GIFtag hi must be A+D register descriptor 0xE");

            uint64_t reg1Desc = 0u;
            uint64_t reg2Desc = 0u;
            uint64_t reg3Desc = 0u;
            uint64_t reg4Desc = 0u;
            std::memcpy(&reg1Desc, rdram.data() + baseAddr + 32u + 0u * 16u + 8u, sizeof(reg1Desc));
            std::memcpy(&reg2Desc, rdram.data() + baseAddr + 32u + 1u * 16u + 8u, sizeof(reg2Desc));
            std::memcpy(&reg3Desc, rdram.data() + baseAddr + 32u + 2u * 16u + 8u, sizeof(reg3Desc));
            std::memcpy(&reg4Desc, rdram.data() + baseAddr + 32u + 3u * 16u + 8u, sizeof(reg4Desc));
            t.Equals(reg1Desc, static_cast<uint64_t>(0x50ULL), "first register qword should be BITBLTBUF (0x50)");
            t.Equals(reg2Desc, static_cast<uint64_t>(0x51ULL), "second register qword should be TRXPOS (0x51)");
            t.Equals(reg3Desc, static_cast<uint64_t>(0x52ULL), "third register qword should be TRXREG (0x52)");
            t.Equals(reg4Desc, static_cast<uint64_t>(0x53ULL), "fourth register qword should be TRXDIR (0x53)");

            uint64_t reg1Payload = 0u;
            uint64_t reg2Payload = 0u;
            uint64_t reg3Payload = 0u;
            uint64_t reg4Payload = 0u;
            std::memcpy(&reg1Payload, rdram.data() + baseAddr + 32u + 0u * 16u + 0u, sizeof(reg1Payload));
            std::memcpy(&reg2Payload, rdram.data() + baseAddr + 32u + 1u * 16u + 0u, sizeof(reg2Payload));
            std::memcpy(&reg3Payload, rdram.data() + baseAddr + 32u + 2u * 16u + 0u, sizeof(reg3Payload));
            std::memcpy(&reg4Payload, rdram.data() + baseAddr + 32u + 3u * 16u + 0u, sizeof(reg4Payload));
            // BITBLTBUF: dbp=0x3fc0 (bits 32-45), dbw=1 (bits 48-53), dpsm=0 (bits 56-61).
            t.Equals(reg1Payload, static_cast<uint64_t>(0x00013FC000000000ULL),
                      "BITBLTBUF payload must encode dbp=0x3fc0, dbw=1, dpsm=0");
            // TRXPOS: dsax=0, dsay=0.
            t.Equals(reg2Payload, static_cast<uint64_t>(0x0ULL),
                      "TRXPOS payload must encode dsax=0, dsay=0");
            // TRXREG: width=16 (bits 0-31), height=1 (bits 32-63).
            t.Equals(reg3Payload, static_cast<uint64_t>(0x0000000100000010ULL),
                      "TRXREG payload must encode width=16, height=1");
            // TRXDIR: host-to-local transfer, dir=0.
            t.Equals(reg4Payload, static_cast<uint64_t>(0x0ULL),
                      "TRXDIR payload must encode dir=0 (host-to-local)");
        });

        tc.Run("sceGsResetGraph frees its temporary GIF packet", [](TestCase &t)
        {
            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "runtime memory initialize should succeed");

            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);
            R5900Context ctx{};
            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 1u);
            setRegU32(ctx, 6, 2u);
            setRegU32(ctx, 7, 1u);
            ps2_stubs::sceGsResetGraph(rdram.data(), &ctx, &runtime);

            t.Equals(static_cast<int32_t>(getRegU32Test(ctx, 2)), 0,
                     "sceGsResetGraph should succeed in reset mode");
            expectGuestHeapReusable(t, runtime,
                                    "sceGsResetGraph should free its temporary GIF packet");
        });

        tc.Run("sceGsSyncV resumes through the scheduler with deterministic field parity", [](TestCase &t)
        {
            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "runtime memory initialize should succeed");
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);

            R5900Context resetCtx{};
            setRegU32(resetCtx, 4, 0u);
            setRegU32(resetCtx, 5, 1u);
            setRegU32(resetCtx, 6, 2u);
            setRegU32(resetCtx, 7, 1u);
            ps2_stubs::sceGsResetGraph(rdram.data(), &resetCtx, &runtime);
            runtime.registerFunction(kGsSyncWait0Pc, testGsSyncWait0);
            runtime.registerFunction(kGsSyncResume0Pc, testGsSyncResume0);
            runtime.registerFunction(kGsSyncWait1Pc, testGsSyncWait1);
            runtime.registerFunction(kGsSyncResume1Pc, testGsSyncResume1);
            g_gsSyncFirstField.store(-1, std::memory_order_release);
            g_gsSyncSecondField.store(-1, std::memory_order_release);

            R5900Context mainContext{};
            mainContext.pc = kGsSyncWait0Pc;
            runtime.eeScheduler().reset(rdram.data(), mainContext);
            runtime.eeScheduler().run();

            t.Equals(g_gsSyncFirstField.load(std::memory_order_acquire), 0,
                     "first interlaced VBlank should report even field");
            t.Equals(g_gsSyncSecondField.load(std::memory_order_acquire), 1,
                     "second interlaced VBlank should report odd field");
        });

        tc.Run("sceGsSyncVCallback runs as a scheduler invocation on its callback stack", [](TestCase &t)
        {
            g_gsSyncCallbackHits.store(0u, std::memory_order_relaxed);
            g_gsSyncCallbackLastTick.store(0u, std::memory_order_relaxed);
            g_gsSyncCallbackSp.store(0u, std::memory_order_relaxed);
            g_gsSyncCallbackGp.store(0u, std::memory_order_relaxed);
            g_gsSyncCallbackPrevious.store(0xFFFFFFFFu, std::memory_order_relaxed);

            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "runtime memory initialize should succeed");
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);
            runtime.configureGuestHeap(0x01F00000u, 0x01F00000u);
            runtime.registerFunction(kGsCallbackMainPc, testGsCallbackMain);
            runtime.registerFunction(kGsCallbackResumePc, testGsCallbackResume);
            runtime.registerFunction(kGsCallbackPc, testGsSyncVCallback);

            R5900Context mainContext{};
            mainContext.pc = kGsCallbackMainPc;
            runtime.eeScheduler().reset(rdram.data(), mainContext);
            runtime.eeScheduler().run();

            t.Equals(g_gsSyncCallbackPrevious.load(std::memory_order_acquire), 0u,
                     "first callback registration should return no previous callback");
            t.Equals(g_gsSyncCallbackHits.load(std::memory_order_acquire), 1u,
                     "the callback should execute once at the next VBlank boundary");
            t.IsTrue(g_gsSyncCallbackLastTick.load(std::memory_order_acquire) > 0u,
                     "VSync callback should receive a positive tick value");
            t.Equals(g_gsSyncCallbackGp.load(std::memory_order_acquire), kGsCallbackGp,
                     "callback invocation should preserve the registered GP");
            t.IsTrue(g_gsSyncCallbackSp.load(std::memory_order_acquire) >= 0x01F00000u,
                     "callback invocation should use the reserved async stack pool");
            t.IsTrue(g_gsSyncCallbackSp.load(std::memory_order_acquire) != kGsCallbackCallerSp,
                     "callback invocation must not reuse the caller stack");
        });



        tc.Run("GS T4HL upload deactivates the transfer at total_pixels and discards excess bytes", [](TestCase &t)
        {
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            constexpr uint32_t kDbw = 1u;
            constexpr uint32_t kRrw = 8u;
            constexpr uint32_t kRrh = 8u;
            constexpr uint64_t kRect = (static_cast<uint64_t>(kRrw) << 0) | (static_cast<uint64_t>(kRrh) << 32);

            auto buildPacked = [&](const auto &indexFn) -> std::vector<uint8_t>
            {
                std::vector<uint8_t> packed((kRrw * kRrh) / 2u, 0u);
                for (uint32_t y = 0; y < kRrh; ++y)
                {
                    for (uint32_t x = 0; x < kRrw; x += 2u)
                    {
                        const uint8_t lo = indexFn(x, y) & 0xFu;
                        const uint8_t hi = indexFn(x + 1u, y) & 0xFu;
                        packed[(y * kRrw + x) / 2u] = static_cast<uint8_t>(lo | (hi << 4));
                    }
                }
                return packed;
            };

            // --- First transfer: an ~8x oversized IMAGE payload (256 bytes / 16 qwords) for a
            // rect that only needs 32 bytes (64 texels). The first 32 bytes carry a known
            // pattern; the remaining 224 bytes are a 0xFF sentinel that must be discarded.
            constexpr uint32_t kDbp1 = 0u;
            constexpr uint64_t kUploadBitblt1 =
                (static_cast<uint64_t>(kDbp1) << 32) |
                (static_cast<uint64_t>(kDbw) << 48) |
                (static_cast<uint64_t>(GS_PSM_T4HL) << 56);

            auto indexPattern1 = [](uint32_t x, uint32_t y) -> uint8_t
            {
                return static_cast<uint8_t>((x + y * 3u + 2u) & 0xFu);
            };
            const std::vector<uint8_t> packed1 = buildPacked(indexPattern1);
            t.Equals(packed1.size(), static_cast<size_t>(32), "sanity: packed rect should be 32 bytes (64 texels)");

            gs.writeRegister(GS_REG_BITBLTBUF, kUploadBitblt1);
            gs.writeRegister(GS_REG_TRXPOS, 0ull);
            gs.writeRegister(GS_REG_TRXREG, kRect);
            gs.writeRegister(GS_REG_TRXDIR, 0ull);

            constexpr uint32_t kOversizedBytes = 256u; // 16 qwords, 8x the required 32 bytes
            std::vector<uint8_t> packet;
            appendU64(packet, makeGifTag(static_cast<uint16_t>(kOversizedBytes / 16u), GIF_FMT_IMAGE, 0u, true));
            appendU64(packet, 0ull);
            const size_t payloadOffset = packet.size();
            packet.resize(payloadOffset + kOversizedBytes, 0xFFu);
            std::memcpy(packet.data() + payloadOffset, packed1.data(), packed1.size());
            gs.processGIFPacket(packet.data(), static_cast<uint32_t>(packet.size()));

            const GSDebugSnapshot snap1 = gs.getDebugSnapshot();
            t.Equals(snap1.transferCopiedPixels, 64u, "T4HL transfer should stop after copying exactly rrw*rrh texels");
            t.Equals(snap1.trxdir, 3u, "T4HL transfer should deactivate (trxdir=3) once total_pixels is reached");

            bool pattern1Ok = true;
            for (uint32_t y = 0; y < kRrh; ++y)
            {
                for (uint32_t x = 0; x < kRrw; ++x)
                {
                    const uint32_t off = GSPSMCT32::addrPSMCT32(kDbp1, kDbw, x, y);
                    uint32_t word = 0u;
                    std::memcpy(&word, vram.data() + off, sizeof(word));
                    if (((word >> 24) & 0xFu) != indexPattern1(x, y))
                        pattern1Ok = false;
                }
            }
            t.IsTrue(pattern1Ok,
                     "the first 64 texels of the oversized T4HL transfer should match the known pattern; sentinel bytes must not leak in");

            // --- Second, correctly-sized transfer to a different DBP: proves the discarded
            // excess bytes from the first transfer were not mis-accounted into later state.
            constexpr uint32_t kDbp2 = 128u;
            constexpr uint64_t kUploadBitblt2 =
                (static_cast<uint64_t>(kDbp2) << 32) |
                (static_cast<uint64_t>(kDbw) << 48) |
                (static_cast<uint64_t>(GS_PSM_T4HL) << 56);

            auto indexPattern2 = [](uint32_t x, uint32_t y) -> uint8_t
            {
                return static_cast<uint8_t>((x * 5u + y + 3u) & 0xFu);
            };
            const std::vector<uint8_t> packed2 = buildPacked(indexPattern2);

            gs.writeRegister(GS_REG_BITBLTBUF, kUploadBitblt2);
            gs.writeRegister(GS_REG_TRXPOS, 0ull);
            gs.writeRegister(GS_REG_TRXREG, kRect);
            gs.writeRegister(GS_REG_TRXDIR, 0ull);

            std::vector<uint8_t> packet2;
            appendU64(packet2, makeGifTag(static_cast<uint16_t>(packed2.size() / 16u), GIF_FMT_IMAGE, 0u, true));
            appendU64(packet2, 0ull);
            packet2.insert(packet2.end(), packed2.begin(), packed2.end());
            gs.processGIFPacket(packet2.data(), static_cast<uint32_t>(packet2.size()));

            const GSDebugSnapshot snap2 = gs.getDebugSnapshot();
            t.Equals(snap2.transferCopiedPixels, 64u, "second, correctly-sized T4HL transfer should copy exactly rrw*rrh texels");
            t.Equals(snap2.trxdir, 3u, "second T4HL transfer should also deactivate cleanly");

            bool pattern2Ok = true;
            for (uint32_t y = 0; y < kRrh; ++y)
            {
                for (uint32_t x = 0; x < kRrw; ++x)
                {
                    const uint32_t off = GSPSMCT32::addrPSMCT32(kDbp2, kDbw, x, y);
                    uint32_t word = 0u;
                    std::memcpy(&word, vram.data() + off, sizeof(word));
                    if (((word >> 24) & 0xFu) != indexPattern2(x, y))
                        pattern2Ok = false;
                }
            }
            t.IsTrue(pattern2Ok,
                     "second T4HL transfer to a different DBP should be byte-correct, proving the discarded excess bytes from the first transfer did not leak into subsequent transfer state");
        });
    });
}
