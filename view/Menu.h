#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Canvas.h"
#include "View.h"

namespace reckless::view
{
/** One row of a menu: an item to choose, a rule between groups, or an item that opens a submenu beside itself. */
struct MenuItem
{
    std::string           text;             ///< the item's text
    int                   id{0};            ///< what choosing the item reports; 0 for an item that cannot be chosen
    bool                  enabled{true};    ///< false: drawn dim and not chosen
    bool                  ticked{false};    ///< a tick in the left column
    bool                  separator{false}; ///< a rule between groups, with no text
    std::vector<MenuItem> submenu;          ///< not empty: the item opens these beside itself
    std::string           shortcut;         ///< the key that chooses it from anywhere ("Ctrl+N"), drawn at the right
};

/** How a menu looks: its colours, its font and its rows' sizes, in design pixels. */
struct MenuStyle
{
    gfx::Color background{gfx::Color::FromRgb(40, 40, 40)};          ///< the menu's fill
    gfx::Color border{gfx::Color::FromRgb(90, 90, 90)};              ///< the border and the separators' rule
    gfx::Color text{gfx::Color::FromRgb(230, 230, 230)};             ///< an enabled item's text and tick
    gfx::Color highlight{gfx::Color::FromRgb(90, 90, 90)};           ///< the row under the pointer
    gfx::Color highlighted_text{gfx::Color::FromRgb(255, 255, 255)}; ///< the text of the row under the pointer
    gfx::Color disabled_text{gfx::Color::FromRgb(120, 120, 120)};    ///< a disabled item's text
    gfx::Font  font{15.0f};                                          ///< the items' font
    int        row_height{22};                                       ///< an item's row, in design pixels
    int        separator_height{9};                                  ///< a separator's row, in design pixels
    int        tick_column{22};                                      ///< the left column a tick sits in
    int        arrow_column{18};                                     ///< the right column a submenu's arrow sits in
    int        padding{8};                                           ///< the text's margin on the right
    int        border_width{1};                                      ///< the border's width, in design pixels
    int        min_width{120};                                       ///< the menu's width at least, in design pixels
    int        shortcut_gap{24}; ///< the room between an item's text and its shortcut, where a menu shows any
};

class MenuSession;

/**
    A menu's items as a view, the content of a popup: rows top to bottom, a tick beside a ticked item, an arrow
    beside one with a submenu, the row under the pointer lit. A release on an enabled item with an id chooses it; the
    pointer resting on an item with a submenu opens that beside the row, and on another row closes it. What the
    choice does is the session's (MenuSession), which owns the menu.
*/
class Menu final : public View
{
  public:
    // Both out of line, where MenuSession is whole: a constructor's cleanup destroys child_ as surely as the
    // destructor does, and C++23 instantiates it where the constructor is defined.
    /** @param items    the rows, top to bottom
        @param style    the look
        @param session  the session that owns the menu and is told a choice; outlives the menu */
    Menu(std::vector<MenuItem> items, MenuStyle style, MenuSession &session);

    /** Closes any open submenu. */
    ~Menu() override;

    /** The menu sized to its items, their text measured as `host`'s window draws it.
        @param host  the host whose window shows the menu */
    void SizeTo(const Host &host)
    {
        float widest          = 0.0f;
        float widest_shortcut = 0.0f;
        bool  any_submenu     = false;
        int   height          = 2 * style_.border_width;
        for (const MenuItem &item : items_)
        {
            height += item.separator ? style_.separator_height : style_.row_height;
            if (!item.separator)
            {
                widest = std::max(widest, host.TextWidth(style_.font, item.text));
                if (!item.shortcut.empty())
                {
                    widest_shortcut = std::max(widest_shortcut, host.TextWidth(style_.font, item.shortcut));
                }
            }
            any_submenu = any_submenu || !item.submenu.empty();
        }
        // A column for the shortcuts only where an item has one, so a menu with none is the width it always was.
        shortcut_column_ =
            widest_shortcut > 0.0f ? style_.shortcut_gap + static_cast<int>(std::ceil(widest_shortcut)) : 0;
        const int width = style_.tick_column + static_cast<int>(std::ceil(widest)) + shortcut_column_ + style_.padding +
                          (any_submenu ? style_.arrow_column : 0) + 2 * style_.border_width;
        SetBounds({std::max(width, style_.min_width), height});
    }

