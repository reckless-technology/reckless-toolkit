#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "Geometry.h"

namespace reckless::gfx
{
/**
    An outline to fill or stroke, written against no toolkit: a list of drawing operations - sub-paths of lines and
    curves, and whole shapes (a rectangle, a rounded rectangle, an ellipse, an arc, a triangle) - and transforms that
    apply to everything before them. A backend replays the list on its own path type, each shape through its own
    construction of it, so a shape is that toolkit's own; the path itself measures its bounds and answers hit tests
    from a flattening of its own.
*/
class Path
{
  public:
    /** What one element of the list does; the comments say which of an element's values it reads. */
    enum class Op
    {
        kMoveTo,      ///< v0, v1: the point
        kLineTo,      ///< v0, v1
        kQuadTo,      ///< v0..v3: the control point, the end
        kCubicTo,     ///< v0..v5: two control points, the end
        kClose,       ///< no values
        kRect,        ///< v0..v3: x, y, width, height
        kRoundedRect, ///< v0..v4: x, y, width, height, the corner radius
        kEllipse,     ///< v0..v3: the bounding rectangle
        kCentredArc,  ///< v0..v6: centre x, y, radii x, y, rotation, from and to angles (radians clockwise from
                      ///< twelve o'clock); `flag` a new sub-path at its start
        kTriangle,    ///< v0..v5: three corners
        kTransform,   ///< v0..v5: m00, m01, m02, m10, m11, m12, applied to everything before it
    };

    /** Which points a filled outline covers where its sub-paths cross or nest: by the non-zero winding rule, every
        path's until one asks otherwise, or by the even-odd rule - a ring cut out of a disc, an SVG drawing's
        fill-rule="evenodd". */
    enum class FillRule
    {
        kNonZero, ///< inside where the outline winds round the point any number of times but zero
        kEvenOdd, ///< inside where a ray from the point crosses the outline an odd number of times
    };

    /** One drawing operation of the list. */
    struct Element
    {
        Op                   op;          ///< what it does
        std::array<float, 7> v{};         ///< its values, as the operation names them; the rest zero
        bool                 flag{false}; ///< a centred arc's start-a-new-sub-path flag; false otherwise
    };

    /** Begin a new sub-path.
        @param x  the point's x
        @param y  the point's y */
    void MoveTo(float x, float y)
    {
        Add(Op::kMoveTo, {x, y});
    }

    /** Begin a new sub-path.
        @param p  the point */
    void MoveTo(PointF p)
    {
        MoveTo(p.x, p.y);
    }

    /** A line from the last point.
        @param x  the end's x
        @param y  the end's y */
    void LineTo(float x, float y)
    {
        Add(Op::kLineTo, {x, y});
    }

    /** A line from the last point.
        @param p  the end */
    void LineTo(PointF p)
    {
        LineTo(p.x, p.y);
    }

    /** A quadratic curve from the last point.
        @param cx  the control point's x
        @param cy  the control point's y
        @param x   the end's x
        @param y   the end's y */
    void QuadTo(float cx, float cy, float x, float y)
    {
        Add(Op::kQuadTo, {cx, cy, x, y});
    }

    /** A cubic curve from the last point.
        @param c1x  the first control point's x
        @param c1y  the first control point's y
        @param c2x  the second control point's x
        @param c2y  the second control point's y
        @param x    the end's x
        @param y    the end's y */
    void CubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
    {
        Add(Op::kCubicTo, {c1x, c1y, c2x, c2y, x, y});
    }

    /** A cubic curve from the last point.
        @param c1   the first control point
        @param c2   the second control point
        @param end  the end */
    void CubicTo(PointF c1, PointF c2, PointF end)
    {
        CubicTo(c1.x, c1.y, c2.x, c2.y, end.x, end.y);
    }

    /** Close the sub-path: a line back to its start. */
    void Close()
    {
        Add(Op::kClose, {});
    }

    /** A rectangle, a whole shape.
        @param r  the rectangle */
    void AddRect(RectF r)
    {
        Add(Op::kRect, {r.x(), r.y(), r.width(), r.height()});
    }

    /** A rounded rectangle, a whole shape.
        @param r              the rectangle
        @param corner_radius  the radius of each corner; held to half the shorter side */
    void AddRoundedRect(RectF r, float corner_radius)
    {
        Add(Op::kRoundedRect, {r.x(), r.y(), r.width(), r.height(), corner_radius});
    }

