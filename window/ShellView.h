#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Canvas.h"
#include "Geometry.h"
#include "Json.h"
#include "LicenceDialog.h"
#include "LicenceStore.h"
#include "LookMetrics.h"
#include "Menu.h"
#include "PanelStyle.h"
#include "PanelView.h"
#include "PresetModel.h"
#include "ScrollBar.h"
#include "SeriesPanel.h"
#include "Tooltip.h"
#include "UpdateCheck.h"
#include "UpdatesMenu.h"
#include "View.h"
#include "ZoomView.h"

namespace reckless::window
{
/** What the shell keeps across windows: the session's "ui", where the window's size rides, and the person's window
    mode, the suite's one preference (core::InterfaceSettings). Every format keeps it on its session the one way
    (SessionShellStore.h); a suite keeps it on a fake. */
class ShellStore
{
  public:
    virtual ~ShellStore() = default;

    /** A copy of the session's "ui": an object, empty where nothing is kept there.
        @return  the "ui" object, by value */
    virtual core::json::Value Ui() const = 0;

    /** `write` run on the session's "ui" (HostSession::WriteUi), under whatever guards the session.
        @param write  what to do to the "ui" object, run once and before this returns */
    virtual void WriteUi(const std::function<void(core::json::Value &ui)> &write) = 0;

    /** The window's size moved the session: the host told it, as a loaded file would tell it. */
    virtual void UiChanged() = 0;

    /** The person's window mode: scroll bars, or scale to fit. Read once, when a window opens.
        @return  true for scroll bars, false for scale to fit */
    virtual bool ScrollMode() const = 0;

    /** The mode written for the next window to open.
        @param scroll  true for scroll bars, false for scale to fit */
    virtual void SetScrollMode(bool scroll) = 0;
};

/** How the shell is dressed: the panel's colour beside a letterboxed panel and under scroll bars, and its tooltips. */
struct ShellStyle
{
    /** The strip beside a letterboxed panel, the corner between two scroll bars and the bars' track. */
    gfx::Color          letterbox{gfx::Color::FromRgb(0, 0, 0)};
    view::TooltipStyle  tooltip{}; ///< how the shell's tooltip is drawn
    view::Dialog::Style dialog{};  ///< how the licence's card is drawn
};

/** The shell's dress for `panel`: a series panel's colour and its palette's tooltip; the defaults for any other.
    @param panel  the panel to dress the shell for; read for its palette, not kept
    @return       the style */
inline ShellStyle ShellStyleOf(view::Panel &panel)
{
    ShellStyle style;
    if (auto *series = dynamic_cast<panel::SeriesPanel *>(&panel))
    {
        const look::Palette &palette = series->chrome().palette;
        style.letterbox              = palette.panel;
        style.tooltip.background     = palette.panel.Brighter(0.08f);
        style.tooltip.outline        = palette.accent.WithAlpha(0.5f);
        style.tooltip.text           = palette.ink;
        style.dialog                 = panel::DialogStyle(palette);
    }
    return style;
}

/**
    The window round a panel in every format: the root view of the window a format's editor puts in its host's
    (window/'s child window), holding the panel and doing what the host's window asks of it in the way the person
    at the machine chose once for the suite, rule for rule as the releases built on JUCE did:

    - **Scale to fit.** The panel shown at a scale s, 50 to 200 %, in a ZoomView (view/ZoomView.h: the panel
      never learns it), centred on whole pixels in a window that is the design size times s; a size off the panel's
      aspect - a host's, rounded - letterboxes it in the panel's own colour. A size the host proposes is held to the
      aspect and the limits as JUCE's VST3 wrapper holds one to the shell's constrainer (Constrain), and a size the host
      gives makes s the fit, unless it is within a pixel's rounding of the scale in force, which then stands. A panel
      that resizes itself (view::Panel::on_design_size: the Amp's two views, the Oldsverb's SHIMMER row, a drawer) is
      followed at the s in force, and the aspect a host's size is held to moves with it.
    - **Scroll bars.** The panel at its design size in a viewport filling the window, the series' 10-pixel scroll bars
      (view/ScrollBar.h) where the window is smaller - the window held between 320 by 240 and the panel's size -
      laid out as JUCE's Viewport lays them out; the wheel over the panel is the panel's.

    The size rides on the session's "ui" (ShellStore) - `scale` in scale mode, `width` and `height` in scroll
    mode, each written only when it differs from what the child implies, so opening a window marks nothing modified -
    and the host is told the session moved 400 ms after the last change; a preset never carries it
    (panel::HostPresetModel). The View submenu, which the settings' gear beside the PRESETS plate lists (MenuItems,
    Perform; panel::SettingsPlateView):
    Scale to fit and Scroll bars, the current one ticked, and the sizes 75, 100, 125, 150 and 200 %, the current one
    ticked, greyed in scroll mode; a mode chosen is written for the next window to open, the panel moved into or out of
    the viewport, and the window asked for what the mode remembers.

    The shell's units are the window's own over the display's scale: points on macOS, pixels over the host's scale on
    Windows and Linux - the units JUCE's components measure in. The window draws the shell at the display's scale; the
    panel's zoom is the shell's. A tooltip shows as JUCE's tooltip window shows one (view/Tooltip.h): inside the
    shell, at the shell's scale and not the panel's zoom, after the pointer rests on a view with a tip. A layout under a
    pointer that stayed where it was finds the panel's view under it again, as JUCE's fake mouse move after a bounds
    change does, so the hover and the tip follow the panel and not the pointer's last event.

    The format sizes the shell (SetBounds) to the window it has, and the shell asks for a new size through on_size -
    a mode or a size chosen from the View submenu - having laid itself out at that size already. Frame, once a display
    frame, steps the panel, the host's notice, the scroll bars' page repeat and the tooltip.
*/
class ShellView final : public view::View
{
  public:
    /** How the panel fills the window. */
    enum class Mode
    {
        kScale, ///< the panel scaled to fit the window, its aspect kept
        kScroll ///< the panel at its design size behind scroll bars
    };