    /** Draws the border, the rows, the ticks and the submenus' arrows.
        @param c  the canvas, in the menu's local coordinates */
    void Paint(gfx::Canvas &c) override
    {
        const gfx::RectI all = LocalBounds();
        c.SetColor(style_.background);
        c.FillRect(all);
        c.SetColor(style_.border);
        c.DrawRect(all.ToFloat(), static_cast<float>(style_.border_width));
        c.SetFont(style_.font);
        for (std::size_t i = 0; i < items_.size(); ++i)
        {
            const MenuItem  &item = items_[i];
            const gfx::RectI row  = RowBounds(i);
            if (item.separator)
            {
                c.SetColor(style_.border);
                c.DrawHorizontalLine(row.y() + row.height() / 2, static_cast<float>(row.x() + style_.padding),
                                     static_cast<float>(row.right() - style_.padding));
                continue;
            }
            const bool lit = static_cast<int>(i) == hover_ && item.enabled;
            if (lit)
            {
                c.SetColor(style_.highlight);
                c.FillRect(row);
            }
            c.SetColor(!item.enabled ? style_.disabled_text : lit ? style_.highlighted_text : style_.text);
            if (item.ticked)
            {
                // A tick: two strokes in the left column.
                const gfx::RectF box =
                    gfx::RectF{static_cast<float>(row.x()), static_cast<float>(row.y()),
                               static_cast<float>(style_.tick_column), static_cast<float>(row.height())}
                        .Reduced(6.0f, 6.0f);
                gfx::Path tick;
                tick.MoveTo({box.x(), box.y() + box.height() * 0.55f});
                tick.LineTo({box.x() + box.width() * 0.4f, box.bottom()});
                tick.LineTo({box.right(), box.y()});
                c.StrokePath(tick, gfx::Stroke(1.8f));
            }
            const gfx::RectF text = gfx::RectF::LeftTopRightBottom(
                static_cast<float>(row.x() + style_.tick_column), static_cast<float>(row.y()),
                static_cast<float>(row.right() - style_.padding - (item.submenu.empty() ? 0 : style_.arrow_column)),
                static_cast<float>(row.bottom()));
            c.DrawText(item.text, text, gfx::Justify::kCentredLeft);
            if (!item.shortcut.empty())
            {
                // Right-aligned in its column, left of where a submenu's arrow would be.
                c.DrawText(item.shortcut, text, gfx::Justify::kCentredRight);
            }
            if (!item.submenu.empty())
            {
                // An arrow: a small triangle pointing right, in the right column.
                const float cx = static_cast<float>(row.right() - style_.padding - style_.arrow_column / 2);
                const float cy = static_cast<float>(row.y()) + static_cast<float>(row.height()) * 0.5f;
                gfx::Path   arrow;
                arrow.AddTriangle({cx - 3.0f, cy - 4.0f}, {cx - 3.0f, cy + 4.0f}, {cx + 3.0f, cy});
                c.FillPath(arrow);
            }
        }
    }

    /** Lights the row under the pointer, opening its submenu if it has one.
        @param e  the move: its position */
    void MouseMove(const MouseEvent &e) override
    {
        SetHover(RowAt(e.position));
    }

    /** As MouseMove.
        @param e  the move: its position */
    void MouseEnter(const MouseEvent &e) override
    {
        SetHover(RowAt(e.position));
    }

    /** Puts the lit row out, unless its submenu is open. */
    void MouseExit(const MouseEvent &) override
    {
        if (child_ == nullptr)
        {
            SetHover(-1); // a row with its submenu open stays lit while the pointer is over the submenu
        }
    }

    /** A release on an enabled item with an id and no submenu chooses it.
        @param e  the release: its position */
    void MouseUp(const MouseEvent &e) override;

    /** @return  the lit row's index, -1 for none */
    int hovered_row() const noexcept
    {
        return hover_;
    }

    /** Whether `screen` lies in the submenu open beside this menu, or one of its own.
        @param screen  a point, in screen coordinates
        @return        true when it lies in an open submenu's window */
    bool ChildContains(gfx::PointI screen) const;

    /** @return  the rows, top to bottom */
    const std::vector<MenuItem> &items() const noexcept
    {
        return items_;
    }