    /** The ellipse inscribed in a rectangle, a whole shape.
        @param r  the bounding rectangle */
    void AddEllipse(RectF r)
    {
        Add(Op::kEllipse, {r.x(), r.y(), r.width(), r.height()});
    }

    /** An arc of the ellipse centred on (cx, cy) with radii rx, ry, turned by `rotation`, from `from` to `to`
        radians clockwise from twelve o'clock; a new sub-path at its start when `start_new_sub_path`, else joined to
        the last point by a line.
        @param cx                the centre's x
        @param cy                the centre's y
        @param rx                the horizontal radius
        @param ry                the vertical radius
        @param rotation          the ellipse's turn, in radians
        @param from              the start angle, in radians clockwise from twelve o'clock
        @param to                the end angle, in radians clockwise from twelve o'clock
        @param start_new_sub_path  true to begin a new sub-path at the start, false to join it to the last point */
    void AddCentredArc(float cx, float cy, float rx, float ry, float rotation, float from, float to,
                       bool start_new_sub_path)
    {
        Element e{Op::kCentredArc, {cx, cy, rx, ry, rotation, from, to}, start_new_sub_path};
        elements_.push_back(e);
    }

    /** A triangle, a whole shape.
        @param a  the first corner
        @param b  the second corner
        @param c  the third corner */
    void AddTriangle(PointF a, PointF b, PointF c)
    {
        Add(Op::kTriangle, {a.x, a.y, b.x, b.y, c.x, c.y});
    }

    /** Everything added so far moved by `t`.
        @param t  the transform */
    void ApplyTransform(const Affine &t)
    {
        Add(Op::kTransform, {t.m00, t.m01, t.m02, t.m10, t.m11, t.m12});
    }

    /** How a fill of the outline covers the points its sub-paths cross or nest round.
        @param rule  the rule; kNonZero unless set */
    void SetFillRule(FillRule rule) noexcept
    {
        fill_rule_ = rule;
    }

    /** @return the rule a fill of the outline covers its points by; kNonZero unless set */
    FillRule fill_rule() const noexcept
    {
        return fill_rule_;
    }

    /** Remove every element. */
    void Clear() noexcept
    {
        elements_.clear();
    }

    /** No line, curve or shape: nothing to fill or stroke.
        @return true when there is nothing to fill or stroke */
    bool IsEmpty() const noexcept
    {
        return std::none_of(elements_.begin(), elements_.end(), [](const Element &e) {
            return e.op != Op::kMoveTo && e.op != Op::kTransform && e.op != Op::kClose;
        });
    }

    /** @return the operations in the order they were added, valid until the next change */
    const std::vector<Element> &elements() const noexcept
    {
        return elements_;
    }

    /** One flattened sub-path or whole shape. */
    struct Polygon
    {
        std::vector<PointF> points;        ///< the vertices in order, in final coordinates
        bool                closed{false}; ///< true when the last vertex joins the first
    };