    static constexpr double kMinScale = 0.5; ///< the smallest scale in scale mode, 50 %
    static constexpr double kMaxScale = 2.0; ///< the largest scale in scale mode, 200 %

    /** The View submenu's fixed sizes, in percent of the design size. */
    static constexpr int kSizes[] = {75, 100, 125, 150, 200};
    /** The number of entries in kSizes. */
    static constexpr int kNumSizes = static_cast<int>(sizeof(kSizes) / sizeof(kSizes[0]));

    /** The smallest window scroll mode allows: room for a knob and two scroll bars. */
    static constexpr int kMinScrollWidth  = 320; ///< the smallest window width in scroll mode, in shell units
    static constexpr int kMinScrollHeight = 240; ///< the smallest window height in scroll mode, in shell units

    static constexpr int kScrollBarThickness = 10; ///< the scroll bars' thickness, in shell units

    /** How long after the last change of size the host is told the session moved. */
    static constexpr double kNotifySeconds = 0.4;

    /** The View submenu's items' ids, the window's own (view::PresetModel::kExtensionBase and up). */
    static constexpr int kModeScaleItem  = view::PresetModel::kExtensionBase;     ///< Scale to fit
    static constexpr int kModeScrollItem = view::PresetModel::kExtensionBase + 1; ///< Scroll bars
    /** The first of the sizes' ids, in kSizes' order. */
    static constexpr int kSizeItemBase = view::PresetModel::kExtensionBase + 10;
    /** The Licence... item's id: the licence's card (LicenceDialog). */
    static constexpr int kLicenceItem = view::PresetModel::kExtensionBase + 200;
    /** The Updates submenu's items' ids: the download page for a newer release, a check now, checks by the clock. */
    static constexpr int kUpdatePageItem = view::PresetModel::kExtensionBase + 210;
    static constexpr int kCheckNowItem   = view::PresetModel::kExtensionBase + 211; ///< Check for updates now
    static constexpr int kCheckDailyItem = view::PresetModel::kExtensionBase + 212; ///< Check automatically
    /** The three as the toolkit's Updates menu takes them (update/UpdatesMenu.h). */
    static constexpr update::UpdatesMenuIds kUpdatesMenuIds{kUpdatePageItem, kCheckNowItem, kCheckDailyItem};
    /** How often the shell polls the update check from its frame: often enough to show an answer within a second. */
    static constexpr double kUpdatePollSeconds = 1.0;

    /** A clock: the time in seconds on a steady scale, for the scroll bars' repeat and the tooltip's rest. */
    using Clock = std::function<double()>;

    /** The shell round `panel`, laid out at the size the store remembers.
        @param panel  the panel to hold, which outlives the shell
        @param style  the letterbox colour and the tooltip's look
        @param store  what the shell keeps across windows, which outlives the shell
        @param clock  seconds on a steady scale; the steady clock by default, a suite's own to step time by hand */
    ShellView(view::Panel &panel, ShellStyle style, ShellStore &store, Clock clock = SteadySeconds)
        : panel_(panel), style_(std::move(style)), store_(store), clock_(std::move(clock)), zoom_(panel),
          vertical_(true), horizontal_(false), tooltip_(style_.tooltip), tracker_(look::kTooltipDelayMs / 1000.0)
    {
        panel_.SetBounds({panel_.DesignWidth(), panel_.DesignHeight()});
        mode_ = store_.ScrollMode() ? Mode::kScroll : Mode::kScale;
        ReadState();

        holder_.SetInterceptsMouse(false, true);
        AddChild(holder_);
        AddChild(vertical_);
        AddChild(horizontal_);
        vertical_.SetSingleStepSize(16.0);
        horizontal_.SetSingleStepSize(16.0);
        vertical_.on_move   = [this](double start) { Scroll(viewport_x_, static_cast<int>(std::lround(start))); };
        horizontal_.on_move = [this](double start) { Scroll(static_cast<int>(std::lround(start)), viewport_y_); };
        const view::PointerWatch watch = [this](view::PointerEvent event, gfx::PointF at) { Pointer(event, at); };
        zoom_.watch                    = watch;
        vertical_.watch                = watch;
        horizontal_.watch              = watch;
        AddChild(tooltip_); // last: in front of everything, taking no mouse
        last_tick_ = clock_();
        Place(false);
        const gfx::PointI size = WantedSize();
        SetBounds({size.x, size.y});
        panel_.on_design_size = [this] { PanelResized(); };
    }

    /** The panel's design-size callback cleared, so the panel never reaches a shell that has gone. */
    ~ShellView() override
    {
        panel_.on_design_size = nullptr;
    }

