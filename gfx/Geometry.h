#pragma once

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace reckless::gfx
{
/**
    The panels' geometry, written against no toolkit: a point, a rectangle and an affine transform, on ints for
    layout and floats for drawing. The arithmetic is the series' layout's, to the last rounding - a rectangle's centre
    halves its size in its own type, a strip taken off one side is held to what there is, a size never goes below
    zero, a float rectangle reaches whole pixels by rounding half to even - so a panel laid out through these lands
    every component on the pixel it always did.
*/

/** Round half to even, as the default floating-point rounding mode does: the one rounding the layout uses.
    @tparam Float  the floating-point type
    @param value  the number to round
    @return       the nearest integer, a half going to the even one */
template <typename Float> inline int RoundToInt(Float value) noexcept
{
    return static_cast<int>(std::lrint(value));
}

/** A point, two coordinates in a unit the type says: pixels on ints, design pixels on floats.
    @tparam T  the coordinate type, int or float */
template <typename T> struct Point
{
    T x{}; ///< the horizontal coordinate
    T y{}; ///< the vertical coordinate

    /** The origin. */
    constexpr Point() = default;

    /** A point.
        @param x_value  the horizontal coordinate
        @param y_value  the vertical coordinate */
    constexpr Point(T x_value, T y_value) noexcept : x(x_value), y(y_value)
    {
    }

    /** The sum of two points, taken as vectors.
        @param other  the point to add
        @return       the sum */
    constexpr Point operator+(Point other) const noexcept
    {
        return {x + other.x, y + other.y};
    }

    /** The difference of two points, taken as vectors.
        @param other  the point to subtract
        @return       this less `other` */
    constexpr Point operator-(Point other) const noexcept
    {
        return {x - other.x, y - other.y};
    }

    /** Both coordinates scaled.
        @param scale  the factor
        @return       the scaled point */
    constexpr Point operator*(T scale) const noexcept
    {
        return {x * scale, y * scale};
    }

    /** Both coordinates divided, in the coordinate type.
        @param divisor  the divisor, not zero
        @return        the quotient point */
    constexpr Point operator/(T divisor) const noexcept
    {
        return {x / divisor, y / divisor};
    }

    /** Two points are equal when both coordinates are.
        @param a  the first point
        @param b  the second point
        @return   true when they coincide */
    friend constexpr bool operator==(Point a, Point b) noexcept
    {
        return !(a.x < b.x) && !(b.x < a.x) && !(a.y < b.y) && !(b.y < a.y);
    }

    /** The straight-line distance to `other`.
        @param other  the other point
        @return       the distance, in the coordinates' unit */
    T DistanceTo(Point other) const noexcept
    {
        return static_cast<T>(std::hypot(x - other.x, y - other.y));
    }

    /** This point on floats.
        @return the point, each coordinate converted */
    constexpr Point<float> ToFloat() const noexcept
    {
        return {static_cast<float>(x), static_cast<float>(y)};
    }

    /** Each coordinate rounded half to even.
        @return the point on ints */
    Point<int> ToNearestInt() const noexcept
    {
        return {RoundToInt(x), RoundToInt(y)};
    }
};

/** An axis-aligned rectangle by its position and size, y down: the left and top edges are in it, the right and
    bottom out.
    @tparam T  the coordinate type, int or float */
template <typename T> class Rect
{
  public:
    /** An empty rectangle at the origin. */
    constexpr Rect() = default;

    /** A rectangle.
        @param x       the left edge
        @param y       the top edge
        @param width   the width
        @param height  the height */
    constexpr Rect(T x, T y, T width, T height) noexcept : x_(x), y_(y), w_(width), h_(height)
    {
    }

    /** A rectangle at the origin.
        @param width   the width
        @param height  the height */
    constexpr Rect(T width, T height) noexcept : w_(width), h_(height)
    {
    }

    /** A rectangle from its edges.
        @param left    the left edge
        @param top     the top edge
        @param right   the right edge
        @param bottom  the bottom edge
        @return        the rectangle, its size the differences */
    static constexpr Rect LeftTopRightBottom(T left, T top, T right, T bottom) noexcept
    {
        return {left, top, right - left, bottom - top};
    }

    /** @return the left edge */
    constexpr T x() const noexcept
    {
        return x_;
    }

    /** @return the top edge */
    constexpr T y() const noexcept
    {
        return y_;
    }

    /** @return the width */
    constexpr T width() const noexcept
    {
        return w_;
    }

    /** @return the height */
    constexpr T height() const noexcept
    {
        return h_;
    }

    /** @return the right edge, x + width */
    constexpr T right() const noexcept
    {
        return x_ + w_;
    }

    /** @return the bottom edge, y + height */
    constexpr T bottom() const noexcept
    {
        return y_ + h_;
    }

    /** The centre, the size halved in the rectangle's own type (an int rectangle's centre rounds down).
        @return the horizontal centre */
    constexpr T CentreX() const noexcept
    {
        return x_ + w_ / static_cast<T>(2);
    }

    /** @return the vertical centre */
    constexpr T CentreY() const noexcept
    {
        return y_ + h_ / static_cast<T>(2);
    }

    /** @return the centre */
    constexpr Point<T> Centre() const noexcept
    {
        return {CentreX(), CentreY()};
    }

    /** @return the top left corner */
    constexpr Point<T> TopLeft() const noexcept
    {
        return {x_, y_};
    }

    /** @return the bottom right corner, just outside it */
    constexpr Point<T> BottomRight() const noexcept
    {
        return {right(), bottom()};
    }

    /** @return true when the width or the height is not above zero */
    constexpr bool IsEmpty() const noexcept
    {
        return !(w_ > T{}) || !(h_ > T{});
    }

    /** This rectangle moved horizontally.
        @param x  the new left edge
        @return   the rectangle, its size kept */
    constexpr Rect WithX(T x) const noexcept
    {
        return {x, y_, w_, h_};
    }

    /** This rectangle moved vertically.
        @param y  the new top edge
        @return   the rectangle, its size kept */
    constexpr Rect WithY(T y) const noexcept
    {
        return {x_, y, w_, h_};
    }

    /** This rectangle moved.
        @param x  the new left edge
        @param y  the new top edge
        @return   the rectangle, its size kept */
    constexpr Rect WithPosition(T x, T y) const noexcept
    {
        return {x, y, w_, h_};
    }

    /** This rectangle moved.
        @param position  the new top left corner
        @return          the rectangle, its size kept */
    constexpr Rect WithPosition(Point<T> position) const noexcept
    {
        return {position.x, position.y, w_, h_};
    }

    /** @return this rectangle moved to the origin, its size kept */
    constexpr Rect WithZeroOrigin() const noexcept
    {
        return {T{}, T{}, w_, h_};
    }

    /** This rectangle moved to a centre.
        @param centre  the new centre
        @return        the rectangle, its size kept */
    constexpr Rect WithCentre(Point<T> centre) const noexcept
    {
        return {centre.x - w_ / static_cast<T>(2), centre.y - h_ / static_cast<T>(2), w_, h_};
    }

    /** This rectangle at another width.
        @param width  the new width; below zero is held to zero
        @return       the rectangle, its left edge kept */
    constexpr Rect WithWidth(T width) const noexcept
    {
        return {x_, y_, std::max(T{}, width), h_};
    }

    /** This rectangle at another height.
        @param height  the new height; below zero is held to zero
        @return        the rectangle, its top edge kept */
    constexpr Rect WithHeight(T height) const noexcept
    {
        return {x_, y_, w_, std::max(T{}, height)};
    }

    /** This rectangle at another size.
        @param width   the new width
        @param height  the new height
        @return        the rectangle, its top left corner kept */
    constexpr Rect WithSize(T width, T height) const noexcept
    {
        return {x_, y_, width, height};
    }

    /** This rectangle at another size about the same centre.
        @param width   the new width
        @param height  the new height
        @return        the rectangle, its centre kept (to the type's rounding) */
    constexpr Rect WithSizeKeepingCentre(T width, T height) const noexcept
    {
        return {x_ + (w_ - width) / static_cast<T>(2), y_ + (h_ - height) / static_cast<T>(2), width, height};
    }

    /** The left edge moved to `left`, the right edge kept.
        @param left  the new left edge
        @return      the rectangle, its width held to zero or more */
    constexpr Rect WithLeft(T left) const noexcept
    {
        return {left, y_, std::max(T{}, x_ + w_ - left), h_};
    }

    /** The top edge moved to `top`, the bottom edge kept.
        @param top  the new top edge
        @return     the rectangle, its height held to zero or more */
    constexpr Rect WithTop(T top) const noexcept
    {
        return {x_, top, w_, std::max(T{}, y_ + h_ - top)};
    }

    /** The right edge moved to `right`, the left edge kept.
        @param right  the new right edge
        @return       the rectangle, its width held to zero or more */
    constexpr Rect WithRight(T right) const noexcept
    {
        return {std::min(x_, right), y_, std::max(T{}, right - x_), h_};
    }

    /** The bottom edge moved to `bottom`, the top edge kept.
        @param bottom  the new bottom edge
        @return        the rectangle, its height held to zero or more */
    constexpr Rect WithBottom(T bottom) const noexcept
    {
        return {x_, std::min(y_, bottom), w_, std::max(T{}, bottom - y_)};
    }

    /** The left edge moved right by `amount`, the right edge kept.
        @param amount  how much to take off the left
        @return        the rectangle, its width held to zero or more */
    constexpr Rect WithTrimmedLeft(T amount) const noexcept
    {
        return WithLeft(x_ + amount);
    }

    /** The right edge moved left by `amount`, the left edge kept.
        @param amount  how much to take off the right
        @return        the rectangle, its width held to zero or more */
    constexpr Rect WithTrimmedRight(T amount) const noexcept
    {
        return WithWidth(w_ - amount);
    }

    /** The top edge moved down by `amount`, the bottom edge kept.
        @param amount  how much to take off the top
        @return        the rectangle, its height held to zero or more */
    constexpr Rect WithTrimmedTop(T amount) const noexcept
    {
        return WithTop(y_ + amount);
    }

    /** The bottom edge moved up by `amount`, the top edge kept.
        @param amount  how much to take off the bottom
        @return        the rectangle, its height held to zero or more */
    constexpr Rect WithTrimmedBottom(T amount) const noexcept
    {
        return WithHeight(h_ - amount);
    }

    /** This rectangle moved by an offset.
        @param dx  the horizontal offset
        @param dy  the vertical offset
        @return    the rectangle, its size kept */
    constexpr Rect Translated(T dx, T dy) const noexcept
    {
        return {x_ + dx, y_ + dy, w_, h_};
    }

    /** This rectangle moved by an offset.
        @param delta  the offset
        @return       the rectangle, its size kept */
    constexpr Rect Translated(Point<T> delta) const noexcept
    {
        return Translated(delta.x, delta.y);
    }

    /** Larger by `dx` each side and `dy` top and bottom, never smaller than nothing.
        @param dx  how much to add at the left and at the right
        @param dy  how much to add at the top and at the bottom
        @return    the rectangle, its centre kept */
    constexpr Rect Expanded(T dx, T dy) const noexcept
    {
        return {x_ - dx, y_ - dy, std::max(T{}, w_ + dx * 2), std::max(T{}, h_ + dy * 2)};
    }

    /** Larger by `delta` on every side, never smaller than nothing.
        @param delta  how much to add at each edge
        @return       the rectangle, its centre kept */
    constexpr Rect Expanded(T delta) const noexcept
    {
        return Expanded(delta, delta);
    }

    /** Smaller by `dx` each side and `dy` top and bottom, never smaller than nothing.
        @param dx  how much to take off at the left and at the right
        @param dy  how much to take off at the top and at the bottom
        @return    the rectangle, its centre kept */
    constexpr Rect Reduced(T dx, T dy) const noexcept
    {
        return Expanded(-dx, -dy);
    }

    /** Smaller by `delta` on every side, never smaller than nothing.
        @param delta  how much to take off at each edge
        @return       the rectangle, its centre kept */
    constexpr Rect Reduced(T delta) const noexcept
    {
        return Reduced(delta, delta);
    }

    /** A strip `amount` tall off the top, held to the height there is; this rectangle keeps the rest.
        @param amount  the strip's height
        @return        the strip */
    Rect RemoveFromTop(T amount) noexcept
    {
        const Rect strip{x_, y_, w_, std::min(amount, h_)};
        y_ += strip.h_;
        h_ -= strip.h_;
        return strip;
    }

    /** A strip `amount` wide off the left, held to the width there is; this rectangle keeps the rest.
        @param amount  the strip's width
        @return        the strip */
    Rect RemoveFromLeft(T amount) noexcept
    {
        const Rect strip{x_, y_, std::min(amount, w_), h_};
        x_ += strip.w_;
        w_ -= strip.w_;
        return strip;
    }

    /** A strip `amount` wide off the right, held to the width there is; this rectangle keeps the rest.
        @param amount  the strip's width
        @return        the strip */
    Rect RemoveFromRight(T amount) noexcept
    {
        amount = std::min(amount, w_);
        const Rect strip{x_ + w_ - amount, y_, amount, h_};
        w_ -= amount;
        return strip;
    }

    /** A strip `amount` tall off the bottom, held to the height there is; this rectangle keeps the rest.
        @param amount  the strip's height
        @return        the strip */
    Rect RemoveFromBottom(T amount) noexcept
    {
        amount = std::min(amount, h_);
        const Rect strip{x_, y_ + h_ - amount, w_, amount};
        h_ -= amount;
        return strip;
    }

    /** Whether `point` is inside: the left and top edges in, the right and bottom out.
        @param point  the point to test
        @return       true when it is inside */
    constexpr bool Contains(Point<T> point) const noexcept
    {
        return point.x >= x_ && point.y >= y_ && point.x < x_ + w_ && point.y < y_ + h_;
    }

    /** Whether `other` lies wholly inside this rectangle, the edges counting as inside.
        @param other  the rectangle to test
        @return       true when it is inside */
    constexpr bool Contains(Rect other) const noexcept
    {
        return x_ <= other.x_ && y_ <= other.y_ && x_ + w_ >= other.x_ + other.w_ && y_ + h_ >= other.y_ + other.h_;
    }

    /** Whether the two overlap by more than an edge; an empty rectangle overlaps nothing.
        @param other  the rectangle to test
        @return       true when they share area */
    constexpr bool Intersects(Rect other) const noexcept
    {
        return x_ + w_ > other.x_ && y_ + h_ > other.y_ && x_ < other.x_ + other.w_ && y_ < other.y_ + other.h_ &&
               w_ > T{} && h_ > T{} && other.w_ > T{} && other.h_ > T{};
    }

    /** The overlap with `other`; an empty rectangle at the origin when there is none.
        @param other  the other rectangle
        @return       the overlap */
    constexpr Rect Intersection(Rect other) const noexcept
    {
        const T nx = std::max(x_, other.x_);
        const T ny = std::max(y_, other.y_);
        const T nw = std::min(x_ + w_, other.x_ + other.w_) - nx;
        if (nw >= T{})
        {
            const T nh = std::min(y_ + h_, other.y_ + other.h_) - ny;
            if (nh >= T{})
            {
                return {nx, ny, nw, nh};
            }
        }
        return {};
    }

    /** The smallest rectangle holding both; an empty one counts for nothing.
        @param other  the other rectangle
        @return       the union */
    constexpr Rect Union(Rect other) const noexcept
    {
        if (other.IsEmpty())
        {
            return *this;
        }
        if (IsEmpty())
        {
            return other;
        }
        const T nx = std::min(x_, other.x_);
        const T ny = std::min(y_, other.y_);
        return {nx, ny, std::max(x_ + w_, other.x_ + other.w_) - nx, std::max(y_ + h_, other.y_ + other.h_) - ny};
    }

    /** This rectangle on floats.
        @return the rectangle, every number converted */
    constexpr Rect<float> ToFloat() const noexcept
    {
        return {static_cast<float>(x_), static_cast<float>(y_), static_cast<float>(w_), static_cast<float>(h_)};
    }

    /** Position and size each rounded half to even.
        @return the rectangle on ints */
    Rect<int> ToNearestInt() const noexcept
    {
        return {RoundToInt(x_), RoundToInt(y_), RoundToInt(w_), RoundToInt(h_)};
    }

    /** The smallest whole-pixel rectangle holding this one.
        @return the rectangle on ints, its edges floored and ceiled */
    Rect<int> SmallestIntegerContainer() const noexcept
    {
        return Rect<int>::LeftTopRightBottom(static_cast<int>(std::floor(x_)), static_cast<int>(std::floor(y_)),
                                             static_cast<int>(std::ceil(x_ + w_)),
                                             static_cast<int>(std::ceil(y_ + h_)));
    }

    /** Two rectangles are equal when their positions and sizes are.
        @param a  the first rectangle
        @param b  the second rectangle
        @return   true when they coincide */
    friend constexpr bool operator==(const Rect &a, const Rect &b) noexcept
    {
        return a.TopLeft() == b.TopLeft() && !(a.w_ < b.w_) && !(b.w_ < a.w_) && !(a.h_ < b.h_) && !(b.h_ < a.h_);
    }

  private:
    T x_{}; ///< the left edge
    T y_{}; ///< the top edge
    T w_{}; ///< the width
    T h_{}; ///< the height
};

using PointF = Point<float>; ///< a point on floats, in design pixels
using PointI = Point<int>;   ///< a point on ints, in pixels
using RectF  = Rect<float>;  ///< a rectangle on floats, in design pixels
using RectI  = Rect<int>;    ///< a rectangle on ints, in pixels

/**
    A 2D affine transform, x' = m00 x + m01 y + m02 and y' = m10 x + m11 y + m12, on floats: what a panel rotates
    a knob's pointer by, scales a drawing to its screen with, and what a canvas carries for everything drawn after.
*/
struct Affine
{
    float m00{1.0f}; ///< x' from x
    float m01{0.0f}; ///< x' from y
    float m02{0.0f}; ///< x' offset
    float m10{0.0f}; ///< y' from x
    float m11{1.0f}; ///< y' from y
    float m12{0.0f}; ///< y' offset

    /** @return the transform that moves nothing */
    static constexpr Affine Identity() noexcept
    {
        return {};
    }

    /** A shift.
        @param dx  the horizontal shift
        @param dy  the vertical shift
        @return    the transform */
    static constexpr Affine Translation(float dx, float dy) noexcept
    {
        return {1.0f, 0.0f, dx, 0.0f, 1.0f, dy};
    }

    /** A scale about the origin.
        @param sx  the horizontal factor
        @param sy  the vertical factor
        @return    the transform */
    static constexpr Affine Scale(float sx, float sy) noexcept
    {
        return {sx, 0.0f, 0.0f, 0.0f, sy, 0.0f};
    }

    /** A uniform scale about the origin.
        @param s  the factor
        @return   the transform */
    static constexpr Affine Scale(float s) noexcept
    {
        return Scale(s, s);
    }

    /** Scaled about the point (px, py), which stays where it is.
        @param sx  the horizontal factor
        @param sy  the vertical factor
        @param px  the fixed point's x
        @param py  the fixed point's y
        @return    the transform */
    static constexpr Affine Scale(float sx, float sy, float px, float py) noexcept
    {
        return {sx, 0.0f, (1.0f - sx) * px, 0.0f, sy, (1.0f - sy) * py};
    }

    /** Rotated by `radians` clockwise on the screen (y down) about the origin.
        @param radians  the angle
        @return         the transform */
    static Affine Rotation(float radians) noexcept
    {
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        return {c, -s, 0.0f, s, c, 0.0f};
    }

    /** Rotated by `radians` about the point (px, py).
        @param radians  the angle, clockwise on the screen
        @param px       the fixed point's x
        @param py       the fixed point's y
        @return         the transform */
    static Affine Rotation(float radians, float px, float py) noexcept
    {
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        return {c, -s, -c * px + s * py + px, s, c, -s * px + -c * py + py};
    }

    /** This transform, then `next`.
        @param next  the transform applied after this one
        @return      the composition */
    constexpr Affine FollowedBy(const Affine &next) const noexcept
    {
        return {next.m00 * m00 + next.m01 * m10,
                next.m00 * m01 + next.m01 * m11,
                next.m00 * m02 + next.m01 * m12 + next.m02,
                next.m10 * m00 + next.m11 * m10,
                next.m10 * m01 + next.m11 * m11,
                next.m10 * m02 + next.m11 * m12 + next.m12};
    }

    /** This transform, then a shift.
        @param dx  the horizontal shift, applied last
        @param dy  the vertical shift, applied last
        @return    the transform */
    constexpr Affine Translated(float dx, float dy) const noexcept
    {
        return {m00, m01, m02 + dx, m10, m11, m12 + dy};
    }

    /** @return true when every coefficient is exactly the identity's */
    constexpr bool IsIdentity() const noexcept
    {
        return !(m00 < 1.0f) && !(m00 > 1.0f) && !(m11 < 1.0f) && !(m11 > 1.0f) && !(m01 < 0.0f) && !(m01 > 0.0f) &&
               !(m10 < 0.0f) && !(m10 > 0.0f) && !(m02 < 0.0f) && !(m02 > 0.0f) && !(m12 < 0.0f) && !(m12 > 0.0f);
    }

    /** A point taken through the transform.
        @param p  the point
        @return   the transformed point */
    constexpr PointF Apply(PointF p) const noexcept
    {
        return {m00 * p.x + m01 * p.y + m02, m10 * p.x + m11 * p.y + m12};
    }
};
} // namespace reckless::gfx