    /** The outline as polygons, curves and shapes flattened to within `tolerance`, each polygon closed or not as
        drawn, in final coordinates.
        @param tolerance  the most a curve may stray from its flattening, in the path's units; a quadratic or
                          cubic reads less than 0.01 as 0.01
        @return           the polygons, in order, every transform applied */
    std::vector<Polygon> Flatten(float tolerance = 0.25f) const
    {
        std::vector<Polygon> out;
        PointF               start{};
        PointF               last{};
        auto                 current = [&]() -> Polygon                 &{
            if (out.empty() || out.back().closed)
            {
                out.push_back({{last}, false});
            }
            return out.back();
        };
        auto line_to = [&](PointF p) {
            current().points.push_back(p);
            last = p;
        };
        auto new_sub_path = [&](PointF p) {
            out.push_back({{p}, false});
            start = last = p;
        };
        auto closed_shape = [&](std::vector<PointF> points) { out.push_back({std::move(points), true}); };

        for (const auto &e : elements_)
        {
            const auto &v = e.v;
            switch (e.op)
            {
            case Op::kMoveTo:
                new_sub_path({v[0], v[1]});
                break;
            case Op::kLineTo:
                line_to({v[0], v[1]});
                break;
            case Op::kQuadTo: {
                const PointF p0 = last, c{v[0], v[1]}, p1{v[2], v[3]};
                const int    n  = Segments(p0.DistanceTo(c) + c.DistanceTo(p1), tolerance);
                for (int i = 1; i <= n; ++i)
                {
                    const float t = static_cast<float>(i) / static_cast<float>(n), u = 1.0f - t;
                    line_to(
                        {u * u * p0.x + 2 * u * t * c.x + t * t * p1.x, u * u * p0.y + 2 * u * t * c.y + t * t * p1.y});
                }
                break;
            }
            case Op::kCubicTo: {
                const PointF p0 = last, c1{v[0], v[1]}, c2{v[2], v[3]}, p1{v[4], v[5]};
                const int    n  = Segments(p0.DistanceTo(c1) + c1.DistanceTo(c2) + c2.DistanceTo(p1), tolerance);
                for (int i = 1; i <= n; ++i)
                {
                    const float t = static_cast<float>(i) / static_cast<float>(n), u = 1.0f - t;
                    const float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
                    line_to({a * p0.x + b * c1.x + c * c2.x + d * p1.x, a * p0.y + b * c1.y + c * c2.y + d * p1.y});
                }
                break;
            }
            case Op::kClose:
                if (!out.empty() && !out.back().closed)
                {
                    out.back().closed = true;
                    last              = start;
                }
                break;
            case Op::kRect:
                closed_shape({{v[0], v[1]}, {v[0] + v[2], v[1]}, {v[0] + v[2], v[1] + v[3]}, {v[0], v[1] + v[3]}});
                break;
            case Op::kRoundedRect:
                closed_shape(RoundedRectPoints(RectF{v[0], v[1], v[2], v[3]}, v[4], tolerance));
                break;
            case Op::kEllipse:
                closed_shape(ArcPoints(v[0] + v[2] * 0.5f, v[1] + v[3] * 0.5f, v[2] * 0.5f, v[3] * 0.5f, 0.0f, 0.0f,
                                       2.0f * kPi, tolerance));
                break;
            case Op::kCentredArc: {
                const auto points = ArcPoints(v[0], v[1], v[2], v[3], v[4], v[5], v[6], tolerance);
                if (e.flag || out.empty() || out.back().closed)
                {
                    new_sub_path(points.front());
                }
                else
                {
                    line_to(points.front());
                }
                for (std::size_t i = 1; i < points.size(); ++i)
                {
                    line_to(points[i]);
                }
                break;
            }
            case Op::kTriangle:
                closed_shape({{v[0], v[1]}, {v[2], v[3]}, {v[4], v[5]}});
                break;
            case Op::kTransform: {
                const Affine t{v[0], v[1], v[2], v[3], v[4], v[5]};
                for (auto &polygon : out)
                {
                    for (auto &p : polygon.points)
                    {
                        p = t.Apply(p);
                    }
                }
                start = t.Apply(start);
                last  = t.Apply(last);
                break;
            }
            }
        }
        return out;
    }

    /** The smallest rectangle holding the flattened outline.
        @return the bounds; an empty rectangle at the origin for an outline with no points */
    RectF Bounds() const
    {
        bool  any = false;
        float l = 0, t = 0, r = 0, b = 0;
        for (const auto &polygon : Flatten())
        {
            for (const auto p : polygon.points)
            {
                l   = any ? std::min(l, p.x) : p.x;
                t   = any ? std::min(t, p.y) : p.y;
                r   = any ? std::max(r, p.x) : p.x;
                b   = any ? std::max(b, p.y) : p.y;
                any = true;
            }
        }
        return RectF::LeftTopRightBottom(l, t, r, b);
    }

    /** Whether `point` is inside the filled outline, every sub-path closed, by the path's fill rule: the non-zero
        winding rule unless SetFillRule() asked for the even-odd one.
        @param point  the point to test
        @return       true when the winding number is not zero - or, by the even-odd rule, is odd */
    bool Contains(PointF point) const
    {
        int winding = 0;
        for (const auto &polygon : Flatten())
        {
            const auto &pts = polygon.points;
            for (std::size_t i = 0; i < pts.size(); ++i)
            {
                const PointF a = pts[i];
                const PointF b = pts[(i + 1) % pts.size()];
                if (a.y <= point.y)
                {
                    if (b.y > point.y && Cross(a, b, point) > 0.0f)
                    {
                        ++winding;
                    }
                }
                else if (b.y <= point.y && Cross(a, b, point) < 0.0f)
                {
                    --winding;
                }
            }
        }
        return fill_rule_ == FillRule::kEvenOdd ? (winding % 2) != 0 : winding != 0;
    }