    /** The machine's licence the settings' menu offers as Licence... (kLicenceItem), and how the card opens the shop.
        @param store     the licence (a product's shared store, licence::Shared() in vst-plugins' formats), outliving
                         the shell; null for no item
        @param open_url  opens a page in the person's browser; null hides the card's Buy... */
    void SetLicence(licence::Store *store, std::function<void(const std::string &)> open_url)
    {
        licence_  = store;
        open_url_ = std::move(open_url);
    }

    /** The update check the settings' menu offers as its Updates submenu, which the shell polls from its frame while
        its panel is on a screen, and how a newer release's page opens.
        @param checker   the process's checker (a product's, update::Shared() in vst-plugins' formats), outliving the
                         shell; null for no submenu and no check
        @param open_url  opens a page in the person's browser */
    void SetUpdates(update::Checker *checker, std::function<void(const std::string &)> open_url)
    {
        updates_          = checker;
        updates_open_url_ = std::move(open_url);
    }

    /** The licence's card over the window, its own while it is up (OpenLicence).
        @return  the card, or null while none is up */
    LicenceDialog *licence_dialog() noexcept
    {
        return licence_dialog_.get();
    }

    /** The licence's card over the whole window, in front of the panel: nothing without a licence to show, and one
        card at a time. */
    void OpenLicence()
    {
        if (licence_ == nullptr || licence_dialog_)
        {
            return;
        }
        licence_dialog_ = std::make_unique<LicenceDialog>(*this, *licence_, style_.dialog, open_url_,
                                                          [this] { licence_dialog_.reset(); });
    }

    /** The format's word that the shell's window should be this size, in shell units: a mode or a size the person
        chose. The shell has laid itself out at that size already. */
    std::function<void(int width, int height)> on_size;

    /** The mode the shell is in.
        @return  scale to fit or scroll bars */
    Mode mode() const noexcept
    {
        return mode_;
    }

    /** The panel's scale in scale mode: the window's size over the design size. 1 in scroll mode.
        @return  the scale, 0.5 to 2 in scale mode */
    double scale() const noexcept
    {
        return mode_ == Mode::kScale ? scale_ : 1.0;
    }

    /** The panel the shell holds.
        @return  the panel, owned by the caller of the constructor */
    view::Panel &panel() noexcept
    {
        return panel_;
    }

    /** The size the shell wants its window, in shell units: the design size times the scale, or in scroll mode the
        size it last had, held to the mode's limits.
        @return  the width and height, in shell units */
    gfx::PointI WantedSize() const noexcept
    {
        const int w = std::max(1, panel_.DesignWidth());
        const int h = std::max(1, panel_.DesignHeight());
        if (mode_ == Mode::kScroll)
        {
            return {std::clamp(window_width_ > 0 ? window_width_ : w, std::min(kMinScrollWidth, w), w),
                    std::clamp(window_height_ > 0 ? window_height_ : h, std::min(kMinScrollHeight, h), h)};
        }
        return {Scaled(w), Scaled(h)};
    }

    /** A size a host proposes, in shell units, held to the shell's limits and, in scale mode, the panel's aspect, as
        JUCE's VST3 wrapper's checkSizeConstraint holds one to a ComponentBoundsConstrainer, in single precision: each
        side held to its limits, then, with an aspect, the side that makes the window too wide or too tall for it worked
        out from the other - and if that leaves its limits, held to them and the other worked out back.
        @param proposed  the width and height the host proposes, in shell units
        @return          the nearest size the shell allows, in shell units */
    gfx::PointF Constrain(gfx::PointF proposed) const noexcept
    {
        const Limits limits = LimitsOf();
        const auto   min_w  = static_cast<float>(limits.min_width);
        const auto   max_w  = static_cast<float>(limits.max_width);
        const auto   min_h  = static_cast<float>(limits.min_height);
        const auto   max_h  = static_cast<float>(limits.max_height);
        float        width  = std::clamp(proposed.x, min_w, max_w);
        float        height = std::clamp(proposed.y, min_h, max_h);
        const auto   aspect = static_cast<float>(limits.aspect);
        if (aspect > 0.0f)
        {
            if (width / height > aspect)
            {
                width = height * aspect;
                if (width > max_w || width < min_w)
                {
                    width  = std::clamp(width, min_w, max_w);
                    height = width / aspect;
                }
            }
            else
            {
                height = width / aspect;
                if (height > max_h || height < min_h)
                {
                    height = std::clamp(height, min_h, max_h);
                    width  = height * aspect;
                }
            }
        }
        return {width, height};
    }

    /** The mode for this window, written to the preference for the next window to open: the panel moved, the window
        asked for the size the mode remembers.
        @param mode  the mode to show the panel in */
    void SetMode(Mode mode)
    {
        store_.SetScrollMode(mode == Mode::kScroll);
        if (mode == mode_)
        {
            return;
        }
        mode_ = mode;
        Place(true);
    }

    /** Scale mode: the window to the design size times `s`, held to the limits. Nothing in scroll mode.
        @param s  the scale, 0.5 to 2; held to that range */
    void SetScale(double s)
    {
        if (mode_ != Mode::kScale)
        {
            return;
        }
        scale_ = std::clamp(s, kMinScale, kMaxScale);
        Request({Scaled(panel_.DesignWidth()), Scaled(panel_.DesignHeight())});
    }

