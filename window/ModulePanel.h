#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "Embedded.h"
#include "PanelView.h"
#include "SkiaArtwork.h"
#include "SkiaCanvas.h"
#include "Window.h"

namespace reckless::window
{
/** What a module's panel is made with, whatever format the module is: its parameters through `parameter` (the format's
    controls), the suite's `version`, the look's images on Skia - decoded, cut, and resampled with a plain bilinear
    read, as the look's halving wants (gfx::RescaleImage) - a meter's artwork drawn on Skia (gfx::DrawArtwork), the
    files the module carries by name (Embedded.h), the series' serif among them, handed to Skia once a process before
    any panel draws, a folder shown in the platform's file manager (RevealInFileManager), as the format's presets
    show theirs, decoded pixels made an image on Skia (gfx::ImageFromPixels) and a page opened in the person's browser
    (OpenInBrowser). The presets are the format's to add (`presets`: panel::HostPresetModel on its session); a context
    without them leaves the panel's PRESETS plate offering Reset to defaults alone.
    @param parameter  the format's control for a parameter id, made the first time it is asked for; the callable is kept
                      in the context and outlives the panel
    @param version    the suite's version, stamped in the panel's header
    @return           the context, to be given the presets before a panel is made on it */
inline view::PanelContext ModulePanelContext(std::function<view::ParameterControl &(std::string_view id)> parameter,
                                             std::string                                                  version)
{
    static const bool faces = [] {
        gfx::SetSerifFaces(embedded::Find("NotoSerif-Regular.ttf"), embedded::Find("NotoSerif-Bold.ttf"));
        return true;
    }();
    (void)faces;
    view::PanelContext context;
    context.parameter         = std::move(parameter);
    context.version           = std::move(version);
    context.asset             = [](std::string_view name) { return embedded::Find(name); };
    context.decode_image      = [](std::span<const std::uint8_t> encoded) { return gfx::DecodeImage(encoded); };
    context.image_from_pixels = [](int width, int height, std::span<const std::uint8_t> rgba) {
        return gfx::ImageFromPixels(width, height, rgba);
    };
    context.sub_image     = [](const gfx::Image &image, gfx::RectI area) { return gfx::SubImage(image, area); };
    context.rescale_image = [](const gfx::Image &image, int width, int height, gfx::Resampling quality) {
        return gfx::RescaleImage(image, width, height, quality);
    };
    context.draw_artwork = [](gfx::Canvas &canvas, const gfx::ArtworkLayer &layer) { gfx::DrawArtwork(canvas, layer); };
    context.reveal_folder = [](const std::string &folder) { RevealInFileManager(folder); };
    context.open_url      = [](const std::string &url) { OpenInBrowser(url); };
    return context;
}
} // namespace reckless::window
