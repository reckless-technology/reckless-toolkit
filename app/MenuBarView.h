#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "Canvas.h"
#include "Menu.h"
#include "MenuBar.h"
#include "View.h"

namespace reckless::app
{
/** How a drawn menu bar looks: the bar's colours, its titles' font and spacing, and its menus' look. */
struct MenuBarStyle
{
    gfx::Color      background{gfx::Color::FromRgb(40, 40, 40)};       ///< the bar's fill
    gfx::Color      rule{gfx::Color::FromRgb(90, 90, 90)};             ///< the line under the bar
    gfx::Color      text{gfx::Color::FromRgb(230, 230, 230)};          ///< an enabled title
    gfx::Color      disabled_text{gfx::Color::FromRgb(120, 120, 120)}; ///< a disabled title
    gfx::Color      highlight{gfx::Color::FromRgb(90, 90, 90)};        ///< behind the title whose menu is open
    gfx::Font       font{15.0f};                                       ///< the titles' font
    int             height{26};                                        ///< the bar's height, in design pixels
    int             padding{10};                                       ///< room either side of a title
    view::MenuStyle menu;                                              ///< the drop-downs' look
};

/**
    A menu bar drawn as a view, for a platform with no menu bar of its own - Linux under X11, where an app's windows
    carry their own - which the app places across the top of its window (MenuBarView::PreferredHeight high) and the
    platform makes (AppPlatform::MakeMenuBarView). It shows the bar its controller holds, a title for each menu; a
    press on a title opens that menu under it in the views' own popup (view::MenuSession), its items with their ticks,
    their submenus and their shortcuts' text, and a choice chooses the entry through the controller, as a native bar's
    click and a shortcut do. A press on the open menu's title, or outside the menu, closes it. The bar lays itself out
    again whenever the controller's bar is set anew (its revision), checked at each paint and press.
*/
class MenuBarView final : public view::View
{
  public:
    /** A bar showing `controller`'s menus.
        @param controller  the bar's model and its one way to choose, shared with the platform's shortcuts
        @param style       the look */
    explicit MenuBarView(std::shared_ptr<MenuBarController> controller, MenuBarStyle style = {})
        : controller_(std::move(controller)), style_(std::move(style))
    {
        watch_ = controller_->Watch([this] {
            laid_out_ = false;
            LayOut();
            Repaint();
        });
    }

    /** The open menu closed, and the bar no longer told of the controller's changes. */
    ~MenuBarView() override
    {
        controller_->Unwatch(watch_);
        session_.reset();
    }

    /** Not copyable: the controller tells it of changes by address. */
    MenuBarView(const MenuBarView &) = delete;

    /** Not assignable: the controller tells it of changes by address.
        @return  never */
    MenuBarView &operator=(const MenuBarView &) = delete;

    /** The height the app gives the bar.
        @return  the height, in design pixels */
    int PreferredHeight() const noexcept
    {
        return style_.height;
    }

    /** The bar, its titles and the open menu's title lit.
        @param c  the canvas, in the bar's local coordinates */
    void Paint(gfx::Canvas &c) override
    {
        LayOut();
        const gfx::RectI all = LocalBounds();
        c.SetColor(style_.background);
        c.FillRect(all);
        c.SetColor(style_.rule);
        c.DrawHorizontalLine(all.bottom() - 1, 0.0f, static_cast<float>(all.width()));
        c.SetFont(style_.font);
        const std::vector<MenuEntry> &menus = controller_->bar().menus;
        for (std::size_t i = 0; i < titles_.size() && i < menus.size(); ++i)
        {
            if (static_cast<int>(i) == open_)
            {
                c.SetColor(style_.highlight);
                c.FillRect(titles_[i]);
            }
            c.SetColor(menus[i].enabled ? style_.text : style_.disabled_text);
            c.DrawText(menus[i].title, titles_[i].ToFloat(), gfx::Justify::kCentred);
        }
    }

    /** A press on a title opens its menu, or closes it where it is the one open.
        @param e  the press: its position */
    void MouseDown(const view::MouseEvent &e) override
    {
        LayOut();
        const int title = TitleAt(e.position);
        if (title < 0)
        {
            return;
        }
        if (title == open_)
        {
            Close();
            return;
        }
        Open(title);
    }