    /** The shell's items as the settings' gear lists them.
        @return  View, holding the two modes, a rule and the sizes, the current ones ticked; and Licence..., where
                 the shell has a licence to show (SetLicence) */
    std::vector<view::MenuItem> MenuItems() const
    {
        view::MenuItem submenu;
        submenu.text = "View";
        submenu.submenu.push_back(Item(kModeScaleItem, "Scale to fit", true, mode_ == Mode::kScale));
        submenu.submenu.push_back(Item(kModeScrollItem, "Scroll bars", true, mode_ == Mode::kScroll));
        view::MenuItem rule;
        rule.separator = true;
        submenu.submenu.push_back(rule);
        for (int i = 0; i < kNumSizes; ++i)
        {
            const double s = kSizes[i] / 100.0;
            submenu.submenu.push_back(Item(kSizeItemBase + i, std::to_string(kSizes[i]) + " %", mode_ == Mode::kScale,
                                           mode_ == Mode::kScale && std::fabs(scale_ - s) < 0.005));
        }
        std::vector<view::MenuItem> items{submenu};
        if (updates_ != nullptr)
        {
            items.push_back(UpdatesSubmenu());
        }
        if (licence_ != nullptr)
        {
            items.push_back(Item(kLicenceItem, "Licence...", true, false));
        }
        return items;
    }

    /** What a View item does: the two modes, and the sizes. An id the submenu does not hold does nothing.
        @param id  the chosen item's id, kModeScaleItem, kModeScrollItem or kSizeItemBase plus an index into kSizes */
    void Perform(int id)
    {
        if (id == kModeScaleItem)
        {
            SetMode(Mode::kScale);
        }
        else if (id == kModeScrollItem)
        {
            SetMode(Mode::kScroll);
        }
        else if (id >= kSizeItemBase && id < kSizeItemBase + kNumSizes)
        {
            SetScale(kSizes[id - kSizeItemBase] / 100.0);
        }
        else if (id == kLicenceItem)
        {
            OpenLicence();
        }
        else if (updates_ != nullptr)
        {
            update::ChooseUpdatesItem(*updates_, kUpdatesMenuIds, id, updates_open_url_);
        }
    }

    /** Once a display frame, `seconds` since the last: the panel's frame, the host's notice when the size has
        settled, the scroll bars' page repeat and the tooltip's tick.
        @param seconds  the time since the last frame, in seconds */
    void Frame(double seconds)
    {
        panel_.Frame(seconds);
        if (updates_ != nullptr)
        {
            update_poll_in_ -= seconds;
            if (update_poll_in_ <= 0.0)
            {
                update_poll_in_ = kUpdatePollSeconds;
                updates_->Poll(panel_.shown());
            }
        }
        if (notify_in_ > 0.0)
        {
            notify_in_ -= seconds;
            if (notify_in_ <= 0.0)
            {
                notify_in_ = 0.0;
                store_.UiChanged();
            }
        }
        const double now = clock_();
        vertical_.Step(now);
        horizontal_.Step(now);
        if (now - last_tick_ >= view::TooltipTracker::kTickSeconds)
        {
            last_tick_ = now;
            Tick(now);
        }
    }

    // ---- View ------------------------------------------------------------------------------------------------------

    /** Fills the shell with the letterbox colour.
        @param c  the canvas to paint on */
    void Paint(gfx::Canvas &c) override
    {
        // The strip beside a letterboxed panel, the corner between two scroll bars and the bars' track in the panel's
        // colour; under a panel that covers the window, the panel paints over it.
        c.FillAll(style_.letterbox);
    }

    /** Lays out the panel for the mode: the zoom centred on whole pixels, or the viewport with its bars. In scale mode
        a size off the scale in force becomes the new scale. */
    void Resized() override
    {
        if (laying_out_ || width() <= 0 || height() <= 0)
        {
            return;
        }
        laying_out_ = true;
        const int w = std::max(1, panel_.DesignWidth());
        const int h = std::max(1, panel_.DesignHeight());
        if (mode_ == Mode::kScroll)
        {
            LayOutViewport();
            RememberWindow();
        }
        else
        {
            // The scale the window asks for, and the intent it was set from: within a pixel's rounding of the design
            // size the intent stands, so a scale chosen never drifts; beyond that the host moved the window, and the
            // new fit is the scale.
            const double fit =
                std::min(static_cast<double>(width()) / static_cast<double>(w), static_cast<double>(height()) / h);
            if (std::fabs(fit - scale_) * std::max(w, h) > 1.0)
            {
                scale_ = std::clamp(fit, kMinScale, kMaxScale);
            }
            const auto s  = static_cast<float>(scale_);
            const auto dx = std::floor((static_cast<float>(width()) - static_cast<float>(w) * s) * 0.5f);
            const auto dy = std::floor((static_cast<float>(height()) - static_cast<float>(h) * s) * 0.5f);
            zoom_.SetZoom(s);
            const gfx::RectI area = zoom_.ContentArea();
            zoom_.SetBounds({static_cast<int>(std::max(dx, 0.0f)), static_cast<int>(std::max(dy, 0.0f)), area.width(),
                             area.height()});
            RememberScale();
        }
        if (licence_dialog_)
        {
            licence_dialog_->SetBounds(LocalBounds());
        }
        laying_out_ = false;
        PointerStill();
    }