  private:
    static constexpr float kPi = 3.14159265358979323846f; ///< pi, in float

    /** Append an element.
        @param op      what it does
        @param values  its values, in the order the operation names; the rest of the seven are zero */
    void Add(Op op, std::initializer_list<float> values)
    {
        Element e{op, {}, false};
        std::copy(values.begin(), values.end(), e.v.begin());
        elements_.push_back(e);
    }

    /** The line segments a curve is flattened to: more for a longer curve and a finer tolerance.
        @param length     the length of the curve's control polygon
        @param tolerance  the flattening tolerance; below 0.01 is read as 0.01
        @return           the count, 1 to 256 */
    static int Segments(float length, float tolerance)
    {
        return std::clamp(static_cast<int>(std::ceil(std::sqrt(length / std::max(tolerance, 0.01f)))), 1, 256);
    }

    /** The cross product of a to b with a to p: the sign says which side of the line a point is on.
        @param a  the line's start
        @param b  the line's end
        @param p  the point
        @return   the z of (b - a) x (p - a): zero on the line, the two signs the line's two sides */
    static float Cross(PointF a, PointF b, PointF p)
    {
        return (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y);
    }

    /** Points along an arc of an ellipse turned by `rotation`, angles clockwise from twelve o'clock.
        @param cx         the centre's x
        @param cy         the centre's y
        @param rx         the horizontal radius
        @param ry         the vertical radius
        @param rotation   the ellipse's turn, in radians
        @param from       the start angle, in radians clockwise from twelve o'clock
        @param to         the end angle, in radians clockwise from twelve o'clock
        @param tolerance  the flattening tolerance
        @return           the points from `from` to `to`, 5 to 513 of them */
    static std::vector<PointF> ArcPoints(float cx, float cy, float rx, float ry, float rotation, float from, float to,
                                         float tolerance)
    {
        const float span = std::abs(to - from);
        const int   n = std::clamp(static_cast<int>(std::ceil(span * std::sqrt(std::max(rx, ry) / tolerance))), 4, 512);
        const float c = std::cos(rotation), s = std::sin(rotation);
        std::vector<PointF> points;
        points.reserve(static_cast<std::size_t>(n) + 1);
        for (int i = 0; i <= n; ++i)
        {
            const float angle = from + (to - from) * static_cast<float>(i) / static_cast<float>(n);
            const float x = rx * std::sin(angle), y = -ry * std::cos(angle);
            points.push_back({cx + c * x - s * y, cy + s * x + c * y});
        }
        return points;
    }

    /** A rounded rectangle's outline, the corners flattened.
        @param r          the rectangle
        @param radius     the corner radius, held to half the shorter side; zero or less is a plain rectangle
        @param tolerance  the flattening tolerance
        @return           the points, clockwise from the right end of the top edge */
    static std::vector<PointF> RoundedRectPoints(RectF r, float radius, float tolerance)
    {
        radius = std::min({radius, r.width() * 0.5f, r.height() * 0.5f});
        if (!(radius > 0.0f))
        {
            return {r.TopLeft(), {r.right(), r.y()}, r.BottomRight(), {r.x(), r.bottom()}};
        }
        std::vector<PointF> points;
        const auto          corner = [&](float cx, float cy, float from) {
            const auto arc = ArcPoints(cx, cy, radius, radius, 0.0f, from, from + kPi * 0.5f, tolerance);
            points.insert(points.end(), arc.begin(), arc.end());
        };
        corner(r.right() - radius, r.y() + radius, 0.0f);
        corner(r.right() - radius, r.bottom() - radius, kPi * 0.5f);
        corner(r.x() + radius, r.bottom() - radius, kPi);
        corner(r.x() + radius, r.y() + radius, kPi * 1.5f);
        return points;
    }

    std::vector<Element> elements_;                      ///< the operations, in order
    FillRule             fill_rule_{FillRule::kNonZero}; ///< how a fill covers the points the sub-paths cross
};
} // namespace reckless::gfx