    /** @param index  the row, from 0
        @return       its bounds in the menu's local coordinates; an index past the last gives an item row's height
                      below the last row */
    gfx::RectI RowBounds(std::size_t index) const
    {
        int y = style_.border_width;
        for (std::size_t i = 0; i < index && i < items_.size(); ++i)
        {
            y += items_[i].separator ? style_.separator_height : style_.row_height;
        }
        const int h = index < items_.size() && items_[index].separator ? style_.separator_height : style_.row_height;
        return {style_.border_width, y, width() - 2 * style_.border_width, h};
    }

  private:
    /** @param p  a point, in the menu's local coordinates
        @return   the index of the item row it is in; -1 for a separator or no row */
    int RowAt(gfx::PointF p) const
    {
        for (std::size_t i = 0; i < items_.size(); ++i)
        {
            if (RowBounds(i).ToFloat().Contains(p))
            {
                return items_[i].separator ? -1 : static_cast<int>(i);
            }
        }
        return -1;
    }

    /** Light a row, closing the submenu of the row left and opening the new row's, when it has one.
        @param row  the row's index; -1 for none */
    void SetHover(int row);

    std::vector<MenuItem> items_;     ///< the rows
    MenuStyle             style_;     ///< the look
    MenuSession          &session_;   ///< the session that owns the menu
    int                   hover_{-1}; ///< the lit row's index; -1 for none
    int shortcut_column_{0};          ///< the shortcuts' column with the gap before it, in design pixels; 0 with none
    std::unique_ptr<MenuSession> child_; ///< the submenu open beside the hovered row
};

/**
    A menu open in a popup over a view's window until an item is chosen or a press lands outside it. `chosen` runs
    once - with the item's id, or 0 when the menu was dismissed - on the window's thread after the event that ended
    the menu (Host::CallSoon), so the menu is never destroyed while it is still handling that event, and not at all
    once the session is gone. If the window the menu hangs from goes first, the menu ends dismissed there and then
    (Popup::closed_by_window): `chosen` runs at once with 0 unless it has run already, a choice still on its way
    going with the window's other deferred calls. Its owner destroys the session there, which closes the popup. A
    submenu is a session of its own whose choice is its parent's.
*/
class MenuSession
{
  public:
    /** `items` in a popup with its top left at `at`, in `anchor`'s coordinates; null when `anchor` is in no host's
        tree or the host has no popups.
        @param anchor  the view the popup hangs from; its host opens the popup
        @param items   the menu's rows
        @param style   the menu's look
        @param at      the popup's top left, in `anchor`'s coordinates
        @param chosen  runs once with the chosen item's id, or 0 when dismissed
        @param parent  the session this one is a submenu of; nullptr for a menu of its own
        @return        the session, owned by the caller; nullptr when no popup could be opened */
    static std::unique_ptr<MenuSession> Open(View &anchor, std::vector<MenuItem> items, const MenuStyle &style,
                                             gfx::PointI at, std::function<void(int)> chosen,
                                             const MenuSession *parent = nullptr)
    {
        Host *host = anchor.host();
        if (host == nullptr)
        {
            return nullptr;
        }
        auto session   = std::unique_ptr<MenuSession>(new MenuSession(*host, std::move(chosen)));
        session->menu_ = std::make_unique<Menu>(std::move(items), style, *session);
        session->menu_->SizeTo(*host);
        session->style_ = style;
        // A press outside every window of the chain - this menu's and its open submenus' - dismisses it; a submenu
        // leaves that to the menu it belongs to, which sees the same press.
        std::weak_ptr<bool> alive = session->alive_;
        MenuSession        *self  = session.get();
        const bool          root  = parent == nullptr;
        session->popup_           = anchor.OpenPopup(*session->menu_, at, [alive, self, root](gfx::PointI screen) {
            if (root && !alive.expired() && !self->ChainContains(screen))
            {
                self->Finish(0);
            }
        });
        if (session->popup_ == nullptr)
        {
            return nullptr;
        }
        // The window going first - a host closing the editor - leaves no event of the menu's own to finish, so the
        // menu ends there and then; a submenu leaves that to the menu it belongs to, whose window goes with it.
        session->popup_->closed_by_window = [alive, self, root] {
            if (root && !alive.expired())
            {
                self->End();
            }
        };
        return session;
    }

    /** Closes the popup, then the menu. */
    ~MenuSession()
    {
        popup_.reset(); // the window first, while the menu it shows still lives
        menu_.reset();
    }