    /** The titles' rectangles, as laid out at the last paint or press.
        @return  each menu's title's rectangle, in the bar's coordinates */
    const std::vector<gfx::RectI> &titles() const noexcept
    {
        return titles_;
    }

    /** The menu open, if any.
        @return  its index among the bar's menus; -1 for none */
    int open_menu() const noexcept
    {
        return open_;
    }

    /** The open menu's session, for a suite to choose from.
        @return  the session; null when no menu is open */
    view::MenuSession *session() noexcept
    {
        return session_.get();
    }

    /** Menu `index` opened under its title, as a press on the title opens it.
        @param index  the menu's index among the bar's
        @return       true when it opened: a menu enabled, with items, in a host with popups */
    bool Open(int index)
    {
        LayOut();
        Close();
        const std::vector<MenuEntry> &menus = controller_->bar().menus;
        if (index < 0 || static_cast<std::size_t>(index) >= menus.size() ||
            !menus[static_cast<std::size_t>(index)].enabled || menus[static_cast<std::size_t>(index)].submenu.empty())
        {
            return false;
        }
        const gfx::RectI    under = titles_[static_cast<std::size_t>(index)];
        std::weak_ptr<bool> alive = alive_;
        session_ = view::MenuSession::Open(*this, controller_->ItemsOf(menus[static_cast<std::size_t>(index)].submenu),
                                           style_.menu, {under.x(), under.bottom()}, [this, alive](int command) {
                                               if (alive.expired())
                                               {
                                                   return;
                                               }
                                               session_.reset(); // the menu has ended: it may go here
                                               open_ = -1;
                                               Repaint();
                                               if (command != 0)
                                               {
                                                   const std::shared_ptr<MenuBarController> controller =
                                                       controller_; // the bar may go in it
                                                   controller->Choose(command);
                                               }
                                           });
        if (session_ == nullptr)
        {
            return false;
        }
        open_ = index;
        Repaint();
        return true;
    }

    /** The open menu closed, unchosen. */
    void Close()
    {
        if (session_ != nullptr)
        {
            session_.reset();
            open_ = -1;
            Repaint();
        }
    }

  private:
    /** The titles laid out left to right, where the bar has been set anew since the last layout or the bar is a new
        size. */
    void LayOut()
    {
        if (laid_out_revision_ == controller_->revision() && laid_out_width_ == width() && laid_out_)
        {
            return;
        }
        laid_out_          = true;
        laid_out_revision_ = controller_->revision();
        laid_out_width_    = width();
        titles_.clear();
        if (controller_->revision() != shown_revision_)
        {
            shown_revision_ = controller_->revision();
            Close(); // the bar changed under an open menu: its rows are stale
        }
        int x = 0;
        for (const MenuEntry &menu : controller_->bar().menus)
        {
            const float text = host() != nullptr ? host()->TextWidth(style_.font, menu.title)
                                                 : 0.5f * style_.font.height * static_cast<float>(menu.title.size());
            const int   wide = static_cast<int>(std::ceil(text)) + 2 * style_.padding;
            titles_.push_back({x, 0, wide, height()});
            x += wide;
        }
    }

    /** The title under a point.
        @param p  the point, in the bar's coordinates
        @return   the menu's index; -1 for none */
    int TitleAt(gfx::PointF p) const
    {
        for (std::size_t i = 0; i < titles_.size(); ++i)
        {
            if (titles_[i].ToFloat().Contains(p))
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    std::shared_ptr<MenuBarController> controller_;                          ///< the bar shown, and its choosing
    MenuBarStyle                       style_;                               ///< the look
    std::vector<gfx::RectI>            titles_;                              ///< each title's rectangle
    std::unique_ptr<view::MenuSession> session_;                             ///< the open menu
    int                                open_{-1};                            ///< the open menu's index; -1 for none
    bool                               laid_out_{false};                     ///< laid out at least once
    std::size_t                        laid_out_revision_{0};                ///< the controller's revision laid out
    int                                laid_out_width_{0};                   ///< the width laid out at
    std::size_t                        shown_revision_{0};                   ///< the revision the open menu shows
    std::shared_ptr<bool>              alive_{std::make_shared<bool>(true)}; ///< expires with the bar
    int                                watch_{0};                            ///< the controller's token for the bar
};
} // namespace reckless::app
