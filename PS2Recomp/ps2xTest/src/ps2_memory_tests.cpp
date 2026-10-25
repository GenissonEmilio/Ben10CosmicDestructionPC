#include "runtime/ps2_perf.h"
#include "MiniTest.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "Stubs/DMA.h"
#include "Stubs/GS.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    uint32_t makeVifCmd(uint8_t opcode, uint8_t num, uint16_t imm)
    {
        return (static_cast<uint32_t>(opcode) << 24) |
               (static_cast<uint32_t>(num) << 16) |
               static_cast<uint32_t>(imm);
    }

    void setRegU32(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    void appendU32(std::vector<uint8_t> &dst, uint32_t value)
    {
        const size_t pos = dst.size();
        dst.resize(pos + sizeof(uint32_t));
        std::memcpy(dst.data() + pos, &value, sizeof(uint32_t));
    }

    void appendU64(std::vector<uint8_t> &dst, uint64_t value)
    {
        const size_t pos = dst.size();
        dst.resize(pos + sizeof(uint64_t));
        std::memcpy(dst.data() + pos, &value, sizeof(uint64_t));
    }

    uint64_t makeDmaTag(uint16_t qwc, uint8_t id, uint32_t addr, bool irq = false)
    {
        return static_cast<uint64_t>(qwc) |
               (static_cast<uint64_t>(id & 0x7u) << 28) |
               (irq ? (1ull << 31) : 0ull) |
               (static_cast<uint64_t>(addr & 0x7FFFFFFFu) << 32);
    }

    void writeDmaTag(uint8_t *rdram, uint32_t tagAddr, uint64_t tagLo)
    {
        std::memset(rdram + tagAddr, 0, 16);
        std::memcpy(rdram + tagAddr, &tagLo, sizeof(tagLo));
    }

    void writeU64(uint8_t *rdram, uint32_t addr, uint64_t value)
    {
        std::memcpy(rdram + addr, &value, sizeof(value));
    }

    uint64_t makeGifTag(uint16_t nloop, uint8_t flg, uint8_t nreg, bool eop);

    uint64_t makeBitbltbuf(uint32_t dbp, uint32_t dbw, uint32_t dpsm)
    {
        return (static_cast<uint64_t>(dbp & 0x3FFFu) << 32) |
               (static_cast<uint64_t>(dbw & 0x3Fu) << 48) |
               (static_cast<uint64_t>(dpsm & 0x3Fu) << 56);
    }

    uint32_t writeGifAd(uint8_t *rdram, uint32_t addr, uint64_t value, uint64_t reg)
    {
        writeU64(rdram, addr + 0u, value);
        writeU64(rdram, addr + 8u, reg);
        return addr + 16u;
    }

    uint32_t writeTextureUploadSetup(uint8_t *rdram, uint32_t addr, uint32_t dbp, uint32_t dpsm)
    {
        writeDmaTag(rdram, addr, makeDmaTag(5u, 1u, 0u, false)); // CNT: setup tag + four A+D writes.
        addr += 16u;
        writeU64(rdram, addr + 0u, makeGifTag(4u, GIF_FMT_PACKED, 1u, false));
        writeU64(rdram, addr + 8u, 0x0Eull); // GIF PACKED A+D descriptor.
        addr += 16u;
        addr = writeGifAd(rdram, addr, makeBitbltbuf(dbp, 1u, dpsm), GS_REG_BITBLTBUF);
        addr = writeGifAd(rdram, addr, 0ull, GS_REG_TRXPOS);
        addr = writeGifAd(rdram, addr, (16ull << 0) | (16ull << 32), GS_REG_TRXREG);
        addr = writeGifAd(rdram, addr, 0ull, GS_REG_TRXDIR);
        return addr;
    }

    uint32_t writeTextureImageRef(uint8_t *rdram, uint32_t addr, uint32_t qwc, uint32_t dataAddr)
    {
        writeDmaTag(rdram, addr, makeDmaTag(1u, 1u, 0u, false)); // CNT: GIF IMAGE tag.
        addr += 16u;
        writeU64(rdram, addr + 0u, makeGifTag(static_cast<uint16_t>(qwc), GIF_FMT_IMAGE, 0u, false));
        writeU64(rdram, addr + 8u, 0ull);
        addr += 16u;
        writeDmaTag(rdram, addr, makeDmaTag(static_cast<uint16_t>(qwc), 3u, dataAddr, false)); // REF: image payload.
        return addr + 16u;
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

    uint64_t makeGifTagPrim(uint16_t nloop, uint16_t prim, uint8_t flg, uint8_t nreg, bool eop = true, bool pre = true)
    {
        uint64_t tag = makeGifTag(nloop, flg, nreg, eop);
        if (pre)
            tag |= (1ull << 46);
        tag |= (static_cast<uint64_t>(prim & 0x7FFu) << 47);
        return tag;
    }

    uint64_t makeGsFrame(uint32_t fbp, uint32_t fbw, uint32_t psm, uint32_t mask = 0u)
    {
        return static_cast<uint64_t>(fbp & 0x1FFu) |
               (static_cast<uint64_t>(fbw & 0x3Fu) << 16u) |
               (static_cast<uint64_t>(psm & 0x3Fu) << 24u) |
               (static_cast<uint64_t>(mask) << 32u);
    }

    uint64_t makeGsScissor(uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1)
    {
        return static_cast<uint64_t>(x0 & 0x7FFu) |
               (static_cast<uint64_t>(x1 & 0x7FFu) << 16u) |
               (static_cast<uint64_t>(y0 & 0x7FFu) << 32u) |
               (static_cast<uint64_t>(y1 & 0x7FFu) << 48u);
    }

    void appendPackedRgbaq(std::vector<uint8_t> &packet, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        appendU64(packet, static_cast<uint64_t>(r) | (static_cast<uint64_t>(g) << 32u));
        appendU64(packet, static_cast<uint64_t>(b) | (static_cast<uint64_t>(a) << 32u));
    }

    void appendPackedXyzf2(std::vector<uint8_t> &packet, uint32_t x, uint32_t y, uint32_t z)
    {
        appendU64(packet, static_cast<uint64_t>(x & 0xFFFFu) | (static_cast<uint64_t>(y & 0xFFFFu) << 32u));
        appendU64(packet, static_cast<uint64_t>(z & 0xFFFFFFu) << 4u);
    }

    void appendPackedUv(std::vector<uint8_t> &packet, uint32_t u, uint32_t v)
    {
        appendU64(packet, static_cast<uint64_t>(u & 0x3FFFu) | (static_cast<uint64_t>(v & 0x3FFFu) << 32u));
        appendU64(packet, 0u);
    }

}

void register_ps2_memory_tests()
{
    MiniTest::Case("PS2Memory", [](TestCase &tc)
    {
        tc.Run("uncached aliases map to same RDRAM bytes", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            mem.write32(0x00001000u, 0xDEADBEEFu);
            t.Equals(mem.read32(0x00001000u), 0xDEADBEEFu, "base readback should match");
            t.Equals(mem.read32(0x20001000u), 0xDEADBEEFu, "0x2000_0000 alias should map to RDRAM");

            // 0x3010_0000 maps to physical 0x0010_0000 (AboutPS2 memory map).
            mem.write32(0x00101000u, 0xDEADBEEFu);
            t.Equals(mem.read32(0x30101000u), 0xDEADBEEFu, "0x3010_0000 accelerated alias should map to RDRAM");

            mem.write32(0x20002000u, 0x13579BDFu);
            t.Equals(mem.read32(0x00002000u), 0x13579BDFu, "writes through 0x2000 alias should land in base RDRAM");

            mem.write32(0x30103000u, 0x2468ACE0u);
            t.Equals(mem.read32(0x00103000u), 0x2468ACE0u, "writes through 0x3010 alias should land in base RDRAM");
        });

        tc.Run("translateAddress handles kseg and uncached aliases", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            t.Equals(mem.translateAddress(0x80001234u), 0x00001234u, "KSEG0 should map directly to physical");
            t.Equals(mem.translateAddress(0xA0005678u), 0x00005678u, "KSEG1 should map directly to physical");
            t.Equals(mem.translateAddress(0x20001234u), 0x00001234u, "0x2000 uncached alias should map to RAM");
            t.Equals(mem.translateAddress(0x30105678u), 0x00105678u, "0x3010 accelerated alias should map to RAM");
            t.Equals(mem.translateAddress(PS2_SCRATCHPAD_BASE + 0x123u), 0x123u, "scratchpad base should translate to local offset");
            t.Equals(mem.translateAddress(PS2_SCRATCHPAD_ALIAS_BASE + 0x123u), 0x123u, "0xF000 scratchpad alias should translate to local offset");
        });

        tc.Run("EE timer0 count advances from scheduler cycles and can be reset", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kTimer0Count = 0x10000000u;
            constexpr uint32_t kTimer0Mode = 0x10000010u;
            constexpr uint32_t kTimer0Compare = 0x10000020u;

            t.IsTrue(mem.writeIORegister(kTimer0Count, 0u), "timer count reset write should succeed");
            t.IsTrue(mem.writeIORegister(kTimer0Compare, 0xFFFFu), "timer compare write should succeed");
            t.IsTrue(mem.writeIORegister(kTimer0Mode, 0x82u), "timer mode write should be retained");
            t.Equals(mem.readIORegister(kTimer0Mode), 0x82u, "timer mode should be readable");

            mem.advanceEeTimers(8u * 512u);
            const uint32_t firstCount = mem.readIORegister(kTimer0Count);
            t.Equals(firstCount, 8u, "BUSCLK/256 should increment once per 512 EE cycles");

            t.IsTrue(mem.writeIORegister(kTimer0Count, 0u), "timer count second reset should succeed");
            mem.advanceEeTimers(512u);
            const uint32_t resetCount = mem.readIORegister(kTimer0Count);
            t.Equals(resetCount, 1u, "timer reset should restart the deterministic count window");
        });

        tc.Run("EE timers 0 through 3 expose independent COUNT MODE and COMP registers", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kTimerBases[] = {
                0x10000000u,
                0x10000800u,
                0x10001000u,
                0x10001800u,
            };
            constexpr uint32_t kBusClockDiv256Cue = 0x82u;
            for (uint32_t index = 0u; index < 4u; ++index)
            {
                const uint32_t base = kTimerBases[index];
                t.IsTrue(mem.writeIORegister(base, 0x100u + index), "timer COUNT write should succeed");
                t.IsTrue(mem.writeIORegister(base + 0x10u, kBusClockDiv256Cue), "timer MODE write should succeed");
                t.IsTrue(mem.writeIORegister(base + 0x20u, 0x200u + index), "timer COMP write should succeed");
            }

            mem.advanceEeTimers(512u);
            for (uint32_t index = 0u; index < 4u; ++index)
            {
                const uint32_t base = kTimerBases[index];
                t.Equals(mem.readIORegister(base), 0x101u + index, "each timer should advance its own COUNT");
                t.Equals(mem.readIORegister(base + 0x10u), kBusClockDiv256Cue, "each timer should retain MODE");
                t.Equals(mem.readIORegister(base + 0x20u), 0x200u + index, "each timer should retain COMP");
            }

            t.IsTrue(mem.writeIORegister(kTimerBases[0] + 0x30u, 0x12345u), "Timer0 HOLD write should succeed");
            t.IsTrue(mem.writeIORegister(kTimerBases[1] + 0x30u, 0x23456u), "Timer1 HOLD write should succeed");
            t.Equals(mem.readIORegister(kTimerBases[0] + 0x30u), 0x2345u, "Timer0 HOLD should be 16-bit");
            t.Equals(mem.readIORegister(kTimerBases[1] + 0x30u), 0x3456u, "Timer1 HOLD should be 16-bit");
        });

        tc.Run("EE Timer2 compare and overflow flags raise INTC_TIM2 and clear on write-one", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kTimer2Count = 0x10001000u;
            constexpr uint32_t kTimer2Mode = 0x10001010u;
            constexpr uint32_t kTimer2Compare = 0x10001020u;
            constexpr uint32_t kCue = 1u << 7u;
            constexpr uint32_t kCmpe = 1u << 8u;
            constexpr uint32_t kOvfe = 1u << 9u;
            constexpr uint32_t kEquf = 1u << 10u;
            constexpr uint32_t kOvff = 1u << 11u;
            constexpr uint32_t kBusClockDiv256 = 2u;

            mem.writeIORegister(kTimer2Count, 0u);
            mem.writeIORegister(kTimer2Compare, 8u);
            mem.writeIORegister(kTimer2Mode, kBusClockDiv256 | kCue | kCmpe | kEquf | kOvff);

            t.Equals(mem.advanceEeTimers(7u * 512u), 0u, "compare should not fire before COUNT reaches COMP");
            t.Equals(mem.readIORegister(kTimer2Count), 7u, "Timer2 should expose its live 16-bit count");
            t.Equals(mem.advanceEeTimers(512u), 1u << 2u, "Timer2 compare should raise the TIM2 interrupt bit");
            t.IsTrue((mem.readIORegister(kTimer2Mode) & kEquf) != 0u, "Timer2 compare should latch EQUF");

            mem.writeIORegister(kTimer2Mode, mem.readIORegister(kTimer2Mode) | kEquf);
            t.IsTrue((mem.readIORegister(kTimer2Mode) & kEquf) == 0u, "writing one should clear EQUF");

            mem.writeIORegister(kTimer2Count, 0xFFFFu);
            mem.writeIORegister(kTimer2Mode, kBusClockDiv256 | kCue | kOvfe | kOvff);
            t.Equals(mem.advanceEeTimers(512u), 1u << 2u, "Timer2 overflow should raise the TIM2 interrupt bit");
            t.Equals(mem.readIORegister(kTimer2Count), 0u, "Timer2 count should wrap at 16 bits");
            t.IsTrue((mem.readIORegister(kTimer2Mode) & kOvff) != 0u, "Timer2 overflow should latch OVFF");

            mem.writeIORegister(kTimer2Mode, mem.readIORegister(kTimer2Mode) | kOvff);
            t.IsTrue((mem.readIORegister(kTimer2Mode) & kOvff) == 0u, "writing one should clear OVFF");
        });

        tc.Run("EE timer zero-return clears COUNT on compare", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kTimer0Count = 0x10000000u;
            constexpr uint32_t kTimer0Mode = 0x10000010u;
            constexpr uint32_t kTimer0Compare = 0x10000020u;
            constexpr uint32_t kZret = 1u << 6u;
            constexpr uint32_t kCue = 1u << 7u;
            constexpr uint32_t kCmpe = 1u << 8u;
            constexpr uint32_t kEquf = 1u << 10u;

            mem.writeIORegister(kTimer0Count, 0u);
            mem.writeIORegister(kTimer0Compare, 3u);
            mem.writeIORegister(kTimer0Mode, kZret | kCue | kCmpe | kEquf);

            t.Equals(mem.advanceEeTimers(6u), 1u, "Timer0 compare should raise TIM0 after three BUSCLK ticks");
            t.Equals(mem.readIORegister(kTimer0Count), 0u, "ZRET should clear COUNT when it equals COMP");
        });

        tc.Run("scratchpad alias accesses the same bytes as base", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kOffset = 0x140u;
            constexpr uint32_t kScratchAddr = PS2_SCRATCHPAD_BASE + kOffset;
            constexpr uint32_t kScratchAliasAddr = PS2_SCRATCHPAD_ALIAS_BASE + kOffset;

            mem.write32(kScratchAliasAddr, 0xCAFEBABEu);
            t.Equals(mem.read32(kScratchAddr), 0xCAFEBABEu, "writes through 0xF000 scratchpad alias should land in scratchpad");
            t.Equals(mem.read32(kScratchAliasAddr), 0xCAFEBABEu, "reads through 0xF000 scratchpad alias should see scratchpad bytes");
        });

        tc.Run("VU0 code and data windows map through EE addresses", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kCodeAddr = PS2_VU0_CODE_BASE + 0x20u;
            mem.write32(kCodeAddr, 0x11223344u);
            t.Equals(mem.read32(kCodeAddr), 0x11223344u, "VU0 code readback should match written word");

            uint32_t codeWord = 0u;
            std::memcpy(&codeWord, mem.getVU0Code() + 0x20u, sizeof(codeWord));
            t.Equals(codeWord, 0x11223344u, "VU0 code write should land in micro memory buffer");

            constexpr uint32_t kDataAddr = PS2_VU0_DATA_BASE + 0x30u;
            const __m128i value = _mm_set_epi32(0x44556677u, 0x01234567u, 0x89ABCDEFu, 0xCAFEBABEu);
            mem.write128(kDataAddr, value);

            alignas(16) uint32_t words[4]{};
            const __m128i readback = mem.read128(kDataAddr);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(words), readback);

            t.Equals(words[0], 0xCAFEBABEu, "VU0 data lane 0 should match");
            t.Equals(words[1], 0x89ABCDEFu, "VU0 data lane 1 should match");
            t.Equals(words[2], 0x01234567u, "VU0 data lane 2 should match");
            t.Equals(words[3], 0x44556677u, "VU0 data lane 3 should match");
        });

        tc.Run("fast memory helpers wrap safely at RAM boundary", [](TestCase &t)
        {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);
            const uint32_t tail = PS2_RAM_SIZE - 4u;

            // Build a wrapped 64-bit pattern: [tail..tail+3] + [0..3]
            rdram[tail + 0u] = 0xA1u;
            rdram[tail + 1u] = 0xB2u;
            rdram[tail + 2u] = 0xC3u;
            rdram[tail + 3u] = 0xD4u;
            rdram[0u] = 0x11u;
            rdram[1u] = 0x22u;
            rdram[2u] = 0x33u;
            rdram[3u] = 0x44u;

            const uint64_t wrappedRead = Ps2FastRead64(rdram.data(), tail);
            t.Equals(wrappedRead, 0x44332211D4C3B2A1ull,
                     "Ps2FastRead64 should wrap across the 32MB boundary");

            Ps2FastWrite64(rdram.data(), tail, 0x8877665544332211ull);
            t.Equals(static_cast<uint32_t>(rdram[tail + 0u]), 0x11u, "write byte 0 should land at tail+0");
            t.Equals(static_cast<uint32_t>(rdram[tail + 1u]), 0x22u, "write byte 1 should land at tail+1");
            t.Equals(static_cast<uint32_t>(rdram[tail + 2u]), 0x33u, "write byte 2 should land at tail+2");
            t.Equals(static_cast<uint32_t>(rdram[tail + 3u]), 0x44u, "write byte 3 should land at tail+3");
            t.Equals(static_cast<uint32_t>(rdram[0u]), 0x55u, "write byte 4 should wrap to address 0");
            t.Equals(static_cast<uint32_t>(rdram[1u]), 0x66u, "write byte 5 should wrap to address 1");
            t.Equals(static_cast<uint32_t>(rdram[2u]), 0x77u, "write byte 6 should wrap to address 2");
            t.Equals(static_cast<uint32_t>(rdram[3u]), 0x88u, "write byte 7 should wrap to address 3");
        });

        tc.Run("VIF MPG num zero uploads 256 instructions", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<uint8_t> packet;
            packet.reserve(4u + 2048u);
            appendU32(packet, makeVifCmd(0x4Au, 0u, 0u)); // MPG, num=0 -> 256 instructions (2048 bytes)

            for (uint32_t i = 0; i < 2048u; ++i)
            {
                packet.push_back(static_cast<uint8_t>(i & 0xFFu));
            }

            std::memset(mem.getVU1Code(), 0, PS2_VU1_CODE_SIZE);
            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu1Code = mem.getVU1Code();
            bool matches = true;
            for (uint32_t i = 0; i < 2048u; ++i)
            {
                if (vu1Code[i] != static_cast<uint8_t>(i & 0xFFu))
                {
                    matches = false;
                    break;
                }
            }
            t.IsTrue(matches, "MPG num=0 should copy 2048 bytes into VU1 code memory");
        });

        tc.Run("VIF UNPACK num zero uploads 256 vectors", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            // UNPACK V4_32: opcode 0x6C (vn=3, vl=0), num=0 => 256 vectors, 16 bytes each.
            std::vector<uint8_t> packet;
            packet.reserve(4u + 4096u);
            appendU32(packet, makeVifCmd(0x6Cu, 0u, 0u));
            for (uint32_t i = 0; i < 4096u; ++i)
            {
                packet.push_back(static_cast<uint8_t>((i * 3u) & 0xFFu));
            }

            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);
            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu1Data = mem.getVU1Data();
            bool matches = true;
            for (uint32_t i = 0; i < 4096u; ++i)
            {
                if (vu1Data[i] != static_cast<uint8_t>((i * 3u) & 0xFFu))
                {
                    matches = false;
                    break;
                }
            }
            t.IsTrue(matches, "UNPACK num=0 should copy 256 V4_32 vectors (4096 bytes)");
        });

        tc.Run("VIF control commands update MARK MASK ROW and COL registers", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x07u, 0u, 0x1234u)); // MARK

            appendU32(packet, makeVifCmd(0x20u, 0u, 0u));      // STMASK
            appendU32(packet, 0x89ABCDEFu);

            appendU32(packet, makeVifCmd(0x30u, 0u, 0u));      // STROW
            appendU32(packet, 0x11111111u);
            appendU32(packet, 0x22222222u);
            appendU32(packet, 0x33333333u);
            appendU32(packet, 0x44444444u);

            appendU32(packet, makeVifCmd(0x31u, 0u, 0u));      // STCOL
            appendU32(packet, 0xAAAA0001u);
            appendU32(packet, 0xAAAA0002u);
            appendU32(packet, 0xAAAA0003u);
            appendU32(packet, 0xAAAA0004u);

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            t.Equals(mem.vif1_regs.mark, 0x1234u, "MARK should set VIF1 MARK register");
            t.Equals(mem.vif1_regs.mask, 0x89ABCDEFu, "STMASK should set VIF1 MASK register");

            t.Equals(mem.vif1_regs.row[0], 0x11111111u, "STROW should set row[0]");
            t.Equals(mem.vif1_regs.row[1], 0x22222222u, "STROW should set row[1]");
            t.Equals(mem.vif1_regs.row[2], 0x33333333u, "STROW should set row[2]");
            t.Equals(mem.vif1_regs.row[3], 0x44444444u, "STROW should set row[3]");

            t.Equals(mem.vif1_regs.col[0], 0xAAAA0001u, "STCOL should set col[0]");
            t.Equals(mem.vif1_regs.col[1], 0xAAAA0002u, "STCOL should set col[1]");
            t.Equals(mem.vif1_regs.col[2], 0xAAAA0003u, "STCOL should set col[2]");
            t.Equals(mem.vif1_regs.col[3], 0xAAAA0004u, "STCOL should set col[3]");
        });

        tc.Run("VIF UNPACK V4-16 sign and zero extension follow immediate bit14", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            // UNPACK V4-16 (opcode 0x6D), num=1, addr=0.
            // Payload components: x=0xFF80, y=0x0001, z=0x7FFF, w=0x8001.
            const uint16_t comps[4] = {0xFF80u, 0x0001u, 0x7FFFu, 0x8001u};

            std::vector<uint8_t> signPacket;
            appendU32(signPacket, makeVifCmd(0x6Du, 1u, 0x0000u)); // sign-extend
            for (uint16_t c : comps)
            {
                const size_t pos = signPacket.size();
                signPacket.resize(pos + sizeof(uint16_t));
                std::memcpy(signPacket.data() + pos, &c, sizeof(uint16_t));
            }
            mem.processVIF1Data(signPacket.data(), static_cast<uint32_t>(signPacket.size()));

            const uint8_t *vu1 = mem.getVU1Data();
            uint32_t sx = 0, sy = 0, sz = 0, sw = 0;
            std::memcpy(&sx, vu1 + 0, 4);
            std::memcpy(&sy, vu1 + 4, 4);
            std::memcpy(&sz, vu1 + 8, 4);
            std::memcpy(&sw, vu1 + 12, 4);
            t.Equals(sx, 0xFFFFFF80u, "sign-extend x");
            t.Equals(sy, 0x00000001u, "sign-extend y");
            t.Equals(sz, 0x00007FFFu, "sign-extend z");
            t.Equals(sw, 0xFFFF8001u, "sign-extend w");

            // Same UNPACK with imm bit14 set => zero-extend.
            std::vector<uint8_t> zeroPacket;
            appendU32(zeroPacket, makeVifCmd(0x6Du, 1u, 0x4000u)); // zero-extend
            for (uint16_t c : comps)
            {
                const size_t pos = zeroPacket.size();
                zeroPacket.resize(pos + sizeof(uint16_t));
                std::memcpy(zeroPacket.data() + pos, &c, sizeof(uint16_t));
            }
            mem.processVIF1Data(zeroPacket.data(), static_cast<uint32_t>(zeroPacket.size()));

            std::memcpy(&sx, vu1 + 0, 4);
            std::memcpy(&sy, vu1 + 4, 4);
            std::memcpy(&sz, vu1 + 8, 4);
            std::memcpy(&sw, vu1 + 12, 4);
            t.Equals(sx, 0x0000FF80u, "zero-extend x");
            t.Equals(sy, 0x00000001u, "zero-extend y");
            t.Equals(sz, 0x00007FFFu, "zero-extend z");
            t.Equals(sw, 0x00008001u, "zero-extend w");
        });

        tc.Run("VIF V2 unpacks expand both pairs for every component width", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "memory should initialize");
            for (uint32_t vl = 0; vl < 3; ++vl)
                for (uint32_t usn = 0; usn < 2; ++usn)
                {
                    std::memset(mem.getVU1Data(), 0xa5, 32);
                    std::vector<uint8_t> packet;
                    appendU32(packet, makeVifCmd(uint8_t(0x64 + vl), 2, uint16_t(usn << 14)));
                    const uint32_t bytes = 4u >> vl;
                    for (uint32_t i = 0; i < 4; ++i)
                    {
                        const uint32_t raw = i % 2 == 0 ? 0xfffffffdu : 2u;
                        const size_t pos = packet.size();
                        packet.resize(pos + bytes);
                        std::memcpy(packet.data() + pos, &raw, bytes);
                    }
                    mem.processVIF1Data(packet.data(), uint32_t(packet.size()));
                    const uint32_t negative = !usn || vl == 0 ? 0xfffffffdu :
                                              vl == 1 ? 0xfffdu : 0xfdu;
                    for (uint32_t v = 0; v < 2; ++v)
                    {
                        uint32_t lanes[4];
                        std::memcpy(lanes, mem.getVU1Data() + v * 16, 16);
                        t.Equals(lanes[0], negative, "X sign/zero extension");
                        t.Equals(lanes[1], 2u, "Y extension");
                        t.Equals(lanes[2], negative, "Z must repeat X");
                        t.Equals(lanes[3], 2u, "W must repeat Y");
                    }
                }
        });

        tc.Run("VIF V2 replication precedes row addition and write protection", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "memory should initialize");
            std::memset(mem.getVU1Data(), 0xa5, 16);
            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x05, 0, 1)); // STMOD: row addition
            appendU32(packet, makeVifCmd(0x30, 0, 0));
            for (uint32_t row : {10u, 20u, 30u, 40u}) appendU32(packet, row);
            appendU32(packet, makeVifCmd(0x20, 0, 0));
            appendU32(packet, 0x4cu); // protect Y, select row W
            appendU32(packet, makeVifCmd(0x74, 1, 0)); // masked V2-32
            appendU32(packet, 3); appendU32(packet, 5);
            mem.processVIF1Data(packet.data(), uint32_t(packet.size()));
            uint32_t lanes[4];
            std::memcpy(lanes, mem.getVU1Data(), 16);
            t.Equals(lanes[0], 13u, "X adds row X");
            t.Equals(lanes[1], 0xa5a5a5a5u, "Y stays protected");
            t.Equals(lanes[2], 33u, "replicated X adds row Z");
            t.Equals(lanes[3], 40u, "W selects row W without addition");
        });

        tc.Run("VIF V4-5 expands RGB and alpha to their upper bits without STMOD", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "memory should initialize");
            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x05, 0, 2));
            appendU32(packet, makeVifCmd(0x30, 0, 0));
            for (uint32_t row : {10u, 20u, 30u, 40u}) appendU32(packet, row);
            appendU32(packet, makeVifCmd(0x6f, 2, 0));
            const uint32_t rgb = 31u | (17u << 5) | (9u << 10);
            appendU32(packet, (rgb | 0x8000u) | (rgb << 16));
            mem.processVIF1Data(packet.data(), uint32_t(packet.size()));
            uint32_t lanes[8];
            std::memcpy(lanes, mem.getVU1Data(), sizeof(lanes));
            for (uint32_t v = 0; v < 2; ++v)
            {
                t.Equals(lanes[v*4], 248u, "red shifts by three");
                t.Equals(lanes[v*4+1], 136u, "green shifts by three");
                t.Equals(lanes[v*4+2], 72u, "blue shifts by three");
                t.Equals(lanes[v*4+3], v == 0 ? 128u : 0u, "alpha shifts to bit seven");
            }
            t.Equals(mem.vif1_regs.row[0], 10u, "V4-5 must not update STMOD row state");
        });

        tc.Run("VIF UNPACK bit15 adds TOPS to destination address", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            mem.vif1_regs.tops = 4u;

            // UNPACK V4-32, num=1, addr=2, bit15 set => effective addr = 6.
            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x6Cu, 1u, static_cast<uint16_t>(0x8000u | 0x0002u)));
            appendU32(packet, 0x11111111u);
            appendU32(packet, 0x22222222u);
            appendU32(packet, 0x33333333u);
            appendU32(packet, 0x44444444u);

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu1 = mem.getVU1Data();

            uint32_t untouched = 0xDEADBEEFu;
            std::memcpy(&untouched, vu1 + (2u * 16u), 4);
            t.Equals(untouched, 0u, "base addr without TOPS should remain untouched");

            uint32_t x = 0, y = 0, z = 0, w = 0;
            const uint32_t dest = 6u * 16u;
            std::memcpy(&x, vu1 + dest + 0u, 4);
            std::memcpy(&y, vu1 + dest + 4u, 4);
            std::memcpy(&z, vu1 + dest + 8u, 4);
            std::memcpy(&w, vu1 + dest + 12u, 4);
            t.Equals(x, 0x11111111u, "TOPS-adjusted x");
            t.Equals(y, 0x22222222u, "TOPS-adjusted y");
            t.Equals(z, 0x33333333u, "TOPS-adjusted z");
            t.Equals(w, 0x44444444u, "TOPS-adjusted w");
        });

        tc.Run("VIF STCYCL skip mode advances destination by CL when CL>=WL", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x01u, 0u, static_cast<uint16_t>((1u << 8) | 3u))); // STCYCL: WL=1, CL=3
            appendU32(packet, makeVifCmd(0x6Cu, 2u, 0u)); // UNPACK V4-32, NUM=2, ADDR=0

            appendU32(packet, 0x11111111u);
            appendU32(packet, 0x22222222u);
            appendU32(packet, 0x33333333u);
            appendU32(packet, 0x44444444u);

            appendU32(packet, 0xAAAAAAAAu);
            appendU32(packet, 0xBBBBBBBBu);
            appendU32(packet, 0xCCCCCCCCu);
            appendU32(packet, 0xDDDDDDDDu);

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu = mem.getVU1Data();

            uint32_t v0x = 0, v1x = 0, v2x = 0, v3x = 0;
            std::memcpy(&v0x, vu + 0u * 16u + 0u, 4);
            std::memcpy(&v1x, vu + 1u * 16u + 0u, 4);
            std::memcpy(&v2x, vu + 2u * 16u + 0u, 4);
            std::memcpy(&v3x, vu + 3u * 16u + 0u, 4);

            t.Equals(v0x, 0x11111111u, "first vector should write at addr 0");
            t.Equals(v1x, 0u, "skip mode should leave addr 1 untouched when WL=1 CL=3");
            t.Equals(v2x, 0u, "skip mode should leave addr 2 untouched when WL=1 CL=3");
            t.Equals(v3x, 0xAAAAAAAAu, "second vector should write at addr CL (addr 3)");
        });

        tc.Run("VIF masked UNPACK uses data row col and protect selectors", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            // Pre-fill destination W lane for write-protect verification.
            uint32_t preservedW = 0xDEADBEEFu;
            std::memcpy(mem.getVU1Data() + 12u, &preservedW, 4u);

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x20u, 0u, 0u)); // STMASK
            appendU32(packet, 0x000000E4u); // m0=0(data), m1=1(row), m2=2(col), m3=3(protect)

            appendU32(packet, makeVifCmd(0x30u, 0u, 0u)); // STROW
            appendU32(packet, 0xAAAAB001u);
            appendU32(packet, 0xAAAAB002u);
            appendU32(packet, 0xAAAAB003u);
            appendU32(packet, 0xAAAAB004u);

            appendU32(packet, makeVifCmd(0x31u, 0u, 0u)); // STCOL
            appendU32(packet, 0x11110001u);
            appendU32(packet, 0x11110002u);
            appendU32(packet, 0x11110003u);
            appendU32(packet, 0x11110004u);

            appendU32(packet, makeVifCmd(0x7Cu, 1u, 0u)); // UNPACK V4-32 with CMD bit4 (mask enable)
            appendU32(packet, 0x01020304u);
            appendU32(packet, 0x11121314u);
            appendU32(packet, 0x21222324u);
            appendU32(packet, 0x31323334u);

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu = mem.getVU1Data();
            uint32_t x = 0, y = 0, z = 0, w = 0;
            std::memcpy(&x, vu + 0u, 4u);
            std::memcpy(&y, vu + 4u, 4u);
            std::memcpy(&z, vu + 8u, 4u);
            std::memcpy(&w, vu + 12u, 4u);

            t.Equals(x, 0x01020304u, "mask=0 should write decompressed data");
            t.Equals(y, 0xAAAAB002u, "mask=1 should write row register for Y field");
            t.Equals(z, 0x11110001u, "mask=2 should write C0 on first write cycle");
            t.Equals(w, preservedW, "mask=3 should write-protect destination field");
        });

        tc.Run("VIF STMOD offset and difference modes apply to UNPACK data", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x30u, 0u, 0u)); // STROW
            appendU32(packet, 10u);
            appendU32(packet, 20u);
            appendU32(packet, 30u);
            appendU32(packet, 40u);

            appendU32(packet, makeVifCmd(0x05u, 0u, 1u)); // STMOD offset mode
            appendU32(packet, makeVifCmd(0x6Cu, 1u, 0u)); // UNPACK V4-32 -> addr 0
            appendU32(packet, 1u);
            appendU32(packet, 2u);
            appendU32(packet, 3u);
            appendU32(packet, 4u);

            appendU32(packet, makeVifCmd(0x30u, 0u, 0u)); // reset STROW for difference mode
            appendU32(packet, 100u);
            appendU32(packet, 100u);
            appendU32(packet, 100u);
            appendU32(packet, 100u);

            appendU32(packet, makeVifCmd(0x05u, 0u, 2u)); // STMOD difference mode
            appendU32(packet, makeVifCmd(0x6Cu, 2u, 1u)); // UNPACK V4-32 -> addr 1 and 2
            appendU32(packet, 1u);
            appendU32(packet, 1u);
            appendU32(packet, 1u);
            appendU32(packet, 1u);
            appendU32(packet, 2u);
            appendU32(packet, 2u);
            appendU32(packet, 2u);
            appendU32(packet, 2u);

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu = mem.getVU1Data();
            uint32_t x0 = 0, y0 = 0, z0 = 0, w0 = 0;
            std::memcpy(&x0, vu + 0u * 16u + 0u, 4u);
            std::memcpy(&y0, vu + 0u * 16u + 4u, 4u);
            std::memcpy(&z0, vu + 0u * 16u + 8u, 4u);
            std::memcpy(&w0, vu + 0u * 16u + 12u, 4u);
            t.Equals(x0, 11u, "offset mode X");
            t.Equals(y0, 22u, "offset mode Y");
            t.Equals(z0, 33u, "offset mode Z");
            t.Equals(w0, 44u, "offset mode W");

            uint32_t x1 = 0, x2 = 0;
            std::memcpy(&x1, vu + 1u * 16u + 0u, 4u);
            std::memcpy(&x2, vu + 2u * 16u + 0u, 4u);
            t.Equals(x1, 101u, "difference mode first write should add initial row");
            t.Equals(x2, 103u, "difference mode second write should accumulate updated row");
            t.Equals(mem.vif1_regs.row[0], 103u, "difference mode should update row register");
            t.Equals(mem.vif1_regs.row[1], 103u, "difference mode should update row register for Y");
            t.Equals(mem.vif1_regs.row[2], 103u, "difference mode should update row register for Z");
            t.Equals(mem.vif1_regs.row[3], 103u, "difference mode should update row register for W");
        });

        // GOW-Port: los tiempos del GS anidado no deben sumarse también a VU/EE.
        tc.Run("Perf scopes account nested work exclusively", [](TestCase &t)
        {
            using namespace ps2_perf;
            Accounting clock;
            std::array<uint64_t, bucketCount> totals{};
            const auto step = [&](Bucket bucket, uint64_t now) {
                const auto charge = clock.switchTo(bucket, now);
                if (charge.bucket != Bucket::None) totals[static_cast<size_t>(charge.bucket)] += charge.nanos;
            };
            step(Bucket::Ee, 0); step(Bucket::Vu, 10); step(Bucket::Gs, 30);
            step(Bucket::Vu, 35); step(Bucket::Ee, 50); step(Bucket::None, 80);
            t.Equals(totals[0], uint64_t{40}, "EE excludes nested VU and GS");
            t.Equals(totals[1], uint64_t{35}, "VU excludes nested GS");
            t.Equals(totals[2], uint64_t{5}, "GS is charged only once");
            t.Equals(totals[0]+totals[1]+totals[2], uint64_t{80}, "exclusive sum equals elapsed time");
        });
        tc.Run("Perf accounting ignores unmeasured gaps and supports repeated buckets", [](TestCase &t)
        {
            using namespace ps2_perf;
            Accounting clock;
            t.IsTrue(clock.switchTo(Bucket::Gs, 100).bucket == Bucket::None, "startup gap is unmeasured");
            const auto recursive = clock.switchTo(Bucket::Gs, 120);
            const auto returned = clock.switchTo(Bucket::Gs, 130);
            const auto done = clock.switchTo(Bucket::None, 160);
            t.Equals(recursive.nanos + returned.nanos + done.nanos, uint64_t{60}, "same bucket nesting does not double elapsed time");
            t.IsTrue(clock.switchTo(Bucket::Ee, 1000).bucket == Bucket::None, "gap between tasks is unmeasured");
        });

        tc.Run("VIF fill write uses STMASK and STROW when WL>CL", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x01u, 0u, static_cast<uint16_t>((3u << 8) | 1u))); // STCYCL: WL=3, CL=1

            appendU32(packet, makeVifCmd(0x20u, 0u, 0u)); // STMASK
            appendU32(packet, 0x55555555u); // all fields all cycles use row register

            appendU32(packet, makeVifCmd(0x30u, 0u, 0u)); // STROW
            appendU32(packet, 0x11111111u);
            appendU32(packet, 0x22222222u);
            appendU32(packet, 0x33333333u);
            appendU32(packet, 0x44444444u);

            appendU32(packet, makeVifCmd(0x7Cu, 3u, 0u)); // masked UNPACK V4-32, NUM=3 writes
            // Only one input vector should be consumed for CL=1, WL=3.
            appendU32(packet, 0xAAAABBBB);
            appendU32(packet, 0xCCCCDDDD);
            appendU32(packet, 0xEEEEFFFF);
            appendU32(packet, 0x12345678);

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu = mem.getVU1Data();
            for (uint32_t i = 0; i < 3u; ++i)
            {
                uint32_t x = 0, y = 0, z = 0, w = 0;
                std::memcpy(&x, vu + i * 16u + 0u, 4u);
                std::memcpy(&y, vu + i * 16u + 4u, 4u);
                std::memcpy(&z, vu + i * 16u + 8u, 4u);
                std::memcpy(&w, vu + i * 16u + 12u, 4u);
                t.Equals(x, 0x11111111u, "fill write X should use row[0]");
                t.Equals(y, 0x22222222u, "fill write Y should use row[1]");
                t.Equals(z, 0x33333333u, "fill write Z should use row[2]");
                t.Equals(w, 0x44444444u, "fill write W should use row[3]");
            }
        });

        tc.Run("VIF irq command sets STAT.INT and CODE until FBRST.STC clears it", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            const uint32_t irqMarkCmd = 0x80000000u | makeVifCmd(0x07u, 0x12u, 0x3456u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&irqMarkCmd), sizeof(irqMarkCmd));

            t.Equals(mem.vif1_regs.code, irqMarkCmd, "VIF CODE should capture the last processed command");
            t.IsTrue((mem.vif1_regs.stat & (1u << 11)) != 0u, "irq bit should raise VIF1 STAT.INT");
            t.Equals(mem.vif1_regs.mark, 0x3456u, "MARK command should still update MARK register");

            t.IsTrue(mem.writeIORegister(0x10003C10u, 0x8u), "FBRST STC write should succeed");
            t.IsTrue((mem.vif1_regs.stat & (1u << 11)) == 0u, "FBRST.STC should clear VIF1 STAT.INT");
        });

        tc.Run("VIF FBRST RST clears VIF1 command state", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            mem.vif1_regs.mark = 0x1234u;
            mem.vif1_regs.cycle = 0x0102u;
            mem.vif1_regs.mode = 2u;
            mem.vif1_regs.num = 7u;
            mem.vif1_regs.mask = 0x89ABCDEFu;
            mem.vif1_regs.code = 0xCAFEBABEu;
            mem.vif1_regs.stat = 0x3F00u;

            t.IsTrue(mem.writeIORegister(0x10003C10u, 0x1u), "FBRST RST write should succeed");

            t.Equals(mem.vif1_regs.mark, 0u, "RST should clear MARK");
            t.Equals(mem.vif1_regs.cycle, 0u, "RST should clear CYCLE");
            t.Equals(mem.vif1_regs.mode, 0u, "RST should clear MODE");
            t.Equals(mem.vif1_regs.num, 0u, "RST should clear NUM");
            t.Equals(mem.vif1_regs.mask, 0u, "RST should clear MASK");
            t.Equals(mem.vif1_regs.code, 0u, "RST should clear CODE");
            t.Equals(mem.vif1_regs.stat, 0u, "RST should clear STAT");
        });

        tc.Run("VIF double-buffer OFFSET BASE and MSCAL update TOPS and ITOPS", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            mem.vif1_regs.base = 0x120u;
            mem.vif1_regs.tops = 0x120u;
            mem.vif1_regs.stat = (1u << 7); // DBF=1 before OFFSET

            struct MscalCall
            {
                uint32_t startPC;
                uint32_t top;
                uint32_t itop;
            };
            std::vector<MscalCall> mscalCalls;
            mem.setVu1MscalCallback([&](uint32_t startPC, uint32_t top, uint32_t itop)
            {
                mscalCalls.push_back({startPC, top, itop});
            });

            const uint32_t offsetCmd = makeVifCmd(0x02u, 0u, 0x0022u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&offsetCmd), sizeof(offsetCmd));
            t.Equals(mem.vif1_regs.ofst, 0x22u, "OFFSET should update OFST");
            t.Equals(mem.vif1_regs.base, 0x120u, "OFFSET should copy old TOPS into BASE");
            t.IsTrue((mem.vif1_regs.stat & (1u << 7)) == 0u, "OFFSET should clear DBF");
            t.Equals(mem.vif1_regs.tops, 0x120u, "DBF=0 should keep TOPS at BASE");

            const uint32_t baseCmd = makeVifCmd(0x03u, 0u, 0x0030u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&baseCmd), sizeof(baseCmd));
            t.Equals(mem.vif1_regs.base, 0x30u, "BASE should update BASE register");
            t.Equals(mem.vif1_regs.tops, 0x120u, "BASE should not rewrite current TOPS");

            const uint32_t itopCmd = makeVifCmd(0x04u, 0u, 0x0044u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&itopCmd), sizeof(itopCmd));
            t.Equals(mem.vif1_regs.itops, 0x44u, "ITOP VIFcode should update pending ITOPS register");

            const uint32_t mscalCmd = makeVifCmd(0x14u, 0u, 0x0003u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&mscalCmd), sizeof(mscalCmd));
            t.Equals(mscalCalls.size(), static_cast<size_t>(1u), "MSCAL should invoke callback once");
            t.Equals(mscalCalls[0].startPC, 0x18u, "MSCAL callback startPC should be IMMEDIATE*8");
            t.Equals(mscalCalls[0].top, 0x120u, "MSCAL callback should receive current TOPS");
            t.Equals(mscalCalls[0].itop, 0x44u, "MSCAL callback should receive pending ITOPS");
            t.Equals(mem.vif1_regs.top, 0x120u, "MSCAL should latch TOP from TOPS");
            t.Equals(mem.vif1_regs.itop, 0x44u, "MSCAL should latch ITOP from ITOPS");
            t.IsTrue((mem.vif1_regs.stat & (1u << 7)) != 0u, "MSCAL should toggle DBF");
            t.Equals(mem.vif1_regs.tops, 0x52u, "DBF=1 should set TOPS to BASE+OFST");

            const uint32_t mscntCmd = makeVifCmd(0x17u, 0u, 0u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&mscntCmd), sizeof(mscntCmd));
            t.IsTrue((mem.vif1_regs.stat & (1u << 7)) == 0u, "MSCNT should toggle DBF again");
            t.Equals(mem.vif1_regs.tops, 0x30u, "DBF=0 should restore TOPS to BASE");
        });

        tc.Run("VIF MSKPATH3 uses immediate bit15", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            const uint32_t setMask = makeVifCmd(0x06u, 0u, 0x8000u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&setMask), sizeof(setMask));
            t.IsTrue(mem.isPath3Masked(), "MSKPATH3 with imm bit15 set should enable PATH3 mask");

            const uint32_t clearMask = makeVifCmd(0x06u, 0u, 0x0000u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&clearMask), sizeof(clearMask));
            t.IsFalse(mem.isPath3Masked(), "MSKPATH3 with imm bit15 clear should disable PATH3 mask");
        });

        tc.Run("VIF1 FIFO MSKPATH3 is visible through GIF_STAT", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            const uint32_t setMask = makeVifCmd(0x06u, 0u, 0x8000u);
            const __m128i setPacket = _mm_set_epi32(0, 0, 0, static_cast<int32_t>(setMask));
            mem.write128(0x10005000u, setPacket);
            t.IsTrue(mem.isPath3Masked(), "a direct VIF1 FIFO command should execute MSKPATH3");
            t.IsTrue((mem.readIORegister(0x10003020u) & 0x2u) != 0u,
                     "GIF_STAT.M3P should report the VIF1 PATH3 mask");

            const uint32_t clearMask = makeVifCmd(0x06u, 0u, 0x0000u);
            const __m128i clearPacket = _mm_set_epi32(0, 0, 0, static_cast<int32_t>(clearMask));
            mem.write128(0x10005000u, clearPacket);
            t.IsFalse(mem.isPath3Masked(), "a direct VIF1 FIFO command should clear MSKPATH3");
            t.IsTrue((mem.readIORegister(0x10003020u) & 0x2u) == 0u,
                     "GIF_STAT.M3P should clear with the VIF1 PATH3 mask");

            mem.writeIORegister(0x10003010u, 0x5u); // GIF_MODE: M3R | IMT
            mem.writeIORegister(0x10003000u, 0x8u); // GIF_CTRL: PSE
            t.Equals(mem.readIORegister(0x10003020u) & 0xDu, 0xDu,
                     "GIF_STAT should mirror GIF_MODE and GIF_CTRL status bits");
        });


        tc.Run("PATH3 mask queues packets until unmask", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            std::vector<uint8_t> packetA(16u);
            std::vector<uint8_t> packetB(16u);
            for (uint32_t i = 0; i < 16u; ++i)
            {
                packetA[i] = static_cast<uint8_t>(0x10u + i);
                packetB[i] = static_cast<uint8_t>(0x40u + i);
            }

            const uint32_t setMask = makeVifCmd(0x06u, 0u, 0x8000u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&setMask), sizeof(setMask));
            t.IsTrue(mem.isPath3Masked(), "PATH3 mask should be enabled");

            mem.submitGifPacket(GifPathId::Path3, packetA.data(), static_cast<uint32_t>(packetA.size()));
            mem.submitGifPacket(GifPathId::Path3, packetB.data(), static_cast<uint32_t>(packetB.size()));
            t.Equals(captured.size(), static_cast<size_t>(0u), "masked PATH3 packets should be queued, not dropped/emitted");

            const uint32_t clearMask = makeVifCmd(0x06u, 0u, 0x0000u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&clearMask), sizeof(clearMask));

            t.Equals(captured.size(), static_cast<size_t>(2u), "unmask should flush queued PATH3 packets");
            bool firstOk = true;
            bool secondOk = true;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                if (captured[0][i] != static_cast<uint8_t>(0x10u + i))
                    firstOk = false;
                if (captured[1][i] != static_cast<uint8_t>(0x40u + i))
                    secondOk = false;
            }
            t.IsTrue(firstOk, "first queued PATH3 packet should flush in-order");
            t.IsTrue(secondOk, "second queued PATH3 packet should flush in-order");
        });

        tc.Run("GIF arbiter prioritizes PATH1 then PATH2 then PATH3", [](TestCase &t)
        {
            std::vector<uint8_t> order;
            GifArbiter arbiter([&](const uint8_t *data, uint32_t sizeBytes)
            {
                if (data && sizeBytes > 0u)
                    order.push_back(data[0]);
            });

            const std::vector<uint8_t> p1(16u, 0x11u);
            const std::vector<uint8_t> p2(16u, 0x22u);
            const std::vector<uint8_t> p3(16u, 0x33u);

            arbiter.submit(GifPathId::Path3, p3.data(), static_cast<uint32_t>(p3.size()));
            arbiter.submit(GifPathId::Path2, p2.data(), static_cast<uint32_t>(p2.size()));
            arbiter.submit(GifPathId::Path1, p1.data(), static_cast<uint32_t>(p1.size()));
            arbiter.drain();

            t.Equals(order.size(), static_cast<size_t>(3u), "all queued packets should be drained");
            t.Equals(order[0], static_cast<uint8_t>(0x11u), "PATH1 should be drained first");
            t.Equals(order[1], static_cast<uint8_t>(0x22u), "PATH2 should be drained second");
            t.Equals(order[2], static_cast<uint8_t>(0x33u), "PATH3 should be drained third");
        });

        tc.Run("VIF DIRECTHL stalls behind queued PATH3 IMAGE packets", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<uint8_t> firstBytes;
            GifArbiter arbiter([&](const uint8_t *data, uint32_t sizeBytes)
            {
                if (data && sizeBytes > 0u)
                    firstBytes.push_back(data[0]);
            });
            mem.setGifArbiter(&arbiter);

            std::vector<uint8_t> path3Image;
            appendU64(path3Image, makeGifTag(0x00AAu, 2u, 0u, true)); // IMAGE packet marker: first byte 0xAA
            appendU64(path3Image, 0ull);
            mem.submitGifPacket(GifPathId::Path3, path3Image.data(), static_cast<uint32_t>(path3Image.size()), false);

            std::vector<uint8_t> vifPacket;
            appendU32(vifPacket, makeVifCmd(0x51u, 0u, 1u)); // DIRECTHL 1 QW
            for (uint32_t i = 0; i < 16u; ++i)
            {
                vifPacket.push_back(static_cast<uint8_t>(0xD2u + i));
            }
            mem.processVIF1Data(vifPacket.data(), static_cast<uint32_t>(vifPacket.size()));

            t.Equals(firstBytes.size(), static_cast<size_t>(2u), "PATH3 and DIRECTHL packets should both drain");
            t.Equals(firstBytes[0], static_cast<uint8_t>(0xAAu), "DIRECTHL should not preempt queued PATH3 IMAGE packet");
            t.Equals(firstBytes[1], static_cast<uint8_t>(0xD2u), "DIRECTHL packet should drain after PATH3 IMAGE packet");
        });






















        // GOW-Port: God of War copia la paleta de huesos con fromSPR en cadena de destino (CHCR 0x104).



        tc.Run("sceDmaSend preserves guest-configured VIF1 TTE", [](TestCase &t)
        {
            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kTag = 0x00028500u;

            PS2Memory &mem = runtime.memory();
            uint8_t *rdram = mem.getRDRAM();
            writeDmaTag(rdram, kTag, makeDmaTag(0u, 7u, 0u, false)); // END
            const uint32_t itopCmd = makeVifCmd(0x04u, 0u, 0x66u);
            std::memcpy(rdram + kTag + 12u, &itopCmd, sizeof(itopCmd));

            // Fatal Frame follows this exact sequence: get channel, set CHCR.TTE,
            // then submit the chain through sceDmaSend.
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x40u),
                     "guest should be able to configure VIF1 CHCR.TTE before submission");

            R5900Context ctx{};
            setRegU32(ctx, 4, 1u); // sceDmaGetChan(1) / VIF1
            setRegU32(ctx, 5, kTag);
            ps2_stubs::sceDmaSend(rdram, &ctx, &runtime);

            t.Equals(static_cast<int32_t>(::getRegU32(&ctx, 2)), 0,
                     "sceDmaSend should accept the VIF1 chain");
            t.IsTrue((mem.readIORegister(kVif1Ch + 0x00u) & 0x40u) != 0u,
                     "sceDmaSend must preserve guest-configured CHCR.TTE");
            t.Equals(mem.vif1_regs.itops, 0x66u,
                     "preserved TTE should deliver the tag high-half VIFcode");
        });


        // GOW-Port: en el hardware la IMAGE continua con la carga del siguiente DIRECT, no con los codigos VIF
        // que siguen al primero. Las dos pruebas de abajo comprobaban el comportamiento anterior.

        // GOW-Port: una etiqueta PACKED tambien puede cruzar varios comandos DIRECT/HL.
        tc.Run("VIF1 DIRECT preserves a PACKED tag across separate commands and MARK", [](TestCase &t)
        {
            PS2Memory mem; t.IsTrue(mem.initialize(),"Inicializar memoria");
            GS gs; gs.init(mem.getGSVRAM(),static_cast<uint32_t>(PS2_GS_VRAM_SIZE),&mem.gs());
            GifArbiter arbiter;
            std::vector<GifPathId> paths;
            arbiter.setProcessPathPacketFn([&](GifPathId path,const uint8_t *data,uint32_t size) {
                paths.push_back(path); gs.processGIFPacket(data,size,path);
            });
            mem.setGifArbiter(&arbiter);
            const uint64_t gif[]={2ull|(1ull<<15)|(1ull<<60),0xeull,7ull,GS_REG_COLCLAMP,1ull,GS_REG_DTHE};
            for(uint32_t i=0u;i<3u;++i)
            {
                std::vector<uint8_t> command(12u,0u);
                appendU32(command,makeVifCmd(i==1u ? 0x51u : 0x50u,0u,1u));
                const auto *payload=reinterpret_cast<const uint8_t *>(gif)+i*16u;
                command.insert(command.end(),payload,payload+16u);
                appendU32(command,makeVifCmd(0x07u,0u,0x1234u+i));
                mem.processVIF1Data(command.data(),static_cast<uint32_t>(command.size()));
            }
            t.Equals(gs.getDebugSnapshot().colclamp,7ull,"Primer A+D sigue al header enviado por otro DIRECT");
            t.Equals(gs.getDebugSnapshot().dthe,1ull,"Segundo A+D sigue al DIRECTHL");
            t.Equals(mem.vif1_regs.mark,0x1236u,"Los MARK intermedios no pasan al GIF");
            t.IsTrue(paths==std::vector<GifPathId>({GifPathId::Path2,GifPathId::Path2,GifPathId::Path2}),"El arbiter conserva PATH2");
        });


        // GOW-Port: los comandos VIF entre dos DIRECT no forman parte de la IMAGE pendiente.

        // GOW-Port: cambiar DIRECT/HL entre comandos tambien cambia la prioridad de PATH2.
        tc.Run("VIF1 image continuation uses the current DIRECT priority", [](TestCase &t)
        {
            for (const bool currentHl : {false, true})
            {
                PS2Memory mem;
                t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
                std::vector<std::vector<uint8_t>> delivered;
                GifArbiter arbiter([&](const uint8_t *data, uint32_t sizeBytes)
                {
                    delivered.emplace_back(data, data + sizeBytes);
                });
                mem.setGifArbiter(&arbiter);
                std::vector<uint8_t> first;
                for (uint32_t i = 0; i < 3u; ++i)
                    appendU32(first, 0u);
                appendU32(first, makeVifCmd(currentHl ? 0x50u : 0x51u, 0u, 1u));
                appendU64(first, makeGifTag(1u, GIF_FMT_IMAGE, 0u));
                appendU64(first, 0ull);
                mem.processVIF1Data(first.data(), static_cast<uint32_t>(first.size()));
                delivered.clear();

                std::vector<uint8_t> path3;
                appendU64(path3, makeGifTag(1u, GIF_FMT_IMAGE, 0u));
                appendU64(path3, 0ull);
                appendU64(path3, 0ull); appendU64(path3, 0ull); // IMAGE no vacia
                arbiter.submit(GifPathId::Path3, path3.data(), static_cast<uint32_t>(path3.size()));
                std::vector<uint8_t> next;
                for (uint32_t i = 0; i < 3u; ++i)
                    appendU32(next, 0u);
                appendU32(next, makeVifCmd(currentHl ? 0x51u : 0x50u, 0u, 1u));
                next.insert(next.end(), 16u, 0xD0u);
                mem.processVIF1Data(next.data(), static_cast<uint32_t>(next.size()));
                t.Equals(delivered.size(), size_t{2}, "both paths should drain");
                if (delivered.size() == 2u)
                    t.IsTrue(delivered[currentHl ? 0u : 1u] == path3,
                             "DIRECTHL follows PATH3 IMAGE, DIRECT precedes it regardless of previous opcode");
            }
        });

        // GOW-Port: un DIRECT mantiene su tamaño aunque el DMA/FIFO entregue varios bloques.
        tc.Run("VIF1 DIRECT retains PACKED and REGLIST payloads across all byte cuts", [](TestCase &t)
        {
            for (const uint8_t format : {GIF_FMT_PACKED, GIF_FMT_REGLIST})
            {
                std::vector<uint8_t> gif;
                appendU64(gif, makeGifTag(1u, format, 1u));
                appendU64(gif, format == GIF_FMT_PACKED ? 0x0Eull : 0x01ull);
                appendU64(gif, 0x8000008000ull);
                appendU64(gif, format == GIF_FMT_PACKED ? GS_REG_TEXA : 0ull);
                std::vector<uint8_t> packet(12u, 0u);
                appendU32(packet, makeVifCmd(0x50u, 0u, 2u));
                packet.insert(packet.end(), gif.begin(), gif.end());
                appendU32(packet, makeVifCmd(0x07u, 0u, 0x4321u));

                for (uint32_t cut = 0u; cut < 32u; ++cut)
                {
                    PS2Memory mem;
                    t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
                    std::vector<std::vector<uint8_t>> delivered;
                    mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
                    {
                        delivered.emplace_back(data, data + sizeBytes);
                    });
                    mem.processVIF1Data(packet.data(), 16u + cut);
                    mem.processVIF1Data(packet.data() + 16u + cut,
                                        static_cast<uint32_t>(packet.size()) - 16u - cut);
                    t.IsTrue(delivered.size() == 1u && delivered[0] == gif,
                             "split DIRECT must deliver one intact GIF payload, including odd REGLIST padding");
                    t.Equals(mem.vif1_regs.mark, 0x4321u, "MARK after the payload must execute exactly once");
                }
            }
        });

        tc.Run("VIF1 FIFO completes DIRECT over separate quadword writes", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::vector<uint8_t> gif;
            appendU64(gif, makeGifTag(1u, GIF_FMT_PACKED, 1u));
            appendU64(gif, 0x0Eull);
            appendU64(gif, 0x8000008000ull);
            appendU64(gif, GS_REG_TEXA);
            std::vector<uint8_t> packet(12u, 0u);
            appendU32(packet, makeVifCmd(0x50u, 0u, 2u));
            packet.insert(packet.end(), gif.begin(), gif.end());
            appendU32(packet, makeVifCmd(0x07u, 0u, 0x2345u));
            packet.insert(packet.end(), 12u, 0u);
            std::vector<std::vector<uint8_t>> delivered;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                delivered.emplace_back(data, data + sizeBytes);
            });
            for (size_t offset = 0u; offset < packet.size(); offset += 16u)
                mem.write128(0x10005000u, _mm_loadu_si128(reinterpret_cast<const __m128i *>(packet.data() + offset)));
            t.IsTrue(delivered.size() == 1u && delivered[0] == gif, "FIFO writes must not decode GIF data as VIF commands");
            t.Equals(mem.vif1_regs.mark, 0x2345u, "FIFO MARK must execute after the complete DIRECT");
        });


        tc.Run("VIF1 fragmented DIRECT preserves DIRECTHL arbitration", [](TestCase &t)
        {
            for (const bool directHl : {false, true})
            {
                PS2Memory mem;
                t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
                std::vector<std::vector<uint8_t>> delivered;
                GifArbiter arbiter([&](const uint8_t *data, uint32_t sizeBytes) { delivered.emplace_back(data, data + sizeBytes); });
                mem.setGifArbiter(&arbiter);
                std::vector<uint8_t> path2;
                appendU64(path2, makeGifTag(1u, GIF_FMT_PACKED, 1u));
                appendU64(path2, 0x0Eull);
                appendU64(path2, 0x8000008000ull);
                appendU64(path2, GS_REG_TEXA);
                std::vector<uint8_t> packet(12u, 0u);
                appendU32(packet, makeVifCmd(directHl ? 0x51u : 0x50u, 0u, 2u));
                packet.insert(packet.end(), path2.begin(), path2.end());
                mem.processVIF1Data(packet.data(), 28u);
                std::vector<uint8_t> path3;
                appendU64(path3, makeGifTag(1u, GIF_FMT_IMAGE, 0u));
                appendU64(path3, 0ull);
                appendU64(path3, 0ull); appendU64(path3, 0ull); // IMAGE no vacia
                arbiter.submit(GifPathId::Path3, path3.data(), static_cast<uint32_t>(path3.size()));
                mem.processVIF1Data(packet.data() + 28u, static_cast<uint32_t>(packet.size()) - 28u);
                t.IsTrue(delivered.size() == 2u && delivered[directHl ? 0u : 1u] == path3 &&
                         delivered[directHl ? 1u : 0u] == path2, "fragmented payload must retain its DIRECT/HL priority");
            }
        });

        tc.Run("VIF1 DIRECT immediate zero consumes 65536 quadwords across blocks", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            constexpr uint32_t payloadBytes = 65536u * 16u;
            std::vector<uint8_t> packet(12u, 0u);
            appendU32(packet, makeVifCmd(0x50u, 0u, 0u));
            packet.insert(packet.end(), payloadBytes, 0u); // empty GIF tags
            appendU32(packet, makeVifCmd(0x07u, 0u, 0x3456u));
            uint32_t calls = 0u, transferred = 0u;
            mem.setGifPacketCallback([&](const uint8_t *, uint32_t sizeBytes) { ++calls; transferred += sizeBytes; });
            mem.processVIF1Data(packet.data(), 48u);
            for (uint32_t pos = 48u; pos < packet.size();)
            {
                const uint32_t chunk = std::min<uint32_t>(4093u, static_cast<uint32_t>(packet.size()) - pos);
                mem.processVIF1Data(packet.data() + pos, chunk);
                pos += chunk;
            }
            t.Equals(calls, 1u, "maximum DIRECT must not lose its remaining payload");
            t.Equals(transferred, payloadBytes, "immediate zero encodes 65536 quadwords");
            t.Equals(mem.vif1_regs.mark, 0x3456u, "maximum DIRECT must stop at its exact byte count");
        });

        tc.Run("VIF1 reset discards an unfinished DIRECT before accepting a new packet", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::vector<std::vector<uint8_t>> delivered;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes) { delivered.emplace_back(data, data + sizeBytes); });
            std::vector<uint8_t> abandoned(12u, 0u);
            appendU32(abandoned, makeVifCmd(0x51u, 0u, 2u));
            appendU64(abandoned, makeGifTag(1u, GIF_FMT_PACKED, 1u));
            appendU64(abandoned, 0x0Eull);
            mem.processVIF1Data(abandoned.data(), static_cast<uint32_t>(abandoned.size()));
            t.IsTrue(delivered.empty(), "incomplete GIF must not be dispatched to the stateless GS parser");
            t.IsTrue(mem.writeIORegister(0x10003C10u, 1u), "VIF1 FBRST reset should succeed");
            std::vector<uint8_t> fresh(12u, 0u);
            appendU32(fresh, makeVifCmd(0x50u, 0u, 1u));
            appendU64(fresh, makeGifTag(0u, GIF_FMT_PACKED, 1u));
            appendU64(fresh, 0x0Eull);
            appendU32(fresh, makeVifCmd(0x07u, 0u, 0x4567u));
            mem.processVIF1Data(fresh.data(), static_cast<uint32_t>(fresh.size()));
            const std::vector<uint8_t> expected(fresh.begin() + 16u, fresh.begin() + 32u);
            t.IsTrue(delivered.size() == 1u && delivered[0] == expected, "reset must discard stale DIRECT bytes");
            t.Equals(mem.vif1_regs.mark, 0x4567u, "reset must restore normal VIF command decoding");
        });

        // GOW-Port: DIRECTHL cambia la eleccion entre PATH2/PATH3, nunca el FIFO de cada path.
        tc.Run("GIF arbiter preserves PATH2 FIFO when DIRECT and DIRECTHL share a queue", [](TestCase &t)
        {
            std::array<unsigned, 3> order{0u, 1u, 2u}; // HL, DIRECT, PATH3 IMAGE
            do
            {
                std::vector<uint64_t> delivered;
                GifArbiter arbiter([&](const uint8_t *data, uint32_t)
                {
                    uint64_t marker = 0u;
                    std::memcpy(&marker, data + 8u, sizeof(marker));
                    delivered.push_back(marker);
                });
                for (const unsigned id : order)
                {
                    std::vector<uint8_t> packet;
                    // GOW-Port: la prioridad IMAGE requiere NLOOP>0; etiqueta vacia no transmite pixels.
                    appendU64(packet, makeGifTag(id == 2u ? 1u : 0u, id == 2u ? GIF_FMT_IMAGE : GIF_FMT_PACKED, 1u));
                    appendU64(packet, id);
                    if (id == 2u) { appendU64(packet, 0ull); appendU64(packet, 0ull); }
                    arbiter.submit(id == 2u ? GifPathId::Path3 : GifPathId::Path2,
                                   packet.data(), static_cast<uint32_t>(packet.size()), id == 0u);
                }
                // PATH1 siempre tiene prioridad sobre ambos, aunque se haya añadido el último.
                std::vector<uint8_t> path1;
                appendU64(path1, makeGifTag(0u, GIF_FMT_PACKED, 1u));
                appendU64(path1, 3u);
                arbiter.submit(GifPathId::Path1, path1.data(), static_cast<uint32_t>(path1.size()));
                arbiter.drain();
                const bool hlFirst = std::find(order.begin(), order.end(), 0u) < std::find(order.begin(), order.end(), 1u);
                const std::vector<uint64_t> expected = hlFirst ? std::vector<uint64_t>{3u, 2u, 0u, 1u}
                                                            : std::vector<uint64_t>{3u, 1u, 2u, 0u};
                t.IsTrue(delivered == expected, "PATH2 order must survive arbitration against PATH3 IMAGE");
                t.IsTrue(arbiter.empty(), "drain should release all packets");
            } while (std::next_permutation(order.begin(), order.end()));
        });

        tc.Run("GIF arbiter preserves PATH3 setup before IMAGE when DIRECTHL is queued", [](TestCase &t)
        {
            std::array<unsigned, 3> order{0u, 1u, 2u}; // HL, PATH3 IMAGE, PATH3 setup
            do
            {
                std::vector<uint64_t> delivered;
                GifArbiter arbiter([&](const uint8_t *data, uint32_t)
                {
                    uint64_t marker = 0u;
                    std::memcpy(&marker, data + 8u, sizeof(marker));
                    delivered.push_back(marker);
                });
                for (const unsigned id : order)
                {
                    std::vector<uint8_t> packet;
                    appendU64(packet, makeGifTag(id == 1u ? 1u : 0u, id == 1u ? GIF_FMT_IMAGE : GIF_FMT_PACKED, 1u));
                    appendU64(packet, id);
                    if (id == 1u) { appendU64(packet, 0ull); appendU64(packet, 0ull); }
                    arbiter.submit(id == 0u ? GifPathId::Path2 : GifPathId::Path3,
                                   packet.data(), static_cast<uint32_t>(packet.size()), id == 0u);
                }
                arbiter.drain();
                const bool imageFirst = std::find(order.begin(), order.end(), 1u) < std::find(order.begin(), order.end(), 2u);
                const std::vector<uint64_t> expected = imageFirst ? std::vector<uint64_t>{1u, 0u, 2u}
                                                               : std::vector<uint64_t>{0u, 2u, 1u};
                t.IsTrue(delivered == expected, "PATH3 setup and IMAGE order must survive DIRECTHL arbitration");
            } while (std::next_permutation(order.begin(), order.end()));
        });

        // GOW-Port: clasificar el flujo completo, incluidos payloads que parecen GIFtags.
        tc.Run("GIF arbiter retains PATH3 IMAGE classification across drains", [](TestCase &t)
        {
            std::vector<GifPathId> order;
            GifArbiter arbiter;
            arbiter.setProcessPathPacketFn([&](GifPathId path, const uint8_t *, uint32_t) { order.push_back(path); });
            std::vector<uint8_t> image;
            appendU64(image, makeGifTag(2u, GIF_FMT_IMAGE, 0u)); appendU64(image, 0ull);
            arbiter.submit(GifPathId::Path3, image.data(), static_cast<uint32_t>(image.size()));
            arbiter.drain(); order.clear();
            const std::vector<uint8_t> pixels(32u, 0u);
            std::vector<uint8_t> hl;
            appendU64(hl, makeGifTag(0u, GIF_FMT_PACKED, 1u)); appendU64(hl, 0ull);
            arbiter.submit(GifPathId::Path2, hl.data(), static_cast<uint32_t>(hl.size()), true);
            arbiter.submit(GifPathId::Path3, pixels.data(), static_cast<uint32_t>(pixels.size()));
            arbiter.drain();
            t.IsTrue(order == std::vector<GifPathId>{GifPathId::Path3, GifPathId::Path2},
                     "DIRECTHL debe esperar al payload IMAGE pendiente, aunque no lleve etiqueta");
        });

        tc.Run("GIF arbiter recognizes IMAGE after PACKED setup in the same block", [](TestCase &t)
        {
            std::vector<GifPathId> order;
            GifArbiter arbiter;
            arbiter.setProcessPathPacketFn([&](GifPathId path, const uint8_t *, uint32_t) { order.push_back(path); });
            std::vector<uint8_t> path3;
            appendU64(path3, makeGifTag(1u, GIF_FMT_PACKED, 1u)); appendU64(path3, 0xEull); // A+D
            appendU64(path3, 7ull); appendU64(path3, GS_REG_COLCLAMP);
            appendU64(path3, makeGifTag(1u, GIF_FMT_IMAGE, 0u)); appendU64(path3, 0ull);
            appendU64(path3, 0ull); appendU64(path3, 0ull);
            const std::vector<uint8_t> hl(16u, 0u);
            arbiter.submit(GifPathId::Path2, hl.data(), static_cast<uint32_t>(hl.size()), true);
            arbiter.submit(GifPathId::Path3, path3.data(), static_cast<uint32_t>(path3.size()));
            arbiter.drain();
            t.IsTrue(order == std::vector<GifPathId>{GifPathId::Path3, GifPathId::Path2},
                     "IMAGE se reconoce tras el setup sin adelantarla a sus registros");
        });

        tc.Run("GIF arbiter does not interpret PACKED payload as an IMAGE tag", [](TestCase &t)
        {
            std::vector<GifPathId> order;
            GifArbiter arbiter;
            arbiter.setProcessPathPacketFn([&](GifPathId path, const uint8_t *, uint32_t) { order.push_back(path); });
            std::vector<uint8_t> packed;
            appendU64(packed, makeGifTag(1u, GIF_FMT_PACKED, 1u)); appendU64(packed, 0xFull); // NOP
            arbiter.submit(GifPathId::Path3, packed.data(), static_cast<uint32_t>(packed.size()));
            arbiter.drain(); order.clear();
            std::vector<uint8_t> payload;
            appendU64(payload, makeGifTag(1u, GIF_FMT_IMAGE, 0u)); appendU64(payload, 0ull);
            const std::vector<uint8_t> hl(16u, 0u);
            arbiter.submit(GifPathId::Path3, payload.data(), static_cast<uint32_t>(payload.size()));
            arbiter.submit(GifPathId::Path2, hl.data(), static_cast<uint32_t>(hl.size()), true);
            arbiter.drain();
            t.IsTrue(order == std::vector<GifPathId>{GifPathId::Path2, GifPathId::Path3},
                     "Un registro PACKED conserva su prioridad aunque sus bits parezcan una etiqueta IMAGE");
        });

        tc.Run("GIF arbiter ignores empty IMAGE tags for DIRECTHL priority", [](TestCase &t)
        {
            std::vector<GifPathId> order;
            GifArbiter arbiter;
            arbiter.setProcessPathPacketFn([&](GifPathId path, const uint8_t *, uint32_t) { order.push_back(path); });
            std::vector<uint8_t> empty;
            appendU64(empty, makeGifTag(0u, GIF_FMT_IMAGE, 0u)); appendU64(empty, 0ull);
            const std::vector<uint8_t> hl(16u, 0u);
            arbiter.submit(GifPathId::Path3, empty.data(), static_cast<uint32_t>(empty.size()));
            arbiter.submit(GifPathId::Path2, hl.data(), static_cast<uint32_t>(hl.size()), true);
            arbiter.drain();
            t.IsTrue(order == std::vector<GifPathId>{GifPathId::Path2, GifPathId::Path3},
                     "NLOOP=0 no inicia una transferencia IMAGE ni bloquea DIRECTHL");
        });

        tc.Run("GIF arbiter keeps REGLIST padding and partial next tag across drains", [](TestCase &t)
        {
            std::vector<GifPathId> order;
            GifArbiter arbiter;
            arbiter.setProcessPathPacketFn([&](GifPathId path, const uint8_t *, uint32_t) { order.push_back(path); });
            std::vector<uint8_t> prefix;
            appendU64(prefix, makeGifTag(1u, GIF_FMT_REGLIST, 1u)); appendU64(prefix, 0xFull);
            appendU64(prefix, 0ull); // un registro, con 8 bytes de relleno pendientes
            arbiter.submit(GifPathId::Path3, prefix.data(), static_cast<uint32_t>(prefix.size()));
            arbiter.drain(); order.clear();
            std::vector<uint8_t> middle;
            appendU64(middle, makeGifTag(1u, GIF_FMT_IMAGE, 0u)); // relleno: no es una etiqueta
            appendU64(middle, makeGifTag(1u, GIF_FMT_IMAGE, 0u)); // mitad real de la siguiente etiqueta
            const std::vector<uint8_t> hl(16u, 0u);
            arbiter.submit(GifPathId::Path3, middle.data(), static_cast<uint32_t>(middle.size()));
            arbiter.submit(GifPathId::Path2, hl.data(), static_cast<uint32_t>(hl.size()), true);
            arbiter.drain();
            t.IsTrue(order == std::vector<GifPathId>{GifPathId::Path2, GifPathId::Path3},
                     "Ni el relleno ni una etiqueta incompleta inician IMAGE");
            order.clear();
            std::vector<uint8_t> last;
            appendU64(last, 0ull); appendU64(last, 0ull); appendU64(last, 0ull);
            arbiter.submit(GifPathId::Path2, hl.data(), static_cast<uint32_t>(hl.size()), true);
            arbiter.submit(GifPathId::Path3, last.data(), static_cast<uint32_t>(last.size()));
            arbiter.drain();
            t.IsTrue(order == std::vector<GifPathId>{GifPathId::Path3, GifPathId::Path2},
                     "La etiqueta IMAGE reconstruida recupera la prioridad sin contar el relleno");
        });

        tc.Run("GIF arbiter reset discards PATH3 classification and queued packets", [](TestCase &t)
        {
            std::vector<GifPathId> order;
            GifArbiter arbiter;
            arbiter.setProcessPathPacketFn([&](GifPathId path, const uint8_t *, uint32_t) { order.push_back(path); });
            std::vector<uint8_t> image;
            appendU64(image, makeGifTag(2u, GIF_FMT_IMAGE, 0u)); appendU64(image, 0ull);
            arbiter.submit(GifPathId::Path3, image.data(), static_cast<uint32_t>(image.size()));
            arbiter.reset();
            t.IsTrue(arbiter.empty(), "reset descarta los paquetes antiguos");
            const std::vector<uint8_t> emptyTag(16u, 0u);
            arbiter.submit(GifPathId::Path3, emptyTag.data(), static_cast<uint32_t>(emptyTag.size()));
            arbiter.submit(GifPathId::Path2, emptyTag.data(), static_cast<uint32_t>(emptyTag.size()), true);
            arbiter.drain();
            t.IsTrue(order == std::vector<GifPathId>{GifPathId::Path2, GifPathId::Path3},
                     "reset evita interpretar la nueva etiqueta como pixels de la transferencia antigua");
        });

        tc.Run("GIF arbiter recognizes IMAGE2 tags and their raw continuations", [](TestCase &t)
        {
            std::vector<GifPathId> order;
            GifArbiter arbiter;
            arbiter.setProcessPathPacketFn([&](GifPathId path, const uint8_t *, uint32_t) { order.push_back(path); });
            std::vector<uint8_t> image;
            appendU64(image, makeGifTag(1u, 3u, 0u)); appendU64(image, 0ull);
            const std::vector<uint8_t> empty(16u, 0u);
            arbiter.submit(GifPathId::Path3, image.data(), static_cast<uint32_t>(image.size()));
            arbiter.submit(GifPathId::Path2, empty.data(), static_cast<uint32_t>(empty.size()), true);
            arbiter.drain();
            t.IsTrue(order == std::vector<GifPathId>{GifPathId::Path3,GifPathId::Path2},"IMAGE2 comparte la prioridad de IMAGE");
            order.clear();
            arbiter.submit(GifPathId::Path2, empty.data(), static_cast<uint32_t>(empty.size()), true);
            arbiter.submit(GifPathId::Path3, empty.data(), static_cast<uint32_t>(empty.size()));
            arbiter.drain();
            t.IsTrue(order == std::vector<GifPathId>{GifPathId::Path3,GifPathId::Path2},"El payload IMAGE2 tampoco se decodifica como etiqueta");
        });

        tc.Run("unaligned accesses throw", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            bool threwRead32 = false;
            bool threwWrite64 = false;
            try
            {
                (void)mem.read32(0x00000002u);
            }
            catch (const std::exception &)
            {
                threwRead32 = true;
            }

            try
            {
                mem.write64(0x00000004u + 2u, 0x1122334455667788ull);
            }
            catch (const std::exception &)
            {
                threwWrite64 = true;
            }

            t.IsTrue(threwRead32, "unaligned read32 should throw");
            t.IsTrue(threwWrite64, "unaligned write64 should throw");
        });
    });
}
