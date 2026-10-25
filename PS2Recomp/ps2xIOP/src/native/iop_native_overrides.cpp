// No IRX address overrides are validated for Cosmic Destruction yet.
// The God of War base's module offsets must never be applied by module name.
#include "native/iop_native.h"
namespace ps2x::iop::detail
{
    const IopNativeOverride *iopNativeOverrides(size_t &count)
    {
        count = 0;
        return nullptr;
    }
}