    /** The pointer moved over one of the shell's own views: the tooltip's tracker follows it.
        @param e  the event, its position in the shell's coordinates */
    void MouseMove(const view::MouseEvent &e) override
    {
        Pointer(view::PointerEvent::kMove, e.position);
    }

    /** The pointer entered the shell: the tooltip's tracker follows it.
        @param e  the event, its position in the shell's coordinates */
    void MouseEnter(const view::MouseEvent &e) override
    {
        Pointer(view::PointerEvent::kMove, e.position);
    }

    /** The pointer left the shell: the tooltip's tracker is told it is gone.
        @param e  the event, its position in the shell's coordinates */
    void MouseExit(const view::MouseEvent &e) override
    {
        Pointer(view::PointerEvent::kExit, e.position);
    }

    /** A button went down on one of the shell's own views: the tooltip goes away.
        @param e  the event, its position in the shell's coordinates */
    void MouseDown(const view::MouseEvent &e) override
    {
        Pointer(view::PointerEvent::kDown, e.position);
    }

    /** The button came up: the tooltip's tracker is told.
        @param e  the event, its position in the shell's coordinates */
    void MouseUp(const view::MouseEvent &e) override
    {
        Pointer(view::PointerEvent::kUp, e.position);
    }

    /** A wheel no view of the panel took - the router hands it up the views it is over - and the shell's own: the tip
        put away, as any wheel puts it away, and in scroll mode the panel scrolled under the window as JUCE's Viewport
        scrolls under an unused wheel - both ways when both bars show and the wheel moves both, across when only the
        horizontal bar shows or Shift is held, else down; never with Command or Alt held.
        @param e  the wheel event, its position in the shell's coordinates
        @return   true when it scrolled */
    bool MouseWheel(const view::MouseEvent &e) override
    {
        Pointer(view::PointerEvent::kWheel, e.position);
        if (mode_ != Mode::kScroll || e.modifiers.command || e.modifiers.alt)
        {
            return false;
        }
        const bool across = horizontal_.visible();
        const bool down   = vertical_.visible();
        const int  dx     = WheelPixels(e.wheel_x);
        const int  dy     = WheelPixels(e.wheel_y);
        int        x      = viewport_x_;
        int        y      = viewport_y_;
        if (dx != 0 && dy != 0 && across && down)
        {
            x -= dx;
            y -= dy;
        }
        else if (across && (dx != 0 || e.modifiers.shift || !down))
        {
            x -= dx != 0 ? dx : dy;
        }
        else if (down && dy != 0)
        {
            y -= dy;
        }
        const gfx::PointI before{viewport_x_, viewport_y_};
        if (x != viewport_x_ || y != viewport_y_)
        {
            Scroll(x, y);
        }
        return viewport_x_ != before.x || viewport_y_ != before.y;
    }

    // ---- For the suites ---------------------------------------------------------------------------------------------

    /** The zoom the panel sits in.
        @return  the zoom view, owned by the shell */
    view::ZoomView &zoom_view() noexcept
    {
        return zoom_;
    }

    /** The viewport's content area, which holds the zoom in scroll mode.
        @return  the holder view, owned by the shell */
    view::View &viewport() noexcept
    {
        return holder_;
    }

    /** The vertical scroll bar.
        @return  the bar, owned by the shell */
    view::ScrollBar &vertical_bar() noexcept
    {
        return vertical_;
    }

    /** The horizontal scroll bar.
        @return  the bar, owned by the shell */
    view::ScrollBar &horizontal_bar() noexcept
    {
        return horizontal_;
    }

    /** The tooltip's view.
        @return  the view, owned by the shell */
    view::TooltipView &tooltip() noexcept
    {
        return tooltip_;
    }

    /** The tracker that decides when the tooltip shows.
        @return  the tracker, owned by the shell */
    const view::TooltipTracker &tooltips() const noexcept
    {
        return tracker_;
    }

    /** Where the viewport shows the panel from, in scroll mode.
        @return  the panel's x and y at the viewport's top left, in design pixels */
    gfx::PointI view_position() const noexcept
    {
        return {viewport_x_, viewport_y_};
    }

  private:
    /** The size limits a host's proposal is held to. */
    struct Limits
    {
        double aspect{0.0};         ///< width over height the window keeps; zero for none
        int    min_width{1};        ///< the smallest width, in shell units
        int    min_height{1};       ///< the smallest height, in shell units
        int    max_width{1 << 20};  ///< the largest width, in shell units
        int    max_height{1 << 20}; ///< the largest height, in shell units
    };

    /** The steady clock's time, the default Clock.
        @return  seconds since the clock's epoch */
    static double SteadySeconds()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    /** The Updates submenu: where the check stands, a newer release's page, a check now and checks by the clock - the
        toolkit's Updates menu (update/UpdatesMenu.h) on the shell's ids.
        @return  the submenu */
    view::MenuItem UpdatesSubmenu() const
    {
        return update::UpdatesSubmenu(*updates_, kUpdatesMenuIds);
    }

