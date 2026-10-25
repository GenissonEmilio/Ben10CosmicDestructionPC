#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;
    class IopMemory;

    class IopCdvd
    {
    public:
        struct CompletionCallback
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
            uint32_t reason = 0u;
        };

        IopCdvd(IopHost &host, IopMemory &memory, IopKernel &kernel);
        ~IopCdvd();

        IopCdvd(const IopCdvd &) = delete;
        IopCdvd &operator=(const IopCdvd &) = delete;

        void reset() noexcept;

        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] std::optional<CompletionCallback> takeCompletionCallback() noexcept;

        // GoW2-Port: file access to the disc for ioman's cdrom device (open/read/lseek of "cdrom0:\NAME;1").
        struct DiscFileInfo
        {
            uint32_t lsn = 0u;
            uint32_t size = 0u;
        };
        [[nodiscard]] std::optional<DiscFileInfo> findDiscFile(const std::string &guestPath);
        // Reads size bytes at offset of the file that starts at lsn; returns the bytes read (clamped to fileSize).
        [[nodiscard]] uint32_t readDiscFile(uint32_t lsn, uint32_t fileSize, uint32_t offset, uint32_t size, uint8_t *destination);

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
