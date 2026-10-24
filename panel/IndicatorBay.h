#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Canvas.h"
#include "Color.h"
#include "Geometry.h"
#include "LookBallistics.h"
#include "LookDrawing.h"
#include "LookMetrics.h"
#include "LookTubes.h"
#include "View.h"

namespace reckless::panel
{
/**
    The clip indicators in a bay under a panel's header, as a view - the Amp's and the Distortion's: each a tube
    (look::DrawTubeCell - the 12AX7 or the EL34 as geometry in millimetres, the glass clear, standing on the indicators'
    floor tube_height under its cell's top, captioned under it) whose heater glows with how hard its stage is driven, or
    a lens (look::DrawLedCell, the Amp's gate LED) in the colour its panel sets, the cells in a row in the order they
    were added.

    A tube is fed a target once a display frame - its stage's glow, ClipGlow of the stage's per-block peak - and
    follows it through the series' filament (look::LampLight: 20 ms up, 80 ms down), so a frame reads a glow and not a
    flicker, and the bay repaints only that tube's light, and only when its glow moved by more than the threshold. A
    lit tube's halo reaches past its cell - over its neighbour, the bay's frame and the margin above it, a little over
    half a tube's height from the heater at full glow - and is painted so, unclipped, in the panel's own coordinates: so
    the bay is laid over the whole panel it sits in (PlaceOver), its cells in the panel's coordinates, and paints there
    with nothing clipped and nothing moved, and it takes no mouse. Added to the panel before the knobs and the screens,
    it paints over the plate and under them.
*/
class IndicatorBay final : public view::View
{
  public:
    /** Which tube a cell draws: the 12AX7 or the EL34. */
    using Tube = look::TubeGlyph::Kind;

    /** A bay with no indicators yet.
        @param lit          the colour that lights the tubes' heaters and halos (the series' amber)
        @param tube_height  the EL34's height, in design pixels (look::kBayTubeHeight in a bay)
        @param threshold    the change in glow, 0 to 1, worth a repaint */
    explicit IndicatorBay(gfx::Color lit, int tube_height = look::kBayTubeHeight, double threshold = 0.005)
        : lit_(lit), tube_height_(tube_height), threshold_(threshold)
    {
        SetInterceptsMouse(false, false); // the panel's, for the mouse: the indicators are paint, not controls
    }

    /** A tube, after the indicators added before it, captioned `caption`, cold until its first Step; its index.
        @param caption  the text under the tube
        @param tube     which tube to draw
        @return         the indicator's index, for Step, SetIndicatorShown, glow and CellOf */
    std::size_t AddTube(std::string caption, Tube tube)
    {
        cells_.push_back({std::move(caption), tube, look::LampLight(threshold_, 0.0, look::LampFilament{}), {}, {}});
        return cells_.size() - 1;
    }

    /** A lens, after the indicators added before it, captioned `caption`, dark until SetLens lights it; its index.
        @param caption  the text under the lens
        @return         the indicator's index, for SetLens, SetIndicatorShown, glow and CellOf */
    std::size_t AddLens(std::string caption)
    {
        cells_.push_back({std::move(caption), std::nullopt, look::LampLight(threshold_, 0.0, std::nullopt), {}, {}});
        return cells_.size() - 1;
    }

    /** The bay laid over `panel` (the panel's whole bounds) with indicator i in `cells[i]`, in the panel's
        coordinates.
        @param panel  the panel's whole bounds, in design pixels
        @param cells  each indicator's cell, in the panel's coordinates; indicators past its end keep their cell */
    void PlaceOver(gfx::RectI panel, const std::vector<gfx::RectI> &cells)
    {
        SetBounds(panel);
        for (std::size_t i = 0; i < cells_.size() && i < cells.size(); ++i)
        {
            cells_[i].area = cells[i];
        }
        Repaint();
    }

    /** One display frame, `seconds` long: tube `index`'s filament toward `target` (0 to 1), and its light painted
        again when its glow moved by more than the threshold - a hidden tube's filament steps all the same, and paints
        nothing.
        @param index    the tube's index; ignored for a lens or an index past the end
        @param target   the glow the filament moves toward, 0 to 1
        @param seconds  the frame's length, in seconds */
    void Step(std::size_t index, double target, double seconds)
    {
        if (index < cells_.size() && cells_[index].tube && cells_[index].light.Step(target, seconds) &&
            cells_[index].shown)
        {
            Repaint(LightArea(cells_[index].area));
        }
    }