    /** One View submenu item.
        @param id       the item's id
        @param text     the item's label
        @param enabled  false greys it
        @param ticked   true marks it as the current choice
        @return         the item */
    static view::MenuItem Item(int id, std::string text, bool enabled, bool ticked)
    {
        view::MenuItem item;
        item.id      = id;
        item.text    = std::move(text);
        item.enabled = enabled;
        item.ticked  = ticked;
        return item;
    }

    /** A design size at the scale in force.
        @param design  a width or height, in design pixels
        @return        the size in shell units, rounded, at least 1 */
    int Scaled(int design) const noexcept
    {
        return std::max(1, gfx::RoundToInt(design * scale_));
    }

    /** The shell's limits for the mode: the aspect and 50 to 200 % in scale mode, between the small minimum and the
        panel's size in scroll mode.
        @return  the limits */
    Limits LimitsOf() const noexcept
    {
        const int w = std::max(1, panel_.DesignWidth());
        const int h = std::max(1, panel_.DesignHeight());
        Limits    limits;
        if (mode_ == Mode::kScroll)
        {
            limits.min_width  = std::min(kMinScrollWidth, w);
            limits.min_height = std::min(kMinScrollHeight, h);
            limits.max_width  = w;
            limits.max_height = h;
        }
        else
        {
            limits.aspect     = static_cast<double>(w) / h;
            limits.min_width  = std::max(1, gfx::RoundToInt(w * kMinScale));
            limits.min_height = std::max(1, gfx::RoundToInt(h * kMinScale));
            limits.max_width  = gfx::RoundToInt(w * kMaxScale);
            limits.max_height = gfx::RoundToInt(h * kMaxScale);
        }
        return limits;
    }

    /** The panel where the mode puts it - the shell's own child scaled, or in the viewport at its design size - and,
        when `resize` asks, the window asked for what the mode remembers.
        @param resize  true to ask the window for the mode's size, false to lay out at the size it has */
    void Place(bool resize)
    {
        if (mode_ == Mode::kScroll)
        {
            if (zoom_.parent() != &holder_)
            {
                zoom_.SetZoom(1.0f);
                holder_.AddChild(zoom_);
            }
            holder_.SetVisible(true);
            vertical_.SetShown(true);
            horizontal_.SetShown(true);
        }
        else
        {
            if (zoom_.parent() != this)
            {
                AddChild(zoom_);
                RemoveChild(tooltip_);
                AddChild(tooltip_); // the tooltip stays in front
            }
            holder_.SetVisible(false);
            vertical_.SetShown(false);
            horizontal_.SetShown(false);
        }
        if (resize)
        {
            Request(WantedSize());
        }
        else
        {
            Resized();
        }
    }

    /** The panel changed its own design size (view::Panel::SetDesignSize: a drawer, a mode's row, another view):
        the window asked for the size the mode gives it now - the new design size times the scale in force, or in scroll
        mode the window it had held to the new panel - and laid out at it, the zoom round the panel's new size, and a
        host's proposals held to the new aspect from here on (Constrain reads the design size). A window that keeps its
        size - a panel grown under scroll bars - is laid out again and nothing asked. */
    void PanelResized()
    {
        const gfx::PointI size = WantedSize();
        if (size.x == width() && size.y == height())
        {
            Resized();
            return;
        }
        Request(size);
    }

    /** The shell at `size`, laid out, and the window asked for it.
        @param size  the width and height, in shell units */
    void Request(gfx::PointI size)
    {
        if (size.x == width() && size.y == height())
        {
            Resized(); // the size stands: the layout is the mode's
        }
        else
        {
            SetBounds({size.x, size.y});
        }
        if (on_size)
        {
            on_size(size.x, size.y);
        }
    }

    /** The "ui"'s size. A scale that is not a positive number - a damaged or hand-edited session's - would open a
        window of no size that never recovers: the design size instead, which RememberScale writes over it. */
    void ReadState()
    {
        const core::json::Value ui     = store_.Ui();
        const double            stored = ui.NumberOr("scale", 1.0);
        scale_                         = std::clamp(stored > 0.0 ? stored : 1.0, kMinScale, kMaxScale);
        window_width_                  = IntOf(ui, "width", 0);
        window_height_                 = IntOf(ui, "height", 0);
    }

    /** An integer from the "ui".
        @param ui        the "ui" object
        @param name      the field's name
        @param fallback  what to return where the field is missing or not a number
        @return          the field, held between 0 and 2^20 */
    static int IntOf(const core::json::Value &ui, std::string_view name, int fallback)
    {
        return static_cast<int>(std::clamp<std::int64_t>(ui.IntOr(name, fallback), 0, 1 << 20));
    }

    // A size the session already implies - no "ui" and the design size, or what the "ui" holds - writes nothing:
    // opening a window must not mark a host's project modified, only moving it.

    /** Writes the scale to the "ui" when it differs from what the session implies, and starts the host's notice. */
    void RememberScale()
    {
        const core::json::Value  ui     = store_.Ui();
        const core::json::Value *stored = ui.Find("scale");
        const bool same = stored == nullptr ? std::fabs(scale_ - 1.0) <= 1.0e-9
                                            : stored->IsNumber() && std::fabs(stored->AsDouble() - scale_) <= 1.0e-9;
        if (!same)
        {
            store_.WriteUi([this](core::json::Value &write) { write.Set("scale", scale_); });
            notify_in_ = kNotifySeconds;
        }
    }

