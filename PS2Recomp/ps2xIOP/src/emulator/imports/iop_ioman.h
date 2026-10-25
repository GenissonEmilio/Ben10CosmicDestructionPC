#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopCdvd;
    class IopGuestExecutor;
    class IopMemory;

    class IopIoman
    {
    public:
        IopIoman(IopMemory &memory, IopCdvd &cdvd) noexcept;

        void reset();
        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor);

    private:
        struct Device
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
            std::string name;
        };

        // GoW2-Port: files opened on the cdrom device (read-only, served from the disc through IopCdvd)
        struct OpenFile
        {
            bool used = false;
            uint32_t lsn = 0u;
            uint32_t size = 0u;
            uint32_t position = 0u;
        };

        OpenFile *file(uint32_t fd);

        IopMemory &m_memory;
        IopCdvd &m_cdvd;
        std::vector<Device> m_devices;
        std::vector<OpenFile> m_files;
    };
}
