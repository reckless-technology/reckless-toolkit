#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "Artwork.h"
#include "Canvas.h"
#include "ParameterControl.h"
#include "PresetModel.h"
#include "View.h"

namespace reckless::view
{
/** What a panel is given by the format that shows it: its parameters as controls by id; the suite's version
    ("0.14.0"); the toolkit's images - a file's bytes decoded, a part of an image, and an image resampled to a size
    at a quality, where kHigh is a plain bilinear read, since the series' look halves its cached pictures down by
    2:1 resamples that average each two by two block; the files the module carries, by name; the plugin's presets,
    or none where the format has none yet; the toolkit's drawing of a meter's artwork; and the platform's showing of a
    folder. */
struct PanelContext
{
    /** A parameter's control by its string id: `id` the parameter's id, the control owned by the format and valid for
        the panel's life; the still control for an id the plugin lacks. */
    std::function<ParameterControl &(std::string_view id)> parameter;

    /** The suite's version, "MAJOR.MINOR.BUILD". */
    std::string version;

    /** Whether the plugin plays in demo mode now - a demo build and no valid licence - which its header says beside
        the version; asked once a display frame. Null for never: the suites' hosts and the render tool. */
    std::function<bool()> demo;

    /** Whether a newer release of the suite is out, which its header says beside the version (update/'s check);
        asked once a display frame. Null for never: the suites' hosts, the render tool and a pedal on the board. */
    std::function<bool()> update_available;

    /** A file's bytes decoded: `encoded` the file's bytes; an invalid image for bytes it cannot decode. */
    std::function<gfx::Image(std::span<const std::uint8_t> encoded)> decode_image;

    /** An image of decoded pixels: `width` x `height`, `rgba` eight bits each of red, green, blue and alpha,
        unpremultiplied, row by row; an invalid image where the format draws none. */
    std::function<gfx::Image(int width, int height, std::span<const std::uint8_t> rgba)> image_from_pixels;

    /** A part of an image: `image` the source, `area` the part, in its pixels. */
    std::function<gfx::Image(const gfx::Image &image, gfx::RectI area)> sub_image;

    /** An image resampled: `image` the source, `width` and `height` the size to make, in pixels, and the quality. */
    std::function<gfx::Image(const gfx::Image &image, int width, int height, gfx::Resampling)> rescale_image;

    /** A file the module carries, by name: `name` as the module embeds it; an empty span for none, valid for the
        module's life. */
    std::function<std::span<const std::uint8_t>(std::string_view name)> asset;

    /** The plugin's presets, owned by the format and outliving the panel; nullptr where the format has none yet. */
    PresetModel *presets{nullptr};

    /** A vector document and a shape's soft shadow drawn by the toolkit on a canvas of its own kind
        (gfx::ArtworkLayer) - a meter's face and glass, into the picture the meter keeps; nothing where the format
        draws none, or on a canvas of another kind.
        @param canvas  the canvas to draw on, in design pixels
        @param layer   the document or shadow to draw */
    std::function<void(gfx::Canvas &canvas, const gfx::ArtworkLayer &layer)> draw_artwork;

    /** A folder shown in the platform's file manager, as the format's own presets show theirs: for presets a panel
        keeps beside the format's (the Pedalboard's pedals' own); nothing where the format shows none.
        @param folder  the folder's path */
    std::function<void(const std::string &folder)> reveal_folder;

    /** A web page opened in the person's own browser, as a native app hands a sign-in to it: the TONE3000 catalogue,
        a tone's page; nothing where the format opens none.
        @param url  the page's URL, http or https */
    std::function<void(const std::string &url)> open_url;
};

/** A plugin's whole panel: the root of its views, at its design size, stepped once a display frame. */
class Panel : public View
{
  public:
    /** The size the panel lays itself out at, in its own design pixels: whoever shows it scales it, and the panel
        never learns the scale.
        @return  the width, in design pixels */
    int DesignWidth() const noexcept
    {
        return design_width_;
    }

    /** @return  the height, in design pixels */
    int DesignHeight() const noexcept
    {
        return design_height_;
    }

    /** Once a display frame, `seconds` since the frame before (FrameSteps: a nominal thirtieth the first time, never
        more than a tenth): whatever follows the parameters, and whatever moves on its own - a lamp's filament, a
        needle - in seconds, so it reads the same at any display rate. Message thread.
        @param seconds  the time since the frame before, in seconds, at most a tenth; nothing by default */
    virtual void Frame(double seconds)
    {
        (void)seconds;
    }

    /** The window's word, each display frame before Frame, whether the panel is on a screen a person can see - in a
        window that shows and is not minimised; never a panel kept off screen, as the Pedalboard keeps one for its
        miniature - and false as the window lets it go: what the window tells the panel's core too
        (core::PanelWatch). ShownChanged hears each change.
        @param on_screen  true when a person can see the panel */
    void SetShown(bool on_screen)
    {
        if (on_screen != shown_)
        {
            shown_ = on_screen;
            ShownChanged(on_screen);
        }
    }

    /** Whether the panel was on a screen at the last frame: what a screen that runs only for someone looking (a
        waterfall's stack) advances on. False until a window finds it on one.
        @return  true when the panel was on a screen at the last frame */
    bool shown() const noexcept
    {
        return shown_;
    }

    /** Who shows the panel - a window's shell - hears here each time the panel changes its own design size
        (SetDesignSize), and follows it: the window the new size times the scale in force, or in scroll mode the
        window it had held to the new panel, and a host's resizing held to the new aspect from then on. Set by that
        window, one at a time; empty before one shows the panel. */
    std::function<void()> on_design_size;

  protected:
    /** @param design_width   the panel's design width, in design pixels
        @param design_height  the panel's design height, in design pixels */
    Panel(int design_width, int design_height) noexcept : design_width_(design_width), design_height_(design_height)
    {
    }

    /** The panel came onto a screen, or went off one: a waterfall drops its stack as it goes.
        @param on_screen  true when it came on, false when it went off */
    virtual void ShownChanged(bool on_screen)
    {
        (void)on_screen;
    }

    /** The panel at a design size of its own choosing - a drawer opened, a mode's extra row, another of its views -
        laid out at it, in its own design pixels as ever, and whoever shows it told (on_design_size), which follows at
        the scale in force. Nothing when the size stands. A panel that changes its size in its constructor tells no
        one: the window it opens in takes the size it has.
        @param width   the new design width, in design pixels
        @param height  the new design height, in design pixels */
    void SetDesignSize(int width, int height)
    {
        if (width == design_width_ && height == design_height_)
        {
            return;
        }
        design_width_  = width;
        design_height_ = height;
        SetBounds({width, height});
        if (on_design_size)
        {
            on_design_size();
        }
    }

  private:
    int  design_width_;  ///< the design width, in design pixels
    int  design_height_; ///< the design height, in design pixels
    bool shown_{false};  ///< on a screen at the last SetShown
};
} // namespace reckless::view
