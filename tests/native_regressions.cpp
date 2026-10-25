// Synthetic PS2/IOP programs only. No instructions or assets from a retail game.
#include "support/iop_compat_test_support.h"
#include "emulator/iop_emulator.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "ps2_runtime_macros.h"
#include "ps2_syscalls.h"
#include <filesystem>
#include <fstream>
#include <array>
#include "Stubs/MPEG.h"

namespace {
void mpegWorkspaceStaysInsideCallerStorage() {
    using namespace iop_test;
    PS2Runtime runtime;
    require(runtime.memory().initialize(), "guest RAM initialization failed");
    auto *ram = runtime.memory().getRDRAM();
    auto word = [ram](uint32_t address) -> uint32_t & { return *reinterpret_cast<uint32_t *>(ram + address); };
    word(0x1717bcu) = 0x12345678u;
    word(0x171800u) = 0x87654321u;
    word(0x9000u) = 0xabcdef01u;
    R5900Context context{};
    ps2_stubs::sceMpegInit(ram, &context, &runtime);
    SET_GPR_U32(&context, 4, 0x2000); SET_GPR_U32(&context, 5, 0x4000); SET_GPR_U32(&context, 6, 0x5000);
    ps2_stubs::sceMpegCreate(ram, &context, &runtime);
    require(getRegU32(&context, 2) == 1, "MPEG Create returned a pointer instead of success");
    require(word(0x2040) == 0x4000 && word(0x41c8) == 0x41f8, "MPEG workspace/reference layout is incompatible");
    require(word(0x4024) == 0x463258 && word(0x4030) == 0x463280, "MPEG callbacks refer to the wrong SDK");
    require(word(0x9000) == 0xabcdef01u, "MPEG initialization wrote outside its supplied work area");
    require(word(0x1717bc) == 0x12345678u && word(0x171800) == 0x87654321u,
            "MPEG initialization overwrote executable addresses from another game");
    word(0x41f8 + 0x28) = 7;
    ps2_stubs::sceMpegClearRefBuff(ram, &context, &runtime);
    require(word(0x41f8 + 0x28) == 0, "MPEG reference buffers were not cleared in caller storage");
    word(0x4878) = 1;
    ps2_stubs::sceMpegReset(ram, &context, &runtime);
    require(word(0x4878) == 0 && word(0x4098) == UINT32_MAX, "MPEG reset used the wrong SDK layout");
    ps2_stubs::enqueueMpegDecodedFrameForTesting(0x2000);
    SET_GPR_U32(&context, 5, 0xa000); SET_GPR_U32(&context, 6, 1);
    word(0xa400) = 0xabcdef01u;
    ps2_stubs::sceMpegGetPicture(ram, &context, &runtime);
    require(word(0x2000) == 16 && word(0x2004) == 16, "MPEG decoded dimensions were not delivered");
    require(word(0xa000) == 0x80808080u && word(0xa400) == 0xabcdef01u,
            "MPEG macroblock output is absent or exceeds capacity");
    require(word(0x40c4) == 1 && word(0x40ec) == 0x2000a000, "MPEG picture metadata uses the wrong SDK layout");
}

void interruptCommandSignalsCompletion() {
    using namespace iop_test;
    Host host;
    Irx image(0x10000u, 0x700u);
    image.words(0u, {
        0x27bdffa0u, 0xafbf005cu,
        0x24040005u, 0x3c050001u, 0x34a50300u, 0x24060010u, 0x00003821u,
        0xafa00010u, 0xafa00014u, 0x3c020001u, 0x34420100u, 0xafa20018u,
        0x3402cafeu, 0xafa2001cu, 0x0c004085u, 0u,
        0x3c030001u, 0xac620404u,
        0x240403e8u, 0x0c004095u, 0u,
        0x8fbf005cu, 0x27bd0060u, 0x00001021u, 0x03e00008u, 0u,
    });
    image.words(0x100u, {0x3c030001u, 0xac640400u, 0x03e00008u, 0u});
    image.words(0x200u, {0x41e00000u, 0u, 0x0101u, 0x63666973u, 0x0000646du,
                       0x03e00008u, 0x2400001cu, 0u, 0u});
    image.words(0x240u, {0x41e00000u, 0u, 0x0101u, 0x61626874u, 0x00006573u,
                       0x03e00008u, 0x24000021u, 0u, 0u});
    image.words(0x300u, {16u, 0u, 5u, 0u});
    image.install(host);
    ps2x::iop::detail::IopEmulator emulator(host);
    require(emulator.loadModuleBuffer(0x1000u, nullptr, 0u).moduleId > 0, "interrupt command module did not load");
    uint32_t result[2]{};
    require(emulator.readMemory(0x10400u, result, sizeof(result)), "interrupt command result read failed");
    require(result[1] != 0u, "SendCmdIntr did not return a DMA identifier");
    require(result[0] == 0xcafeu, "SendCmdIntr did not invoke its completion callback with the argument");
}

void commandReachesRegisteredIopHandler() {
    using namespace iop_test;
    Host host;
    Irx image(0x10000u, 0x700u);
    image.words(0u, {
        0x27bdffe0u, 0xafbf001cu,
        0x24040003u, 0x3c050001u, 0x34a50100u,
        0x24061234u, 0x0c004085u, 0u,
        0x8fbf001cu, 0x27bd0020u, 0x00001021u, 0x03e00008u, 0u,
    });
    image.words(0x100u, {
        0x8c820010u, 0x3c030001u, 0xac620400u, 0xac650404u, 0x03e00008u, 0u,
    });
    image.words(0x200u, {0x41e00000u, 0u, 0x0101u, 0x63666973u, 0x0000646du,
                       0x03e00008u, 0x2400000au, 0u, 0u});
    image.install(host);
    ps2x::iop::detail::IopEmulator emulator(host);
    require(emulator.loadModuleBuffer(0x1000u, nullptr, 0u).moduleId > 0, "command module did not load");
    const uint32_t packet[] = {20u, 0u, 3u, 0u, 0x98765432u};
    require(emulator.writeMemory(0x8000u, packet, sizeof(packet)), "mailbox write failed");
    emulator.onSifTransfer({SifTransferKind::SetDma, SifTransferPhase::AfterCopy, 0x1000u, 0x8000u, sizeof(packet)});
    uint32_t result[2]{};
    require(emulator.readMemory(0x10400u, result, sizeof(result)), "command result read failed");
    require(result[0] == 0x98765432u && result[1] == 0x1234u, "registered IOP command did not receive its packet and argument");
    emulator.reset();
    require(emulator.writeMemory(0x8000u, packet, sizeof(packet)), "mailbox reset write failed");
    emulator.onSifTransfer({SifTransferKind::SetDma, SifTransferPhase::AfterCopy, 0x1000u, 0x8000u, sizeof(packet)});
    require(emulator.readMemory(0x10400u, result, sizeof(result)), "reset result read failed");
    require(result[0] == 0u && result[1] == 0u, "reset retained a stale command handler");
}

void commandExtraDataUsesIopAddressSpace() {
    using namespace iop_test;
    PS2Runtime runtime;
    require(runtime.memory().initialize(), "guest RAM initialization failed");
    auto *ram = runtime.memory().getRDRAM();
    std::memset(ram + 0x4000u, 'Z', 16u);
    std::memset(ram + 0x9000u, 'Y', 16u);
    const uint32_t badStack[] = {1u, 0x2bb288u};
    std::memcpy(ram + 0x8010u, badStack, sizeof(badStack));
    R5900Context context{};
    SET_GPR_U32(&context, 4, 77u); SET_GPR_U32(&context, 5, 0x3000u);
    SET_GPR_U32(&context, 6, 16u); SET_GPR_U32(&context, 7, 0x4000u);
    SET_GPR_U32(&context, 8, 0x9000u); SET_GPR_U32(&context, 9, 16u);
    SET_GPR_U32(&context, 29, 0x8000u);
    ps2_syscalls::sceSifSendCmd(ram, &context, &runtime);
    require(getRegU32(&context, 2) != 0u, "SIF command was rejected");
    std::array<char, 16> received{};
    require(runtime.readIopMemory(0x9000u, received.data(), received.size()), "IOP payload read failed");
    require(received[0] == 'Z' && received[15] == 'Z', "extra payload did not reach IOP RAM");
    require(ram[0x9000u] == 'Y' && ram[0x900fu] == 'Y', "IOP destination aliased equal-numbered EE memory");
}

void outsideDelayRunsWorkers() {
    using namespace iop_test;
    Host host;
    Irx image(0x10000u, 0x700u);
    image.words(0u, {
        0x27bdffe0u, 0xafbf001cu, // save return address
        0x3c040001u, 0x34840300u, // thread descriptor
        0x0c004085u, 0u,         // CreateThread, import at 0x10214
        0x00402021u, 0x00002821u,
        0x0c004087u, 0u,         // StartThread, import at 0x1021c
        0x240403e8u,              // 1000 microseconds
        0x0c004089u, 0u,         // DelayThread, import at 0x10224
        0x3c030001u, 0x8c620400u, 0u, 0xac620404u, // R3000 load delay, then copy flag
        0x8fbf001cu, 0x27bd0020u, 0x00001021u, 0x03e00008u, 0u,
    });
    image.words(0x100u, {
        0x3c030001u, 0x24020001u, 0xac620400u,
        0x0c00408bu, 0u,         // ExitThread, import at 0x1022c
    });
    image.words(0x200u, {
        0x41e00000u, 0u, 0x0101u, 0x61626874u, 0x00006573u, // thbase
        0x03e00008u, 0x24000004u,
        0x03e00008u, 0x24000006u,
        0x03e00008u, 0x24000021u,
        0x03e00008u, 0x24000008u,
        0u, 0u,
    });
    image.words(0x300u, {0u, 0u, 0x10100u, 0x400u, 16u, 0u});
    image.words(0x400u, {0u, 0u});
    image.install(host);
    ps2x::iop::detail::IopEmulator emulator(host);
    auto loaded = emulator.loadModuleBuffer(0x1000u, nullptr, 0u);
    require(loaded.moduleId > 0, "synthetic module did not load");
    uint32_t flag = 0u;
    require(emulator.readMemory(0x10404u, &flag, sizeof(flag)), "cannot read initialization result");
    if (flag != 1u) {
        for (const auto &line : host.logs) std::cerr << line << '\n';
        std::cerr << "delay flag=" << flag << " cycles=" << emulator.cycles() << " threads=" << emulator.threadCount() << '\n';
    }
    require(flag == 1u, "DelayThread outside a scheduled thread did not let its worker run");
    require(emulator.cycles() >= 36864u, "DelayThread did not advance virtual IOP time");
}

void extractedCdAndBusyCommands(const std::filesystem::path &root) {
    using namespace iop_test;
    std::filesystem::create_directories(root / "ASSETS");
    std::ofstream(root / ".disc_layout") << "sectors 32\nlayer1 0\nfile 20 2048 ASSETS/DATA.DAT\n";
    std::array<char, 2048> pvd{}, data{};
    pvd[0] = 1; std::memcpy(pvd.data() + 1, "CD001", 5); pvd[6] = 1;
    data.fill('X');
    std::ofstream(root / "ASSETS/DATA.DAT", std::ios::binary).write(data.data(), data.size());
    std::ofstream meta(root / ".disc_meta", std::ios::binary);
    const uint32_t sector = 16;
    meta.write(reinterpret_cast<const char *>(&sector), sizeof(sector));
    meta.write(pvd.data(), pvd.size()); meta.close();
    auto paths = PS2Runtime::getIoPaths(); paths.cdRoot = root;
    PS2Runtime::setIoPaths(paths);
    PS2Runtime runtime;
    require(runtime.memory().initialize(), "guest RAM initialization failed");
    auto *ram = runtime.memory().getRDRAM();
    R5900Context context{};
    SET_GPR_U32(&context, 4, 16u); SET_GPR_U32(&context, 5, 1u); SET_GPR_U32(&context, 6, 0x2000u);
    ps2_stubs::sceCdRead(ram, &context, &runtime);
    require(getRegU32(&context, 2) == 1u, "first CD command was not accepted");
    require(std::memcmp(ram + 0x2000u, pvd.data(), pvd.size()) == 0, "raw directory sector differs from extracted metadata");
    ps2_stubs::sceCdRead(ram, &context, &runtime);
    require(getRegU32(&context, 2) == 0u, "duplicate CD command was accepted while one was pending");
    ps2_stubs::sceCdSync(ram, &context, &runtime);
    require(getRegU32(&context, 2) == 0u, "completed CD command did not synchronize");
    SET_GPR_U32(&context, 4, 20u); SET_GPR_U32(&context, 5, 1u); SET_GPR_U32(&context, 6, 0x2000u);
    ps2_stubs::sceCdRead(ram, &context, &runtime);
    require(getRegU32(&context, 2) == 1u, "next CD command failed after synchronization");
    require(std::memcmp(ram + 0x2000u, data.data(), data.size()) == 0, "file sector differs from extracted file");
    ps2_stubs::sceCdSync(ram, &context, &runtime);
}
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    try {
    outsideDelayRunsWorkers();
    interruptCommandSignalsCompletion();
    commandReachesRegisteredIopHandler();
    commandExtraDataUsesIopAddressSpace();
    mpegWorkspaceStaysInsideCallerStorage();
    extractedCdAndBusyCommands(std::filesystem::path(argv[1]));
    std::cout << "Native regression checks passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