    /** Not copyable: the popup and the menu refer to the session. */
    MenuSession(const MenuSession &) = delete;

    /** Not assignable: the popup and the menu refer to the session.
        @return  never */
    MenuSession &operator=(const MenuSession &) = delete;

    /** The menu ends with `id` (0: dismissed): `chosen` runs once, soon, unless the session is gone by then.
        @param id  the chosen item's id; 0 for dismissed */
    void Finish(int id)
    {
        if (finished_)
        {
            return;
        }
        finished_                 = true;
        std::weak_ptr<bool> alive = alive_;
        MenuSession        *self  = this;
        host_.CallSoon([alive, self, id] {
            if (!alive.expired())
            {
                self->Deliver(id);
            }
        });
    }

    /** The menu ends dismissed at once - the window it hangs from going, with no event of the menu's own on the
        stack: `chosen` runs now with 0, unless it has run already. */
    void End()
    {
        finished_ = true;
        Deliver(0);
    }

    /** @return  the menu the session owns */
    Menu &menu() noexcept
    {
        return *menu_;
    }

    /** Whether `screen` lies in this menu's window or an open submenu's.
        @param screen  a point, in screen coordinates
        @return        true when it lies in the menu's window or an open submenu's */
    bool ChainContains(gfx::PointI screen) const
    {
        return (popup_ != nullptr && popup_->Contains(screen)) || menu_->ChildContains(screen);
    }

    /** @return  the menu's look */
    const MenuStyle &style() const noexcept
    {
        return style_;
    }

  private:
    /** @param host    the host whose window shows the menu; outlives the session
        @param chosen  runs once with the chosen item's id, or 0 when dismissed */
    MenuSession(Host &host, std::function<void(int)> chosen) : host_(host), chosen_(std::move(chosen))
    {
    }

    /** `chosen` with `id`, once, whichever way the menu ended; its owner may destroy the session in it.
        @param id  the chosen item's id; 0 for dismissed */
    void Deliver(int id)
    {
        if (delivered_)
        {
            return;
        }
        delivered_                            = true;
        const std::function<void(int)> chosen = chosen_;
        if (chosen)
        {
            chosen(id);
        }
    }

    Host                    &host_;                                ///< the host that runs deferred calls
    std::function<void(int)> chosen_;                              ///< told the choice
    MenuStyle                style_;                               ///< the look, handed on to submenus
    std::shared_ptr<bool>    alive_{std::make_shared<bool>(true)}; ///< expires with the session
    bool                     finished_{false};                     ///< the menu has ended; later ends are dropped
    bool                     delivered_{false};                    ///< `chosen` has run
    std::unique_ptr<Menu>    menu_;                                ///< the menu shown
    std::unique_ptr<Popup>   popup_;                               ///< the popup window that shows it
};

inline Menu::Menu(std::vector<MenuItem> items, MenuStyle style, MenuSession &session)
    : items_(std::move(items)), style_(std::move(style)), session_(session)
{
}

inline Menu::~Menu() = default;

inline bool Menu::ChildContains(gfx::PointI screen) const
{
    return child_ != nullptr && child_->ChainContains(screen);
}

inline void Menu::MouseUp(const MouseEvent &e)
{
    const int row = RowAt(e.position);
    if (row < 0)
    {
        return;
    }
    const MenuItem &item = items_[static_cast<std::size_t>(row)];
    if (item.enabled && item.id != 0 && item.submenu.empty())
    {
        session_.Finish(item.id);
    }
}

inline void Menu::SetHover(int row)
{
    if (row == hover_)
    {
        return;
    }
    hover_ = row;
    Repaint();
    child_.reset(); // the submenu of the row the pointer left
    if (row >= 0)
    {
        const MenuItem &item = items_[static_cast<std::size_t>(row)];
        if (item.enabled && !item.submenu.empty())
        {
            const gfx::RectI bounds = RowBounds(static_cast<std::size_t>(row));
            MenuSession     &parent = session_;
            child_                  = MenuSession::Open(
                *this, item.submenu, session_.style(), {bounds.right(), bounds.y()},
                [&parent](int id) {
                    if (id != 0)
                    {
                        parent.Finish(id);
                    }
                },
                &parent);
        }
    }
}
} // namespace reckless::view
