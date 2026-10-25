// Cosmic Destruction SLUS-21938 only. GPL-3.0.
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_syscalls.h"
#include "ps2_stubs.h"

namespace {
void sendInternalCommand(uint8_t *ram, R5900Context *context, PS2Runtime *runtime)
{
    // _sceSifSendCmd has an extra mode argument before the packet pointer.
    const uint32_t packet = getRegU32(context, 6);
    const uint32_t packetSize = getRegU32(context, 7);
    const uint32_t source = getRegU32(context, 8);
    const uint32_t destination = getRegU32(context, 9);
    const uint32_t bytes = getRegU32(context, 10);
    SET_GPR_U32(context, 5, packet);
    SET_GPR_U32(context, 6, packetSize);
    SET_GPR_U32(context, 7, source);
    SET_GPR_U32(context, 8, destination);
    SET_GPR_U32(context, 9, bytes);
    ps2_syscalls::sceSifSendCmd(ram, context, runtime);
}

void applyCosmicMemoryLayout(PS2Runtime &runtime)
{
    // The executable starts at 0x100000 and owns the heap above its BSS.
    // Keep runtime RPC/TLS scratch allocations in unused RAM below the ELF,
    // beyond the kernel mirror, instead of overwriting the game's free list.
    runtime.setPrivateGuestHeap(0x000A0000u, 0x000FF000u);
    runtime.replaceFunction(0x00446578u, sendInternalCommand);
    // This SDK variant's layer-aware search otherwise waits for CDVDFSV RPC
    // 0x80000597. The supported disc has one layer; use the verified ISO index.
    runtime.replaceFunction(0x004570C0u, ps2_stubs::sceCdSearchFile);
}
}

// CRC is filled from the supported, SHA-256-validated retail executable.
PS2_REGISTER_GAME_OVERRIDE("Cosmic Destruction runtime memory", "SLUS_219.38",
                           0x00100008u, 0x09763C89u, applyCosmicMemoryLayout);
