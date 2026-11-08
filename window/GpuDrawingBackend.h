#pragma once

// The GPU back end a product links in to have its window drawn on the GPU (GpuDrawing.h): declared here and defined by
// reckless_window_gpu alone, so a product that includes this links that library, and one that never asks for the GPU
// links neither and carries none of the GPU's code. On Windows it is Skia's Ganesh on WGL
// (window/win/GpuDrawingWin.cpp); elsewhere, for now, it is none, and a window asked for the GPU there draws with the
// CPU raster and says kNoGpuOnThisPlatform.

#include "GpuDrawing.h"

namespace reckless::window
{
/** This build's GPU back end, for a DrawingRequest: Ganesh on WGL on Windows; null on a platform with none yet.
    @return  the back end, which lives as long as the program; null where the platform has none */
const GpuBackend *GpuDrawingBackend() noexcept;

/** The request for the GPU, with this build's back end named: what a product hands ChildWindow::SetDrawing to have its
    window drawn on the GPU wherever it can be.
    @return  the request */
inline DrawingRequest GpuDrawingRequest() noexcept
{
    return {Drawing::kGpu, GpuDrawingBackend()};
}
} // namespace reckless::window
