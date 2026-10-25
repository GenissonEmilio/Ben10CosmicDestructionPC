#include "iop_ioman.h"

#include "iop_cdvd.h"
#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "../services/iop_rpc.h"

#include <algorithm>
#include <cctype>

namespace ps2x::iop::detail
{
    namespace
    {
        // ioman returns negative errno values
        constexpr uint32_t kErrNoEnt = static_cast<uint32_t>(-2);
        constexpr uint32_t kErrBadF = static_cast<uint32_t>(-9);
        constexpr uint32_t kErrMFile = static_cast<uint32_t>(-24);
        constexpr uint32_t kErrInval = static_cast<uint32_t>(-22);
        constexpr uint32_t kErrRoFs = static_cast<uint32_t>(-30);
        constexpr size_t kMaxFiles = 32u;
        constexpr uint32_t kFirstFd = 3u; // keep 0-2 free, as for the console descriptors

        // "cdrom0:\GODOFWAR.TOC;1", "cdrom:file" ... -> true for the cdrom device
        bool isCdromPath(const std::string &path)
        {
            const size_t colon = path.find(':');
            if (colon == std::string::npos)
                return false;
            std::string device = path.substr(0, colon);
            while (!device.empty() && std::isdigit(static_cast<unsigned char>(device.back())))
                device.pop_back();
            std::transform(device.begin(), device.end(), device.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return device == "cdrom";
        }
    }

    IopIoman::IopIoman(IopMemory &memory, IopCdvd &cdvd) noexcept
        : m_memory(memory),
          m_cdvd(cdvd)
    {
    }

    void IopIoman::reset()
    {
        m_devices.clear();
        m_files.clear();
    }

    IopIoman::OpenFile *IopIoman::file(uint32_t fd)
    {
        if (fd < kFirstFd || fd - kFirstFd >= m_files.size() || !m_files[fd - kFirstFd].used)
            return nullptr;
        return &m_files[fd - kFirstFd];
    }

    bool IopIoman::dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor)
    {
        constexpr size_t kMaxDevices = 16u;
        const uint32_t a0 = cpu.gpr[4];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };

        switch (ordinal)
        {
        // GoW2-Port: open/close/read/lseek for the cdrom device. God of War II's smpd opens
        // "cdrom0:\GODOFWAR.TOC" through ioman; unhandled, every call returned 0 and it loaded an empty table.
        case 4: // open(name, flags, mode)
        {
            const std::string path = m_memory.readString(a0, 1024u);
            if (!isCdromPath(path))
            {
                setV0(kErrNoEnt); // no host or other devices on a console
                return true;
            }
            if ((cpu.gpr[5] & 2u) != 0u) // O_WRONLY / O_RDWR: the disc is read-only
            {
                setV0(kErrRoFs);
                return true;
            }
            const auto found = m_cdvd.findDiscFile(path);
            if (!found)
            {
                setV0(kErrNoEnt);
                return true;
            }
            auto slot = std::find_if(m_files.begin(), m_files.end(), [](const OpenFile &f) { return !f.used; });
            if (slot == m_files.end())
            {
                if (m_files.size() >= kMaxFiles)
                {
                    setV0(kErrMFile);
                    return true;
                }
                slot = m_files.insert(m_files.end(), OpenFile{});
            }
            *slot = OpenFile{true, found->lsn, found->size, 0u};
            setV0(kFirstFd + static_cast<uint32_t>(slot - m_files.begin()));
            return true;
        }
        case 5: // close(fd)
        {
            OpenFile *f = file(a0);
            if (!f)
            {
                setV0(kErrBadF);
                return true;
            }
            *f = OpenFile{};
            setV0(0u);
            return true;
        }
        case 6: // read(fd, buffer, size)
        {
            OpenFile *f = file(a0);
            const uint32_t buffer = cpu.gpr[5];
            const uint32_t size = cpu.gpr[6];
            if (!f)
            {
                setV0(kErrBadF);
                return true;
            }
            if (size == 0u || f->position >= f->size)
            {
                setV0(0u);
                return true;
            }
            std::vector<uint8_t> bytes(std::min(size, f->size - f->position));
            const uint32_t read = m_cdvd.readDiscFile(f->lsn, f->size, f->position, static_cast<uint32_t>(bytes.size()), bytes.data());
            if (read != 0u && !m_memory.writeRam(buffer, bytes.data(), read))
            {
                setV0(kErrInval);
                return true;
            }
            f->position += read;
            setV0(read);
            return true;
        }
        case 7: // write(fd, buffer, size)
            setV0(file(a0) ? kErrRoFs : kErrBadF);
            return true;
        case 8: // lseek(fd, offset, whence): SEEK_SET 0, SEEK_CUR 1, SEEK_END 2
        {
            OpenFile *f = file(a0);
            if (!f)
            {
                setV0(kErrBadF);
                return true;
            }
            const int64_t offset = static_cast<int32_t>(cpu.gpr[5]);
            const uint32_t whence = cpu.gpr[6];
            const int64_t base = whence == 0u ? 0 : whence == 1u ? f->position : whence == 2u ? f->size : -1;
            const int64_t target = base + offset;
            if (base < 0 || target < 0 || target > 0x7FFFFFFF)
            {
                setV0(kErrInval);
                return true;
            }
            f->position = static_cast<uint32_t>(target);
            setV0(f->position);
            return true;
        }
        case 20: // AddDrv
        {
            if (a0 == 0u || m_devices.size() >= kMaxDevices)
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            const uint32_t nameAddress = m_memory.read32(a0);
            const uint32_t operations = m_memory.read32(a0 + 16u);
            const std::string name = m_memory.readString(nameAddress, 64u);
            if (nameAddress == 0u || operations == 0u || name.empty())
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            m_devices.push_back({a0, cpu.gpr[28], name});
            const uint32_t init = m_memory.read32(operations);
            if (init != 0u)
            {
                const int32_t result = static_cast<int32_t>(
                    executor.executeGuestFunction(init, a0, 0u, 0u, 0u, cpu.gpr[28]));
                if (result < 0)
                {
                    m_devices.pop_back();
                    setV0(0xFFFFFFFFu);
                    return true;
                }
            }

            setV0(0u);
            return true;
        }
        case 21: // DelDrv
        {
            const std::string name = m_memory.readString(a0, 64u);
            const auto device = std::find_if(
                m_devices.begin(), m_devices.end(),
                [&](const Device &candidate)
                { return candidate.name == name; });
            if (device == m_devices.end())
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            const uint32_t operations = m_memory.read32(device->address + 16u);
            const uint32_t deinit = operations != 0u ? m_memory.read32(operations + 4u) : 0u;
            if (deinit != 0u)
                (void)executor.executeGuestFunction(deinit, device->address, 0u, 0u, 0u, device->gp);
            m_devices.erase(device);
            setV0(0u);
            return true;
        }
        default:
            return false;
        }
    }
}