    /** Writes the window's size to the "ui" when it differs from what the session implies, and starts the host's
        notice. */
    void RememberWindow()
    {
        const core::json::Value ui = store_.Ui();
        const int               w  = IntOf(ui, "width", panel_.DesignWidth());
        const int               h  = IntOf(ui, "height", panel_.DesignHeight());
        if (w != width() || h != height())
        {
            store_.WriteUi([this](core::json::Value &write) {
                write.Set("width", width());
                write.Set("height", height());
            });
            notify_in_ = kNotifySeconds;
        }
        window_width_  = width();
        window_height_ = height();
    }

    /** Juce's Viewport::updateVisibleArea, for a viewport filling the shell with both bars allowed: which bars show,
        the content area they leave, the panel's position in it held to what can show, and the bars' ranges. */
    void LayOutViewport()
    {
        const int        bar          = kScrollBarThickness;
        const int        content_w    = zoom_.ContentArea().width(); // the panel at its design size: the zoom is 1 here
        const int        content_h    = zoom_.ContentArea().height();
        const bool       can_show_any = width() > bar && height() > bar;
        bool             h_visible    = false;
        bool             v_visible    = false;
        gfx::RectI       area         = LocalBounds();
        const gfx::RectI content{-viewport_x_, -viewport_y_, content_w, content_h};
        if (!area.Contains(content))
        {
            h_visible = can_show_any && (content.x() < 0 || content.right() > area.width());
            v_visible = can_show_any && (content.y() < 0 || content.bottom() > area.height());
            if (v_visible)
            {
                area = area.WithWidth(width() - bar);
            }
            if (h_visible)
            {
                area = area.WithHeight(height() - bar);
            }
            if (!area.Contains(content))
            {
                h_visible = can_show_any && (h_visible || content.right() > area.width());
                v_visible = can_show_any && (v_visible || content.bottom() > area.height());
            }
        }
        area = LocalBounds();
        if (v_visible)
        {
            area = area.WithWidth(width() - bar);
        }
        if (h_visible)
        {
            area = area.WithHeight(height() - bar);
        }
        holder_.SetBounds(area);
        // The view position held to what the content area can show.
        viewport_x_ = std::clamp(viewport_x_, 0, std::max(0, content_w - area.width()));
        viewport_y_ = std::clamp(viewport_y_, 0, std::max(0, content_h - area.height()));
        if (!h_visible)
        {
            viewport_x_ = 0;
        }
        if (!v_visible)
        {
            viewport_y_ = 0;
        }
        zoom_.SetBounds({-viewport_x_, -viewport_y_, content_w, content_h});
        horizontal_.SetBounds({area.x(), area.height(), area.width(), bar});
        horizontal_.SetRangeLimits(0.0, content_w);
        horizontal_.SetCurrentRange(viewport_x_, area.width());
        vertical_.SetBounds({area.width(), area.y(), bar, area.height()});
        vertical_.SetRangeLimits(0.0, content_h);
        vertical_.SetCurrentRange(viewport_y_, area.height());
        horizontal_.SetShown(h_visible);
        vertical_.SetShown(v_visible);
    }

    /** The viewport moved to show the panel from (x, y): a bar dragged, paged or wheeled.
        @param x  the panel's x at the viewport's left, in design pixels
        @param y  the panel's y at the viewport's top, in design pixels */
    void Scroll(int x, int y)
    {
        viewport_x_ = x;
        viewport_y_ = y;
        LayOutViewport();
    }

    /** A wheel's distance as JUCE's Viewport moves by it: its delta in JUCE's units - 100 of the views' pixels one -
        times fourteen single steps of 16 pixels, rounded, and at least a pixel either way. The child windows hand a
        wheel's notch on as 20 pixels on Windows and Linux (their kNotchPixels), a Mac's line wheel as 10 a line and a
        trackpad's pixels as they come, so a notch scrolls 45 pixels.
        @param wheel  the wheel's distance in the views' pixels, zero for none
        @return       the distance to scroll, in pixels: 0 for none, otherwise at least 1 in magnitude */
    static int WheelPixels(float wheel)
    {
        if (std::abs(wheel) < 1.0e-6f)
        {
            return 0;
        }
        const float distance = wheel / 100.0f * 14.0f * 16.0f;
        return gfx::RoundToInt(distance < 0.0f ? std::min(distance, -1.0f) : std::max(distance, 1.0f));
    }

    // ---- The tooltip ----------------------------------------------------------------------------------------------

    /** A mouse event on one of the shell's views, its position in the shell's coordinates.
        @param event  what the pointer did
        @param at     where, in the shell's coordinates */
    void Pointer(view::PointerEvent event, gfx::PointF at)
    {
        if (event != view::PointerEvent::kExit)
        {
            pointer_ = at;
        }
        // A view left is told before the one entered, so the last word says whether the pointer is over the shell.
        inside_ = event != view::PointerEvent::kExit;
        switch (event)
        {
        case view::PointerEvent::kMove:
            tracker_.PointerAt(at);
            if (tooltip_.visible() && tooltip_.bounds().ToFloat().Contains(at))
            {
                tracker_.Hide(clock_()); // onto the tip: it goes, as JUCE's does on the pointer's entering it
                ShowTracked();
            }
            break;
        case view::PointerEvent::kExit:
            tracker_.PointerLeft();
            break;
        case view::PointerEvent::kDown:
            held_ = true;
            tracker_.Pressed(at);
            break;
        case view::PointerEvent::kUp:
            held_ = false;
            tracker_.Released();
            break;
        case view::PointerEvent::kWheel:
            tracker_.Wheeled();
            break;
        }
    }