    /** Lens `index` shown in `colour` at `glow` (0 dark, 1 lit), painted again when either changed and it shows.
        @param index   the lens's index; ignored for a tube or an index past the end
        @param colour  the lens's colour
        @param glow    the lens's glow, 0 to 1 */
    void SetLens(std::size_t index, gfx::Color colour, double glow)
    {
        if (index >= cells_.size() || cells_[index].tube)
        {
            return;
        }
        auto      &cell = cells_[index];
        const bool same = cell.lens_colour == colour && !(cell.lens_glow < glow) && !(glow < cell.lens_glow);
        if (!same)
        {
            cell.lens_colour = colour;
            cell.lens_glow   = glow;
            if (cell.shown)
            {
                Repaint(LightArea(cell.area));
            }
        }
    }

    /** Indicator `index` drawn, or left out of the bay - a panel's view with no place for it (the Amp's SIMPLE, which
        has no gate LED) - painting nothing, its tube's filament still following its target so it comes back at the
        glow it would have had. Every indicator shows as it is added.
        @param index  the indicator's index; ignored past the end
        @param shown  true to draw it, false to leave it out */
    void SetIndicatorShown(std::size_t index, bool shown)
    {
        if (index < cells_.size() && cells_[index].shown != shown)
        {
            cells_[index].shown = shown;
            Repaint(LightArea(cells_[index].area));
        }
    }

    /** Whether indicator `index` is drawn.
        @param index  the indicator's index
        @return       true when it shows; false when it is left out or the index is past the end */
    bool indicator_shown(std::size_t index) const noexcept
    {
        return index < cells_.size() && cells_[index].shown;
    }

    /** The glow indicator `index` shows: a tube's filament as last painted, a lens's as set.
        @param index  the indicator's index
        @return       the glow, 0 to 1; 0 for an index past the end */
    double glow(std::size_t index) const noexcept
    {
        if (index >= cells_.size())
        {
            return 0.0;
        }
        return cells_[index].tube ? cells_[index].light.glow() : cells_[index].lens_glow;
    }

    /** How many indicators the bay holds.
        @return  the count of tubes and lenses added */
    std::size_t size() const noexcept
    {
        return cells_.size();
    }

    /** Indicator `index`'s cell, in the panel's coordinates.
        @param index  the indicator's index
        @return       the cell, in design pixels; empty for an index past the end */
    gfx::RectI CellOf(std::size_t index) const noexcept
    {
        return index < cells_.size() ? cells_[index].area : gfx::RectI{};
    }

    /** The cells shown, together: the indicators' area, which the bay's frame goes round.
        @return  the union of the shown cells, in the panel's coordinates; empty when none shows */
    gfx::RectI CellsArea() const noexcept
    {
        gfx::RectI area;
        for (const auto &cell : cells_)
        {
            if (cell.shown)
            {
                area = area.Union(cell.area);
            }
        }
        return area;
    }

    /** Draws every shown indicator in its cell, unclipped, in the panel's coordinates.
        @param c  the canvas, in the panel's coordinates */
    void Paint(gfx::Canvas &c) override
    {
        for (const auto &cell : cells_)
        {
            if (!cell.shown)
            {
                continue;
            }
            if (cell.tube)
            {
                look::DrawTubeCell(c, cell.area, cell.caption, *cell.tube, lit_, cell.light.glow(), tube_height_);
            }
            else
            {
                look::DrawLedCell(c, cell.area, cell.caption, cell.lens_colour, cell.lens_glow, tube_height_);
            }
        }
    }

  private:
    /** One indicator: a tube or a lens, with its light and where it stands. */
    struct Cell
    {
        std::string         caption;        ///< the text under the indicator
        std::optional<Tube> tube;           ///< the tube drawn; empty for a lens
        look::LampLight     light;          ///< a tube's filament and its glow
        gfx::Color          lens_colour;    ///< a lens's colour
        double              lens_glow{0.0}; ///< a lens's glow, 0 to 1
        gfx::RectI          area{};         ///< the cell, in the panel's coordinates
        bool                shown{true};    ///< whether it is drawn
    };

    /** Where a cell's light reaches: the cell and the halo round its tube at full glow, (0.55 + 0.25) of the tube's
        height from the heater, with a pixel to spare.
        @param cell  an indicator's cell, in the panel's coordinates
        @return      the area to repaint when its light changes */
    gfx::RectI LightArea(gfx::RectI cell) const noexcept
    {
        const int reach = static_cast<int>(std::ceil(0.8 * static_cast<double>(tube_height_))) + 1;
        return cell.Expanded(reach, reach);
    }

    gfx::Color        lit_;         ///< the colour of a lit heater and its halo
    int               tube_height_; ///< the EL34's height, in design pixels
    double            threshold_;   ///< the change in glow worth a repaint
    std::vector<Cell> cells_;       ///< the indicators, in the order added
};
} // namespace reckless::panel
