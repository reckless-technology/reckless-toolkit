#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "Geometry.h"

namespace reckless::view
{
/**
    The knob bank's arithmetic: where a knob's caption, its dial and its frame fall in a cell, where a plate's caption
    and plate fall, a row's or a grid's cells, and a plate's cell in the switch strip under a column. Every rectangle
    is the one the knob bank of the releases built on JUCE gave, pixel for pixel - gfx's rectangle arithmetic is
    JUCE's - so a panel lays out where it did.
    The series' sizes come in as a KnobMetrics, the panel's own: every series panel's are the look's (panel/'s
    kKnobMetrics, on reckless::look's numbers), which live there alone.
*/
struct KnobMetrics
{
    int label_height{0};   ///< a caption's row, in design pixels
    int knob_block{0};     ///< the dial with its value box under it, in design pixels
    int knob_width{0};     ///< the dial's width at most, the series' slider, in design pixels
    int plate_height{0};   ///< a plate's height, in design pixels
    int frame_inset{0};    ///< the frame in from the cell's sides, in design pixels
    int frame_overhang{0}; ///< and out past the group's top and bottom, in design pixels
};

/** Where one knob falls in its cell: its caption over it, the dial (with its value box) centred under, and the
    frame round both. */
struct KnobPlace
{
    gfx::RectI caption; ///< the caption's row, over the dial
    gfx::RectI dial;    ///< the dial with its value box, centred under the caption
    gfx::RectF frame;   ///< the cream frame round both, on the half pixel
};

/** Where one plate falls in its cell: its caption over it and the plate centred under. */
struct PlatePlace
{
    gfx::RectI caption; ///< the caption's row, over the plate
    gfx::RectI plate;   ///< the plate, centred under the caption
};

/** The cream frame round a knob's group: in from its sides, out past its top and bottom, on the half pixel.
    @param group  the caption and dial together, in design pixels
    @param m      the panel's metrics
    @return       the frame, in design pixels */
inline gfx::RectF FrameAround(gfx::RectI group, const KnobMetrics &m) noexcept
{
    return group.Reduced(m.frame_inset, 0).Expanded(0, m.frame_overhang).ToFloat().Reduced(0.6f);
}

/** A knob in `cell`: the group of caption and dial centred in it, the frame round the group.
    @param cell  the knob's cell, in design pixels
    @param m     the panel's metrics
    @return      where the caption, the dial and the frame fall */
inline KnobPlace PlaceKnob(gfx::RectI cell, const KnobMetrics &m) noexcept
{
    gfx::RectI group = cell.WithSizeKeepingCentre(cell.width(), m.label_height + m.knob_block);
    KnobPlace  place;
    place.frame   = FrameAround(group, m);
    place.caption = group.RemoveFromTop(m.label_height);
    place.dial    = group.WithSizeKeepingCentre(std::min(cell.width() - 8, m.knob_width), m.knob_block);
    return place;
}

/** A plate in `cell`, `plate_width` wide: the caption and plate pair centred in it.
    @param cell         the plate's cell, in design pixels
    @param plate_width  the plate's width, in design pixels
    @param m            the panel's metrics
    @return             where the caption and the plate fall */
inline PlatePlace PlacePlate(gfx::RectI cell, int plate_width, const KnobMetrics &m) noexcept
{
    gfx::RectI pair = cell.WithSizeKeepingCentre(cell.width(), m.label_height + m.plate_height);
    PlatePlace place;
    place.caption = pair.RemoveFromTop(m.label_height);
    place.plate   = pair.WithSizeKeepingCentre(plate_width, m.plate_height);
    return place;
}

/** `count` cells across `area`, one equal column each of `columns` (the remainder left at the right).
    @param area     the row, in design pixels
    @param columns  how many equal columns the row's width is divided into; at least 1 is used
    @param count    how many cells to return, from the left
    @return         the cells, each the full height of `area` */
inline std::vector<gfx::RectI> RowCells(gfx::RectI area, int columns, std::size_t count)
{
    const int               width = area.width() / std::max(columns, 1);
    std::vector<gfx::RectI> cells;
    for (std::size_t i = 0; i < count; ++i)
    {
        cells.push_back({area.x() + static_cast<int>(i) * width, area.y(), width, area.height()});
    }
    return cells;
}

/** `count` cells in a grid of `columns` across `area`, row by row, the rows as many as the count needs.
    @param area     the grid, in design pixels, shared equally among the rows and columns
    @param columns  how many cells across; at least 1 is used
    @param count    how many cells to return, in reading order
    @return         the cells */
inline std::vector<gfx::RectI> GridCells(gfx::RectI area, int columns, std::size_t count)
{
    const int               cols   = std::max(columns, 1);
    const int               n      = static_cast<int>(count);
    const int               rows   = (n + cols - 1) / cols;
    const int               width  = area.width() / cols;
    const int               height = area.height() / std::max(rows, 1);
    std::vector<gfx::RectI> cells;
    for (int i = 0; i < n; ++i)
    {
        cells.push_back({area.x() + (i % cols) * width, area.y() + (i / cols) * height, width, height});
    }
    return cells;
}

/** A plate's cell in the switch `strip` under knob column `column` (a fraction for a plate between columns) of
    `cell_width`-wide columns starting at `area`'s left; the plate `cell_width - trim` wide.
    @param area        the knob area the columns start at, in design pixels
    @param strip       the switch strip, in design pixels; its y and height are the cell's
    @param column      the column the plate is under, counted from 0; a fraction for a plate between columns
    @param cell_width  one column's width, in design pixels
    @param trim        how much narrower than a column the plate is, in design pixels
    @param m           the panel's metrics
    @return            where the caption and the plate fall */
inline PlatePlace PlacePlateInStrip(gfx::RectI area, gfx::RectI strip, double column, int cell_width, int trim,
                                    const KnobMetrics &m) noexcept
{
    const int centre_x = area.x() + static_cast<int>(column * cell_width + 0.5);
    return PlacePlate({centre_x - cell_width / 2, strip.y(), cell_width, strip.height()}, cell_width - trim, m);
}
} // namespace reckless::view