    /** After a layout under a pointer that stayed where it was - a size the host gave, a mode, a scale - the panel's
       view under the pointer found again, as JUCE sends a fake mouse move when a showing component's bounds change: the
        view the panel moved out from under it told it left, the one moved under it that it entered, and a tip showing
        replaced at the next tick. Not while a button is held, as JUCE fakes no move during a drag. */
    void PointerStill()
    {
        if (!inside_ || held_)
        {
            return;
        }
        if (view::ViewAt(*this, pointer_.ToNearestInt()) == &zoom_)
        {
            const gfx::PointF origin = zoom_.ToRoot(gfx::PointF{0.0f, 0.0f});
            zoom_.PointerStill({pointer_.x - origin.x, pointer_.y - origin.y});
        }
        else
        {
            zoom_.PointerGone();
        }
    }

    /** One tick of the tooltip's timer: what is under the pointer - the shell's view there, and for the panel the tip
       of the view the panel's router has hovered - and the tip shown, moved or hidden as the tracker decides.
        @param now  the clock's time, in seconds */
    void Tick(double now)
    {
        view::TooltipTracker::Under under;
        // The view of the shell's under the pointer: the panel's zoom, a bar, or the shell itself.
        under.view = this;
        if (const view::View *found = view::ViewAt(*this, pointer_.ToNearestInt()))
        {
            under.view = found;
        }
        if (under.view == &zoom_)
        {
            under.tip = view::TipOf(zoom_.router().hovered());
        }
        tracker_.Tick(now, under);
        ShowTracked();
    }

    /** The tooltip view brought to what the tracker shows. */
    void ShowTracked()
    {
        if (!tracker_.showing())
        {
            if (tooltip_.visible())
            {
                tooltip_.Hide();
            }
            return;
        }
        if (tooltip_.visible() && shows_ == tracker_.shows())
        {
            return;
        }
        shows_                         = tracker_.shows();
        const view::Host         *h    = host();
        const view::TooltipStyle &tip  = tooltip_.style();
        view::TooltipLayout       text = view::LayOutTooltip(tracker_.tip(), tip, [h, &tip](std::string_view line) {
            return h != nullptr ? h->TextWidth(tip.font, line)
                                : 0.5f * tip.font.height * static_cast<float>(line.size());
        });
        const gfx::RectI          at   = view::TooltipBounds(text, tracker_.shown_at(), LocalBounds());
        tooltip_.Show(tracker_.tip(), std::move(text), at);
    }

    view::Panel         &panel_;      ///< the panel the shell holds
    ShellStyle           style_;      ///< the letterbox colour and the tooltip's look
    ShellStore          &store_;      ///< what the shell keeps across windows
    Clock                clock_;      ///< seconds on a steady scale
    view::View           holder_;     ///< the viewport's content area: the panel's zoom inside it in scroll mode
    view::ZoomView       zoom_;       ///< the zoom the panel sits in, in the shell or in the holder
    view::ScrollBar      vertical_;   ///< the vertical bar of scroll mode
    view::ScrollBar      horizontal_; ///< the horizontal bar of scroll mode
    view::TooltipView    tooltip_;    ///< the tooltip, in front of everything
    view::TooltipTracker tracker_;    ///< decides when the tooltip shows, moves and hides
    Mode                 mode_{Mode::kScale}; ///< the mode in force
    double               scale_{1.0}; ///< the scale in scale mode, 0.5 to 2: the intent a size within a pixel keeps
    int                  window_width_{0}; ///< the window's width in scroll mode, in shell units; 0 for the design size
    int             window_height_{0};  ///< the window's height in scroll mode, in shell units; 0 for the design size
    int             viewport_x_{0};     ///< the panel's x at the viewport's left, in design pixels
    int             viewport_y_{0};     ///< the panel's y at the viewport's top, in design pixels
    double          notify_in_{0.0};    ///< seconds until the host is told the session moved; 0 for no notice owed
    double          last_tick_{0.0};    ///< the clock's time at the last tooltip tick, in seconds
    unsigned        shows_{0};          ///< the tracker's count of tips shown when the tooltip view last took one
    gfx::PointF     pointer_{};         ///< the pointer's last position, in the shell's coordinates
    bool            inside_{false};     ///< the pointer over the shell, as its last event left it
    bool            held_{false};       ///< a button down on one of the shell's views
    bool            laying_out_{false}; ///< true inside Resized(), so the layout does not recurse
    licence::Store *licence_ = nullptr; ///< the licence the menu offers; null for none
    std::function<void(const std::string &)> open_url_;           ///< opens the shop from the card
    std::unique_ptr<LicenceDialog>           licence_dialog_;     ///< the licence's card while it is up
    update::Checker                         *updates_ = nullptr;  ///< the update check the menu offers; null for none
    std::function<void(const std::string &)> updates_open_url_;   ///< opens a newer release's page
    double                                   update_poll_in_ = 0; ///< seconds to the next poll of the update check
};
} // namespace reckless::window
