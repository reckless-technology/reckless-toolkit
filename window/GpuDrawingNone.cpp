// The GPU back end on a platform whose window has no GPU path - Linux built where libEGL's files were not found
// (skia/CMakeLists.txt): none, so a product may link reckless_window_gpu and ask for the GPU on every platform alike,
// and is drawn with the CPU raster where there is no GPU path, told kNoGpuOnThisPlatform.
#include "GpuDrawingBackend.h"

namespace reckless::window
{
const GpuBackend *GpuDrawingBackend() noexcept
{
    return nullptr;
}
} // namespace reckless::window
