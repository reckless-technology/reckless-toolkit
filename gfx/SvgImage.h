#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "Canvas.h"
#include "Color.h"
#include "Geometry.h"
#include "Path.h"
#include "SvgSyntax.h"

namespace reckless::gfx
{
/**
    A drawing's SVG document - an icon, a chess piece, a glyph a product draws at any size - read with no toolkit and
    drawn on any canvas as a browser draws it, by the SVG specification's rules (where the meters' reader, SvgArt.h,
    draws as JUCE's parser did). It reads a subset and refuses anything else with the reason, naming the element, the
    attribute or the text it does not draw, so a drawing that needs more fails where it is read rather than drawing
    wrong:

    - the root `<svg>` (xmlns, version, width, height, viewBox, preserveAspectRatio - every alignment, meet or slice,
      or none), its viewBox the drawing's own units; with none, its width and height are;
    - `<g>` (transform), and the shapes: `<path>` (d), `<circle>` (cx, cy, r), `<ellipse>` (cx, cy, rx, ry), `<rect>`
      (x, y, width, height, rx, ry), `<line>` (x1, y1, x2, y2), `<polyline>` and `<polygon>` (points);
    - path data of every command, absolute and relative - M, L, H, V, C, S, Q, T, A and Z - with the grammar's implicit
      repeats (coordinates after an M are lines), its numbers run together ("1.5.5", "-1-2") and an arc's flags
   unspaced; an arc drawn as cubic curves of a quarter turn or less;
    - transform lists of matrix, translate, scale, rotate (about the origin or a point), skewX and skewY;
    - in `<defs>`, `<linearGradient>` (x1, y1, x2, y2, as fractions or percentages of the bounds of the shape it paints,
      or in the document's units under gradientUnits="userSpaceOnUse"; spreadMethod pad) of `<stop>`s (offset, a number
      or a percentage, stop-color, stop-opacity);
    - on the root, a group or a shape - inherited down the tree as the specification inherits them - fill, fill-opacity,
      fill-rule (nonzero, evenodd), stroke, stroke-width, stroke-opacity, stroke-linejoin (miter, round, bevel),
      stroke-linecap (butt, round, square), stroke-miterlimit (4, the specification's and Skia's own), stroke-dasharray
      (none, or lengths); and, not inherited, opacity, which draws the element and everything in it as one layer;
    - colours `#rgb`, `#rrggbb`, black, white, none, and `url(#id)` of a gradient; numbers plain or in px; an id on
      anything; comments, an XML declaration.

    And as the specification draws them: a shape with no fill is filled black, and with no stroke not stroked; a stroke
    of a width of zero is not drawn; a gradient in the shape's bounds is the blend that bounds' own units make of it -
    its lines of one colour slanted with a box that is not square - and paints nothing on a shape with no width or no
    height; a gradient's colour is its first stop's before the first and its last stop's after the last, each stop's
    offset held at least the one before it, one stop is a solid colour and none paints nothing; a stroke's width, its
    dashes and the gradients scale with the element's transforms and the transform the document is drawn through. Each
    shape is filled, then stroked, in the document's order. Nothing is clipped to the viewBox.
*/
class SvgImage : private SvgSyntax
{
  public:
    /** The document in `text`, or none - with what refused it in `error`, where one is given - for a document outside
        the subset or not well formed.
        @param text   the document, as its bytes read as text
        @param error  where the reason is put when the document is refused; nullptr for none wanted
        @return       the document, or nothing for one outside the subset or not well formed */
    static std::optional<SvgImage> Parse(std::string_view text, std::string *error = nullptr)
    {
        SvgImage image;
        Reader   reader;
        if (!reader.Read(text, image))
        {
            if (error != nullptr)
            {
                *error = reader.error;
            }
            return std::nullopt;
        }
        return image;
    }

    /** @return the drawing's own units: its viewBox, or its width and height from the origin where it has none */
    RectF ViewBox() const noexcept
    {
        return view_box_;
    }

    /** The transform that puts the viewBox in `area` as the root's preserveAspectRatio asks: by default the whole
        drawing as large as fits, its aspect kept, centred - or filling `area` for "slice", or stretched to it for
        "none".
        @param area  where the drawing goes, in the canvas's units
        @return      the transform from the drawing's units onto the canvas's */
    Affine FitTransform(RectF area) const noexcept
    {
        const float sx = view_box_.width() > 0.0f ? area.width() / view_box_.width() : 1.0f;
        const float sy = view_box_.height() > 0.0f ? area.height() / view_box_.height() : 1.0f;
        if (stretch_)
        {
            return Affine::Translation(-view_box_.x(), -view_box_.y())
                .FollowedBy(Affine::Scale(sx, sy))
                .FollowedBy(Affine::Translation(area.x(), area.y()));
        }
        const float s  = slice_ ? std::max(sx, sy) : std::min(sx, sy);
        const float dx = area.x() + (area.width() - view_box_.width() * s) * align_x_;
        const float dy = area.y() + (area.height() - view_box_.height() * s) * align_y_;
        return Affine::Translation(-view_box_.x(), -view_box_.y())
            .FollowedBy(Affine::Scale(s))
            .FollowedBy(Affine::Translation(dx, dy));
    }

    /** The document on `c` through `t`, its units onto the canvas's; the canvas's state as it was afterwards.
        @param c  the canvas drawn on, which is left in the state it was in
        @param t  the transform from the drawing's units onto the canvas's */
    void Draw(Canvas &c, const Affine &t) const
    {
        const ScopedState state(c);
        c.AddTransform(t);
        for (const Step &step : steps_)
        {
            switch (step.kind)
            {
            case Step::Kind::kBeginLayer:
                c.BeginTransparencyLayer(step.opacity);
                break;
            case Step::Kind::kEndLayer:
                c.EndTransparencyLayer();
                break;
            case Step::Kind::kShape:
                DrawShape(c, step.shape);
                break;
            }
        }
    }

    /** The document in `area`, placed by FitTransform().
        @param c     the canvas drawn on, which is left in the state it was in
        @param area  where the drawing goes, in the canvas's units */
    void DrawIn(Canvas &c, RectF area) const
    {
        Draw(c, FitTransform(area));
    }

    /** @return how many shapes the document draws */
    int Shapes() const noexcept
    {
        return static_cast<int>(
            std::count_if(steps_.begin(), steps_.end(), [](const Step &s) { return s.kind == Step::Kind::kShape; }));
    }

  private:
    /** A fill or a stroke's paint: a solid colour or a gradient. */
    using Paint = std::variant<Color, Gradient>;

    /** A shape as it is drawn: its outline in its own units, the transform from them to the drawing's, its fill and
        its stroke. */
    struct Shape
    {
        Path                 path;         ///< the outline, in the shape's own units
        Affine               transform;    ///< the shape's units onto the drawing's: its groups' and its own transforms
        std::optional<Paint> fill;         ///< the fill; none for no fill
        std::optional<Paint> stroke_paint; ///< the stroke's paint; none for no stroke
        Stroke               stroke;       ///< the stroke's width, joins, ends and dashes, in the shape's units
    };

    /** One step of the drawing: a shape, or the start or the end of a layer drawn at an opacity. */
    struct Step
    {
        /** What the step does. */
        enum class Kind
        {
            kShape,      ///< draws `shape`
            kBeginLayer, ///< begins a layer composited at `opacity`
            kEndLayer,   ///< ends the layer the matching kBeginLayer began
        };

        Kind  kind{Kind::kShape}; ///< what the step does
        Shape shape;              ///< the shape, for a kShape step
        float opacity{1.0f};      ///< the layer's opacity, 0 to 1, for a kBeginLayer step
    };

    /** Make a paint the canvas's brush.
        @param c      the canvas
        @param paint  the colour or gradient */
    static void SetPaint(Canvas &c, const Paint &paint)
    {
        if (const auto *colour = std::get_if<Color>(&paint))
        {
            c.SetColor(*colour);
        }
        else
        {
            c.SetGradient(std::get<Gradient>(paint));
        }
    }

    /** Draw a shape through its transform: its fill, then its stroke.
        @param c      the canvas, the document's transform already on it
        @param shape  the shape */
    static void DrawShape(Canvas &c, const Shape &shape)
    {
        const ScopedState state(c);
        if (!shape.transform.IsIdentity())
        {
            c.AddTransform(shape.transform);
        }
        if (shape.fill)
        {
            SetPaint(c, *shape.fill);
            c.FillPath(shape.path);
        }
        if (shape.stroke_paint)
        {
            SetPaint(c, *shape.stroke_paint);
            c.StrokePath(shape.path, shape.stroke);
        }
    }

    // ---- The reader ------------------------------------------------------------------------------------------------

    /** A linear gradient as the document defines it, before it meets the shape it paints. */
    struct GradientDef
    {
        bool  bounding_box{true}; ///< true for coordinates in the painted shape's bounds, false for the drawing's
        float x1{0.0f},           ///< the start's x
            y1{0.0f},             ///< the start's y
            x2{1.0f},             ///< the end's x
            y2{0.0f};             ///< the end's y
        std::vector<std::pair<float, Color>> stops; ///< the stops, offset 0 to 1, each at least the one before
    };

    /** Reads the document's text into the steps it draws: the XML, then every gradient defined anywhere in it, then
        the drawing, refusing what it does not draw as the specification does. */
    class Reader
    {
      public:
        std::string error; ///< the reason the last Read() refused, or empty

        /** Read a document.
            @param text   the document
            @param image  the drawing, filled in
            @return       true when the document is within the subset; else false with `error` set */
        bool Read(std::string_view text, SvgImage &image)
        {
            Node root;
            Xml  xml(text);
            if (!xml.Parse(root, error))
            {
                return false;
            }
            Link(root);
            if (root.name != "svg")
            {
                return Refuse("a root that is not <svg>");
            }
            if (!Allowed(root, {"xmlns", "version", "width", "height", "viewBox", "preserveAspectRatio"}) ||
                !RootGeometry(root, image) || !Definitions(root))
            {
                return false;
            }
            return Group(root, Affine{}, image.steps_);
        }

      private:
        /** The attributes a drawn element may carry besides its own: the presentation attributes, opacity and an id. */
        static constexpr std::string_view kPresentation[] = {"fill",
                                                             "fill-opacity",
                                                             "fill-rule",
                                                             "stroke",
                                                             "stroke-width",
                                                             "stroke-opacity",
                                                             "stroke-linejoin",
                                                             "stroke-linecap",
                                                             "stroke-miterlimit",
                                                             "stroke-dasharray",
                                                             "opacity",
                                                             "id"};

        /** Record why the document is refused.
            @param why  the reason
            @return     false, to be returned */
        bool Refuse(std::string why)
        {
            error = std::move(why);
            return false;
        }

        /** Set every descendant's parent.
            @param node  the element whose descendants are linked */
        static void Link(Node &node)
        {
            for (Node &child : node.children)
            {
                child.parent = &node;
                Link(child);
            }
        }

        /** Every attribute of `node` among `own` or the presentation attributes, and the first that is not refused.
            @param node  the element
            @param own   the attributes of its own it may carry
            @return      true when every attribute is allowed; else false with `error` set */
        bool Allowed(const Node &node, std::initializer_list<std::string_view> own)
        {
            for (const auto &[key, value] : node.attributes)
            {
                if (std::find(own.begin(), own.end(), key) == own.end() &&
                    std::find(std::begin(kPresentation), std::end(kPresentation), key) == std::end(kPresentation))
                {
                    return Refuse("<" + node.name + "> with " + key);
                }
            }
            return true;
        }

        /** Every attribute of `node` among `allowed` alone - for an element that takes no presentation attributes.
            @param node     the element
            @param allowed  the attribute names it may carry
            @return         true when every attribute is allowed; else false with `error` set */
        bool AllowedOnly(const Node &node, std::initializer_list<std::string_view> allowed)
        {
            for (const auto &[key, value] : node.attributes)
            {
                if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
                {
                    return Refuse("<" + node.name + "> with " + key);
                }
            }
            return true;
        }

        /** A length: a plain number, or one in px.
            @param text  the attribute's text
            @param out   the number, set on success
            @return      true when the text is a length the reader reads */
        static bool LengthText(std::string_view text, float &out)
        {
            while (!text.empty() &&
                   (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r'))
            {
                text.remove_suffix(1);
            }
            if (text.size() > 2 && text.substr(text.size() - 2) == "px")
            {
                text.remove_suffix(2);
            }
            return Number(text, out);
        }

        /** An attribute that is a length, `otherwise` where it is absent.
            @param node       the element
            @param key        the attribute's name
            @param out        the length; set to `otherwise` when absent
            @param otherwise  the value for an absent attribute
            @return           true when the attribute is absent or a length */
        bool Length(const Node &node, std::string_view key, float &out, float otherwise = 0.0f)
        {
            const std::string *text = node.Find(key);
            if (text == nullptr)
            {
                out = otherwise;
                return true;
            }
            if (!LengthText(*text, out))
            {
                return Refuse("<" + node.name + ">'s " + std::string(key) + " is not a plain number: " + *text);
            }
            return true;
        }

        /** A length that may not be negative.
            @param node  the element
            @param key   the attribute's name
            @param out   the length; zero when absent
            @return      true when the attribute is absent or a length of zero or more */
        bool NonNegative(const Node &node, std::string_view key, float &out)
        {
            if (!Length(node, key, out))
            {
                return false;
            }
            if (out < 0.0f)
            {
                return Refuse("<" + node.name + ">'s " + std::string(key) + " is negative");
            }
            return true;
        }

        /** The root's viewBox, size and preserveAspectRatio.
            @param root   the root element
            @param image  the drawing, its viewBox and placement set
            @return       true when they are within the subset */
        bool RootGeometry(const Node &root, SvgImage &image)
        {
            float width = 0.0f, height = 0.0f;
            if (!NonNegative(root, "width", width) || !NonNegative(root, "height", height))
            {
                return false;
            }
            if (const std::string *box = root.Find("viewBox"))
            {
                std::string_view text = *box;
                double           v[4]{};
                for (double &n : v)
                {
                    SkipSeparators(text);
                    if (!TakeNumber(text, n))
                    {
                        return Refuse("a viewBox that is not four numbers: " + *box);
                    }
                }
                SkipSeparators(text);
                if (!text.empty() || !(v[2] > 0.0) || !(v[3] > 0.0))
                {
                    return Refuse("a viewBox that is not four numbers with a width and a height: " + *box);
                }
                image.view_box_ = {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]),
                                   static_cast<float>(v[3])};
            }
            else if (width > 0.0f && height > 0.0f)
            {
                image.view_box_ = {0.0f, 0.0f, width, height};
            }
            else
            {
                return Refuse("an <svg> with neither a viewBox nor a width and a height");
            }
            if (const std::string *aspect = root.Find("preserveAspectRatio"))
            {
                std::string_view text = *aspect;
                SkipSeparators(text);
                const auto       space = text.find(' ');
                std::string_view align = text.substr(0, space);
                std::string_view rest  = space == std::string_view::npos ? std::string_view{} : text.substr(space);
                SkipSeparators(rest);
                if (align == "none" && rest.empty())
                {
                    image.stretch_ = true;
                    return true;
                }
                const auto fraction = [](std::string_view a) {
                    return a == "Min" ? 0.0f : a == "Mid" ? 0.5f : a == "Max" ? 1.0f : -1.0f;
                };
                if (align.size() != 8 || align[0] != 'x' || align[4] != 'Y' ||
                    (rest != "" && rest != "meet" && rest != "slice"))
                {
                    return Refuse("a preserveAspectRatio the reader does not read: " + *aspect);
                }
                image.align_x_ = fraction(align.substr(1, 3));
                image.align_y_ = fraction(align.substr(5, 3));
                image.slice_   = rest == "slice";
                if (image.align_x_ < 0.0f || image.align_y_ < 0.0f)
                {
                    return Refuse("a preserveAspectRatio the reader does not read: " + *aspect);
                }
            }
            return true;
        }

        /** Every `<defs>` anywhere in the document read, so a shape may name a gradient defined after it.
            @param node  the element whose descendants are searched
            @return      true when every definition is within the subset */
        bool Definitions(const Node &node)
        {
            for (const Node &child : node.children)
            {
                if (child.name == "defs")
                {
                    if (!AllowedOnly(child, {"id"}))
                    {
                        return false;
                    }
                    for (const Node &def : child.children)
                    {
                        if (def.name != "linearGradient")
                        {
                            return Refuse("<defs> with <" + def.name + ">, which the reader does not draw");
                        }
                        if (!LinearGradient(def))
                        {
                            return false;
                        }
                    }
                }
                else if (!Definitions(child))
                {
                    return false;
                }
            }
            return true;
        }

        /** A gradient coordinate: a number, or a percentage where the coordinates are fractions of the bounds.
            @param node          the gradient element
            @param key           the attribute's name
            @param bounding_box  true when the gradient is in the painted shape's bounds
            @param out           the coordinate; set to `otherwise` when absent
            @param otherwise     the value for an absent attribute
            @return              true when the attribute is absent or within the subset */
        bool GradientCoordinate(const Node &node, std::string_view key, bool bounding_box, float &out, float otherwise)
        {
            const std::string *text = node.Find(key);
            if (text == nullptr)
            {
                out = otherwise;
                return true;
            }
            std::string_view number  = *text;
            const bool       percent = !number.empty() && number.back() == '%';
            if (percent)
            {
                number.remove_suffix(1);
            }
            if ((percent && !bounding_box) || !(percent ? Number(number, out) : LengthText(number, out)))
            {
                return Refuse("<" + node.name + ">'s " + std::string(key) +
                              " is not a number the reader reads: " + *text);
            }
            out *= percent ? 0.01f : 1.0f;
            return true;
        }

        /** A colour the reader reads: `#rgb`, `#rrggbb`, black or white.
            @param text  the colour's text
            @return      the colour, or none for any other text */
        static std::optional<Color> ColourOf(std::string_view text)
        {
            if (text == "black")
            {
                return colors::kBlack;
            }
            if (text == "white")
            {
                return colors::kWhite;
            }
            return HexColour(text);
        }

        /** An opacity: a number held to 0 to 1, 1 where absent.
            @param node  the element
            @param text  the attribute's text; nullptr for none
            @param key   the attribute's name, for the reason
            @param out   the opacity
            @return      true when absent or a number */
        bool OpacityOf(const Node &node, const std::string *text, std::string_view key, float &out)
        {
            out = 1.0f;
            if (text == nullptr)
            {
                return true;
            }
            if (!Number(*text, out))
            {
                return Refuse("<" + node.name + ">'s " + std::string(key) + " is not a number: " + *text);
            }
            out = std::clamp(out, 0.0f, 1.0f);
            return true;
        }

        /** A linear gradient's geometry and stops, kept by its id.
            @param node  the gradient element
            @return      true when it is within the subset */
        bool LinearGradient(const Node &node)
        {
            const std::string *id = node.Find("id");
            if (id == nullptr)
            {
                return Refuse("<linearGradient> with no id");
            }
            if (!AllowedOnly(node, {"id", "x1", "y1", "x2", "y2", "gradientUnits", "spreadMethod"}))
            {
                return false;
            }
            GradientDef def;
            if (const std::string *units = node.Find("gradientUnits"))
            {
                if (*units != "objectBoundingBox" && *units != "userSpaceOnUse")
                {
                    return Refuse("a gradientUnits the reader does not read: " + *units);
                }
                def.bounding_box = *units == "objectBoundingBox";
            }
            if (const std::string *spread = node.Find("spreadMethod"); spread != nullptr && *spread != "pad")
            {
                return Refuse("a spreadMethod other than pad: " + *spread);
            }
            if (!GradientCoordinate(node, "x1", def.bounding_box, def.x1, 0.0f) ||
                !GradientCoordinate(node, "y1", def.bounding_box, def.y1, 0.0f) ||
                !GradientCoordinate(node, "x2", def.bounding_box, def.x2, def.bounding_box ? 1.0f : 0.0f) ||
                !GradientCoordinate(node, "y2", def.bounding_box, def.y2, 0.0f))
            {
                return false;
            }
            if (!def.bounding_box && node.Find("x2") == nullptr)
            {
                return Refuse("a userSpaceOnUse <linearGradient> with no x2"); // its default is a percentage
            }
            for (const Node &stop : node.children)
            {
                if (stop.name != "stop")
                {
                    return Refuse("<linearGradient> with <" + stop.name + ">");
                }
                if (!AllowedOnly(stop, {"offset", "stop-color", "stop-opacity", "id"}))
                {
                    return false;
                }
                if (!stop.children.empty())
                {
                    return Refuse("<stop> with children");
                }
                const std::string   *colour_text = stop.Find("stop-color");
                std::optional<Color> colour      = colour_text != nullptr ? ColourOf(*colour_text) : colors::kBlack;
                if (!colour)
                {
                    return Refuse("a stop-color that is not #rgb, #rrggbb, black or white: " + *colour_text);
                }
                float opacity = 1.0f;
                if (!OpacityOf(stop, stop.Find("stop-opacity"), "stop-opacity", opacity))
                {
                    return false;
                }
                float offset = 0.0f;
                if (const std::string *offset_text = stop.Find("offset"))
                {
                    std::string_view text    = *offset_text;
                    const bool       percent = !text.empty() && text.back() == '%';
                    if (percent)
                    {
                        text.remove_suffix(1);
                    }
                    if (!Number(text, offset))
                    {
                        return Refuse("a stop offset that is not a number: " + *offset_text);
                    }
                    offset *= percent ? 0.01f : 1.0f;
                }
                offset = std::clamp(offset, 0.0f, 1.0f);
                if (!def.stops.empty())
                {
                    offset = std::max(offset, def.stops.back().first);
                }
                def.stops.emplace_back(offset, colour->WithMultipliedAlpha(opacity));
            }
            gradients_[*id] = std::move(def);
            return true;
        }

        /** An attribute of `node` or of its nearest ancestor that has it, as a presentation attribute inherits.
            @param node  the element
            @param key   the attribute's name
            @return      the value, valid as long as the document's nodes; nullptr when none has it */
        static const std::string *Inherited(const Node &node, std::string_view key)
        {
            for (const Node *n = &node; n != nullptr; n = n->parent)
            {
                if (const std::string *value = n->Find(key))
                {
                    return value;
                }
            }
            return nullptr;
        }

        /** The bounds of an outline in its own units, its curves flattened finely.
            @param path  the outline
            @return      the bounds; empty at the origin for an outline with no points */
        static RectF FineBounds(const Path &path)
        {
            bool  any = false;
            float l = 0.0f, t = 0.0f, r = 0.0f, b = 0.0f;
            for (const auto &polygon : path.Flatten(0.01f))
            {
                for (const PointF p : polygon.points)
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

        /** `text` - a fill's or a stroke's - as the paint for the outline `path` at `opacity`: none, a colour, or a
            gradient placed on the outline's bounds or in the drawing's units.
            @param text     the fill or stroke attribute
            @param path     the outline the paint is for, in its own units
            @param opacity  the alpha it is multiplied by, 0 to 1
            @param out      the paint; none for `none`, or for a gradient that paints nothing
            @return         true when the text is within the subset */
        bool PaintOf(const std::string &text, const Path &path, float opacity, std::optional<Paint> &out)
        {
            out.reset();
            if (text == "none")
            {
                return true;
            }
            if (const auto colour = ColourOf(text))
            {
                out = colour->WithMultipliedAlpha(opacity);
                return true;
            }
            const std::string id = UrlId(text);
            const auto        it = gradients_.find(id);
            if (it == gradients_.end())
            {
                return Refuse("a paint that is not #rgb, #rrggbb, black, white, none or a gradient's url: " + text);
            }
            const GradientDef &def = it->second;
            if (def.stops.empty())
            {
                return true; // no stops: painted as none
            }
            if (def.stops.size() == 1)
            {
                out = def.stops.front().second.WithMultipliedAlpha(opacity);
                return true;
            }
            PointF start{def.x1, def.y1};
            PointF end{def.x2, def.y2};
            if (def.bounding_box)
            {
                const RectF box = FineBounds(path);
                if (!(box.width() > 0.0f) || !(box.height() > 0.0f))
                {
                    return true; // a box with no area: the gradient paints nothing
                }
                // The blend is linear in the box's own units, so it is linear in the shape's: the colour at a point is
                // its offset along G = (dx / w, dy / h) / |d|^2 from the start, which a gradient from the start to the
                // start plus G / |G|^2 gives exactly - its lines of one colour slanted as the box's units slant them.
                const double dx = def.x2 - def.x1, dy = def.y2 - def.y1;
                const double d2 = dx * dx + dy * dy;
                start           = {box.x() + box.width() * def.x1, box.y() + box.height() * def.y1};
                if (d2 > 0.0)
                {
                    const double gx = dx / box.width() / d2, gy = dy / box.height() / d2;
                    const double g2 = gx * gx + gy * gy;
                    end             = {static_cast<float>(start.x + gx / g2), static_cast<float>(start.y + gy / g2)};
                }
                else
                {
                    end = start;
                }
            }
            if (Same(start, end))
            {
                out = def.stops.back().second.WithMultipliedAlpha(opacity); // no direction: the last stop's colour
                return true;
            }
            Gradient g;
            g.start       = start;
            g.end         = end;
            g.start_color = def.stops.front().second.WithMultipliedAlpha(opacity);
            g.end_color   = def.stops.back().second.WithMultipliedAlpha(opacity);
            for (const auto &[offset, colour] : def.stops)
            {
                g.stops.emplace_back(offset, colour.WithMultipliedAlpha(opacity));
            }
            out = std::move(g);
            return true;
        }

        /** One function of a transform list, its name and its numbers.
            @param name     the function's name
            @param n        its numbers
            @param count    how many numbers it has
            @param out      the function's transform
            @return         true when the name is one the reader reads with a count it takes */
        static bool TransformFunction(std::string_view name, const double *n, int count, Affine &out)
        {
            constexpr double kRadians = std::numbers::pi / 180.0;
            const auto       f        = [](double v) { return static_cast<float>(v); };
            if (name == "matrix" && count == 6)
            {
                out = {f(n[0]), f(n[2]), f(n[4]), f(n[1]), f(n[3]), f(n[5])};
            }
            else if (name == "translate" && (count == 1 || count == 2))
            {
                out = Affine::Translation(f(n[0]), count == 2 ? f(n[1]) : 0.0f);
            }
            else if (name == "scale" && (count == 1 || count == 2))
            {
                out = Affine::Scale(f(n[0]), f(count == 2 ? n[1] : n[0]));
            }
            else if (name == "rotate" && (count == 1 || count == 3))
            {
                const double c = std::cos(n[0] * kRadians), s = std::sin(n[0] * kRadians);
                const double cx = count == 3 ? n[1] : 0.0, cy = count == 3 ? n[2] : 0.0;
                out = {f(c), f(-s), f(cx - c * cx + s * cy), f(s), f(c), f(cy - s * cx - c * cy)};
            }
            else if (name == "skewX" && count == 1)
            {
                out = {1.0f, f(std::tan(n[0] * kRadians)), 0.0f, 0.0f, 1.0f, 0.0f};
            }
            else if (name == "skewY" && count == 1)
            {
                out = {1.0f, 0.0f, 0.0f, f(std::tan(n[0] * kRadians)), 1.0f, 0.0f};
            }
            else
            {
                return false;
            }
            return true;
        }

        /** `node`'s transform followed by `outer`: its transform list, the first function outermost.
            @param node   the element
            @param outer  the transform in force outside it
            @param out    the node's transform followed by `outer`
            @return       true when the transform is within the subset */
        bool TransformOf(const Node &node, const Affine &outer, Affine &out)
        {
            out                     = outer;
            const std::string *text = node.Find("transform");
            if (text == nullptr)
            {
                return true;
            }
            std::string_view t = *text;
            Affine           list;
            for (SkipSeparators(t); !t.empty(); SkipSeparators(t))
            {
                std::size_t i = 0;
                while (i < t.size() && ((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z')))
                {
                    ++i;
                }
                const std::string_view name = t.substr(0, i);
                t.remove_prefix(i);
                SkipSeparators(t);
                if (t.empty() || t.front() != '(')
                {
                    return Refuse("a transform the reader does not read: " + *text);
                }
                t.remove_prefix(1);
                double numbers[6]{};
                int    count = 0;
                for (SkipSeparators(t); !t.empty() && t.front() != ')'; SkipSeparators(t))
                {
                    if (count == 6 || !TakeNumber(t, numbers[count]))
                    {
                        return Refuse("a transform the reader does not read: " + *text);
                    }
                    ++count;
                }
                Affine function;
                if (t.empty() || !TransformFunction(name, numbers, count, function))
                {
                    return Refuse("a transform the reader does not read: " + *text);
                }
                t.remove_prefix(1);
                list = function.FollowedBy(list);
            }
            out = list.FollowedBy(outer);
            return true;
        }

        /** The children of a group - or of the root - into `steps`, under `transform`, as one layer when it has an
            opacity under 1.
            @param node       the group or the root
            @param transform  the transform in force inside it
            @param steps      the steps, appended to
            @return           true when everything in it is within the subset */
        bool Group(const Node &node, const Affine &transform, std::vector<Step> &steps)
        {
            float opacity = 1.0f;
            if (!OpacityOf(node, node.Find("opacity"), "opacity", opacity))
            {
                return false;
            }
            const bool layer = opacity < 1.0f;
            if (layer)
            {
                steps.push_back({Step::Kind::kBeginLayer, {}, opacity});
            }
            for (const Node &child : node.children)
            {
                if (child.name == "defs")
                {
                    continue; // read already
                }
                if (!Drawn(child, transform, steps))
                {
                    return false;
                }
            }
            if (layer)
            {
                steps.push_back({Step::Kind::kEndLayer, {}, 1.0f});
            }
            return true;
        }

        /** A list of points - a polyline's or a polygon's - as an outline.
            @param node   the element
            @param close  true to close the outline, for a polygon
            @param out    the outline, added to
            @return       true when the points are pairs of numbers */
        bool PointsOutline(const Node &node, bool close, Path &out)
        {
            const std::string *points = node.Find("points");
            if (points == nullptr)
            {
                return Refuse("<" + node.name + "> with no points");
            }
            std::string_view s     = *points;
            int              count = 0;
            for (SkipSeparators(s); !s.empty(); SkipSeparators(s))
            {
                double x = 0.0, y = 0.0;
                if (!TakeNumber(s, x))
                {
                    return Refuse("points that are not pairs of numbers: " + *points);
                }
                SkipSeparators(s);
                if (!TakeNumber(s, y))
                {
                    return Refuse("points that are not pairs of numbers: " + *points);
                }
                if (count++ == 0)
                {
                    out.MoveTo(static_cast<float>(x), static_cast<float>(y));
                }
                else
                {
                    out.LineTo(static_cast<float>(x), static_cast<float>(y));
                }
            }
            if (close && count > 0)
            {
                out.Close();
            }
            return true;
        }

        /** A rectangle's outline as the specification writes it: from (x + rx, y) clockwise, its corners quarter
            ellipses of rx by ry - each the other where only one is given, held to half the side.
            @param node  the rect element
            @param out   the outline, added to; nothing for a rectangle of no width or height
            @return      true when its numbers are lengths of zero or more */
        bool RectOutline(const Node &node, Path &out)
        {
            float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f, rx = 0.0f, ry = 0.0f;
            if (!Length(node, "x", x) || !Length(node, "y", y) || !NonNegative(node, "width", w) ||
                !NonNegative(node, "height", h) || !NonNegative(node, "rx", rx) || !NonNegative(node, "ry", ry))
            {
                return false;
            }
            if (!(w > 0.0f) || !(h > 0.0f))
            {
                return true;
            }
            const bool has_rx = node.Find("rx") != nullptr, has_ry = node.Find("ry") != nullptr;
            rx = has_rx ? rx : ry;
            ry = has_ry ? ry : rx;
            rx = std::min(rx, w * 0.5f);
            ry = std::min(ry, h * 0.5f);
            if (!(rx > 0.0f) || !(ry > 0.0f))
            {
                out.MoveTo(x, y);
                out.LineTo(x + w, y);
                out.LineTo(x + w, y + h);
                out.LineTo(x, y + h);
                out.Close();
                return true;
            }
            constexpr float k = 0.5522847498f; // a quarter circle's cubic handles, as a fraction of its radius
            const float     r = x + w, b = y + h;
            out.MoveTo(x + rx, y);
            out.LineTo(r - rx, y);
            out.CubicTo(r - rx + k * rx, y, r, y + ry - k * ry, r, y + ry);
            out.LineTo(r, b - ry);
            out.CubicTo(r, b - ry + k * ry, r - rx + k * rx, b, r - rx, b);
            out.LineTo(x + rx, b);
            out.CubicTo(x + rx - k * rx, b, x, b - ry + k * ry, x, b - ry);
            out.LineTo(x, y + ry);
            out.CubicTo(x, y + ry - k * ry, x + rx - k * rx, y, x + rx, y);
            out.Close();
            return true;
        }

        /** An SVG arc from `from` as cubic curves of a quarter turn or less, by the specification's conversion from
            its endpoints to its centre - a line for a radius of zero, nothing for an arc that ends where it starts.
            @param out    the outline, added to
            @param from   the arc's start, the current point
            @param rx     the horizontal radius
            @param ry     the vertical radius
            @param angle  the ellipse's rotation, in degrees
            @param large  the large-arc flag
            @param sweep  the sweep flag: true for the positive-angle direction, clockwise on the screen
            @param to     the arc's end */
        static void ArcTo(Path &out, PointF from, double rx, double ry, double angle, bool large, bool sweep, PointF to)
        {
            if (Same(from, to))
            {
                return;
            }
            rx = std::abs(rx);
            ry = std::abs(ry);
            if (!(rx > 0.0) || !(ry > 0.0))
            {
                out.LineTo(to);
                return;
            }
            const double phi = angle * std::numbers::pi / 180.0;
            const double c = std::cos(phi), s = std::sin(phi);
            const double hx = (from.x - to.x) * 0.5, hy = (from.y - to.y) * 0.5;
            const double x1 = c * hx + s * hy, y1 = -s * hx + c * hy;
            const double lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
            if (lambda > 1.0)
            {
                rx *= std::sqrt(lambda);
                ry *= std::sqrt(lambda);
            }
            const double rx2 = rx * rx, ry2 = ry * ry;
            const double num = rx2 * ry2 - rx2 * y1 * y1 - ry2 * x1 * x1;
            const double den = rx2 * y1 * y1 + ry2 * x1 * x1;
            double       co  = den > 0.0 ? std::sqrt(std::max(0.0, num / den)) : 0.0;
            if (large == sweep)
            {
                co = -co;
            }
            const double cxp = co * rx * y1 / ry, cyp = -co * ry * x1 / rx;
            const double cx       = c * cxp - s * cyp + (from.x + to.x) * 0.5;
            const double cy       = s * cxp + c * cyp + (from.y + to.y) * 0.5;
            const auto   angle_of = [](double ux, double uy, double vx, double vy) {
                return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
            };
            const double ux = (x1 - cxp) / rx, uy = (y1 - cyp) / ry;
            const double vx = (-x1 - cxp) / rx, vy = (-y1 - cyp) / ry;
            const double theta = angle_of(1.0, 0.0, ux, uy);
            double       delta = angle_of(ux, uy, vx, vy);
            if (!sweep && delta > 0.0)
            {
                delta -= 2.0 * std::numbers::pi;
            }
            else if (sweep && delta < 0.0)
            {
                delta += 2.0 * std::numbers::pi;
            }
            const int segments =
                std::max(1, static_cast<int>(std::ceil(std::abs(delta) / (std::numbers::pi / 2.0) - 1e-9)));
            const double step  = delta / segments;
            const double k     = 4.0 / 3.0 * std::tan(step / 4.0);
            const auto   point = [&](double t) {
                const double ex = rx * std::cos(t), ey = ry * std::sin(t);
                return std::pair<double, double>{cx + c * ex - s * ey, cy + s * ex + c * ey};
            };
            const auto tangent = [&](double t) {
                const double ex = -rx * std::sin(t), ey = ry * std::cos(t);
                return std::pair<double, double>{c * ex - s * ey, s * ex + c * ey};
            };
            double t0 = theta;
            for (int i = 0; i < segments; ++i)
            {
                const double t1       = i + 1 == segments ? theta + delta : t0 + step;
                const auto [p0x, p0y] = point(t0);
                const auto [d0x, d0y] = tangent(t0);
                const auto [p1x, p1y] = point(t1);
                const auto [d1x, d1y] = tangent(t1);
                const bool last       = i + 1 == segments;
                const auto f          = [](double v) { return static_cast<float>(v); };
                out.CubicTo(f(p0x + k * d0x), f(p0y + k * d0y), f(p1x - k * d1x), f(p1y - k * d1y),
                            last ? to.x : f(p1x), last ? to.y : f(p1y));
                t0 = t1;
            }
        }

        /** The path data of every command, absolute and relative, walked as the specification walks it.
            @param node  the path element
            @param out   the outline, added to
            @return      true when the data is within the grammar */
        bool PathData(const Node &node, Path &out)
        {
            const std::string *d = node.Find("d");
            if (d == nullptr)
            {
                return Refuse("a <path> with no d");
            }
            std::string_view s = *d;
            PointF           current{}, start{}, control{};
            char             command = 0, previous = 0;
            const auto       number = [&s](double &v) {
                SkipSeparators(s);
                return TakeNumber(s, v);
            };
            const auto pair = [&](PointF &p, bool relative) {
                double x = 0.0, y = 0.0;
                if (!number(x) || !number(y))
                {
                    return false;
                }
                p = {static_cast<float>(x) + (relative ? current.x : 0.0f),
                     static_cast<float>(y) + (relative ? current.y : 0.0f)};
                return true;
            };
            const auto flag = [&s](bool &f) {
                SkipSeparators(s);
                if (s.empty() || (s.front() != '0' && s.front() != '1'))
                {
                    return false;
                }
                f = s.front() == '1';
                s.remove_prefix(1);
                return true;
            };
            const auto bad = [&](const char *what) { return Refuse(std::string(what) + " in path data: " + *d); };
            for (SkipSeparators(s); !s.empty(); SkipSeparators(s))
            {
                const char c = s.front();
                if (std::string_view("MmLlHhVvCcSsQqTtAaZz").find(c) != std::string_view::npos)
                {
                    command = c;
                    s.remove_prefix(1);
                }
                else if (!(Digit(c) || c == '-' || c == '+' || c == '.'))
                {
                    return bad((std::string("a command the grammar has not, '") + c + "',").c_str());
                }
                else if (command == 0 || command == 'Z' || command == 'z')
                {
                    return bad("numbers with no command");
                }
                if (previous == 0 && command != 'M' && command != 'm')
                {
                    return bad("no M at the start");
                }
                const bool relative = command >= 'a' && command <= 'z';
                const char upper    = relative ? static_cast<char>(command - 'a' + 'A') : command;
                PointF     p, c1, c2;
                switch (upper)
                {
                case 'M':
                    if (!pair(p, relative))
                    {
                        return bad("an M that is not a point");
                    }
                    out.MoveTo(p);
                    start = current = control = p;
                    command                   = relative ? 'l' : 'L'; // what follows an M's point is a line
                    break;
                case 'L':
                    if (!pair(p, relative))
                    {
                        return bad("an L that is not a point");
                    }
                    out.LineTo(p);
                    current = control = p;
                    break;
                case 'H':
                case 'V': {
                    double v = 0.0;
                    if (!number(v))
                    {
                        return bad("an H or a V with no number");
                    }
                    p = current;
                    (upper == 'H' ? p.x : p.y) =
                        static_cast<float>(v) + (relative ? (upper == 'H' ? current.x : current.y) : 0.0f);
                    out.LineTo(p);
                    current = control = p;
                    break;
                }
                case 'C':
                case 'S':
                    if (upper == 'S')
                    {
                        // The first handle the last curve's second reflected, or the current point after anything else.
                        const bool after_cubic = previous == 'C' || previous == 'S';
                        c1 = after_cubic ? PointF{2.0f * current.x - control.x, 2.0f * current.y - control.y} : current;
                    }
                    else if (!pair(c1, relative))
                    {
                        return bad("a C that is not three points");
                    }
                    if (!pair(c2, relative) || !pair(p, relative))
                    {
                        return bad("a C or an S that is not its points");
                    }
                    out.CubicTo(c1, c2, p);
                    control = c2;
                    current = p;
                    break;
                case 'Q':
                case 'T':
                    if (upper == 'T')
                    {
                        const bool after_quad = previous == 'Q' || previous == 'T';
                        c1 = after_quad ? PointF{2.0f * current.x - control.x, 2.0f * current.y - control.y} : current;
                    }
                    else if (!pair(c1, relative))
                    {
                        return bad("a Q that is not two points");
                    }
                    if (!pair(p, relative))
                    {
                        return bad("a Q or a T that is not its points");
                    }
                    out.QuadTo(c1.x, c1.y, p.x, p.y);
                    control = c1;
                    current = p;
                    break;
                case 'A': {
                    double rx = 0.0, ry = 0.0, angle = 0.0;
                    bool   large = false, sweep = false;
                    if (!number(rx) || !number(ry) || !number(angle) || !flag(large) || !flag(sweep) ||
                        !pair(p, relative))
                    {
                        return bad("a malformed A");
                    }
                    ArcTo(out, current, rx, ry, angle, large, sweep, p);
                    current = control = p;
                    break;
                }
                default: // 'Z'
                    out.Close();
                    current = control = start;
                    break;
                }
                previous = upper;
            }
            if (previous == 0)
            {
                return Refuse("a <path> that draws nothing");
            }
            return true;
        }

        /** The presentation of a shape: its fill, its stroke and their opacities, inherited as the specification
            inherits them.
            @param node   the shape element
            @param shape  the shape, its outline made; its paints and stroke set
            @return       true when they are within the subset */
        bool Presentation(const Node &node, Shape &shape)
        {
            float fill_opacity = 1.0f, stroke_opacity = 1.0f;
            if (!OpacityOf(node, Inherited(node, "fill-opacity"), "fill-opacity", fill_opacity) ||
                !OpacityOf(node, Inherited(node, "stroke-opacity"), "stroke-opacity", stroke_opacity))
            {
                return false;
            }
            const std::string *fill = Inherited(node, "fill");
            if (fill == nullptr)
            {
                shape.fill = colors::kBlack.WithMultipliedAlpha(fill_opacity);
            }
            else if (!PaintOf(*fill, shape.path, fill_opacity, shape.fill))
            {
                return false;
            }
            if (const std::string *rule = Inherited(node, "fill-rule"))
            {
                if (*rule == "evenodd")
                {
                    shape.path.SetFillRule(Path::FillRule::kEvenOdd);
                }
                else if (*rule != "nonzero")
                {
                    return Refuse("a fill-rule other than nonzero and evenodd: " + *rule);
                }
            }
            if (const std::string *stroke = Inherited(node, "stroke"))
            {
                if (!PaintOf(*stroke, shape.path, stroke_opacity, shape.stroke_paint))
                {
                    return false;
                }
            }
            float width = 1.0f;
            if (const std::string *w = Inherited(node, "stroke-width"))
            {
                if (!LengthText(*w, width) || width < 0.0f)
                {
                    return Refuse("a stroke-width that is not a length of zero or more: " + *w);
                }
            }
            shape.stroke.thickness = width;
            if (!(width > 0.0f))
            {
                shape.stroke_paint.reset();
            }
            if (const std::string *join = Inherited(node, "stroke-linejoin"))
            {
                if (*join == "round")
                {
                    shape.stroke.join = Stroke::Join::kCurved;
                }
                else if (*join == "bevel")
                {
                    shape.stroke.join = Stroke::Join::kBevelled;
                }
                else if (*join != "miter")
                {
                    return Refuse("a stroke-linejoin other than miter, round and bevel: " + *join);
                }
            }
            if (const std::string *cap = Inherited(node, "stroke-linecap"))
            {
                if (*cap == "round")
                {
                    shape.stroke.cap = Stroke::Cap::kRounded;
                }
                else if (*cap == "square")
                {
                    shape.stroke.cap = Stroke::Cap::kSquare;
                }
                else if (*cap != "butt")
                {
                    return Refuse("a stroke-linecap other than butt, round and square: " + *cap);
                }
            }
            if (const std::string *limit = Inherited(node, "stroke-miterlimit"))
            {
                float value = 0.0f;
                if (!Number(*limit, value) || value < 4.0f || value > 4.0f)
                {
                    return Refuse("a stroke-miterlimit other than 4: " + *limit);
                }
            }
            if (const std::string *dashes = Inherited(node, "stroke-dasharray"); dashes != nullptr && *dashes != "none")
            {
                std::string_view   s = *dashes;
                std::vector<float> lengths;
                bool               any = false;
                for (SkipSeparators(s); !s.empty(); SkipSeparators(s))
                {
                    double v = 0.0;
                    if (!TakeNumber(s, v) || v < 0.0)
                    {
                        return Refuse("a stroke-dasharray that is not lengths of zero or more: " + *dashes);
                    }
                    lengths.push_back(static_cast<float>(v));
                    any = any || v > 0.0;
                }
                if (lengths.size() % 2 == 1)
                {
                    const std::vector<float> once = lengths;
                    lengths.insert(lengths.end(), once.begin(), once.end());
                }
                if (any)
                {
                    shape.stroke.dashes = std::move(lengths);
                }
            }
            return true;
        }

        /** A drawn element - a group, or a shape - into `steps`, under `transform`.
            @param node       the element
            @param transform  the transform in force outside it
            @param steps      the steps, appended to
            @return           true when the element is within the subset */
        bool Drawn(const Node &node, const Affine &transform, std::vector<Step> &steps)
        {
            Affine inner;
            if (node.name == "g")
            {
                return Allowed(node, {"transform"}) && TransformOf(node, transform, inner) && Group(node, inner, steps);
            }
            Shape shape;
            if (node.name == "path")
            {
                if (!Allowed(node, {"d", "transform"}) || !PathData(node, shape.path))
                {
                    return false;
                }
            }
            else if (node.name == "circle" || node.name == "ellipse")
            {
                const bool circle = node.name == "circle";
                float      cx = 0.0f, cy = 0.0f, rx = 0.0f, ry = 0.0f;
                if (!(circle ? Allowed(node, {"cx", "cy", "r", "transform"})
                             : Allowed(node, {"cx", "cy", "rx", "ry", "transform"})) ||
                    !Length(node, "cx", cx) || !Length(node, "cy", cy) || !NonNegative(node, circle ? "r" : "rx", rx) ||
                    !NonNegative(node, circle ? "r" : "ry", ry))
                {
                    return false;
                }
                if (rx > 0.0f && ry > 0.0f)
                {
                    shape.path.AddEllipse({cx - rx, cy - ry, rx * 2.0f, ry * 2.0f});
                }
            }
            else if (node.name == "rect")
            {
                if (!Allowed(node, {"x", "y", "width", "height", "rx", "ry", "transform"}) ||
                    !RectOutline(node, shape.path))
                {
                    return false;
                }
            }
            else if (node.name == "line")
            {
                float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
                if (!Allowed(node, {"x1", "y1", "x2", "y2", "transform"}) || !Length(node, "x1", x1) ||
                    !Length(node, "y1", y1) || !Length(node, "x2", x2) || !Length(node, "y2", y2))
                {
                    return false;
                }
                shape.path.MoveTo(x1, y1);
                shape.path.LineTo(x2, y2);
            }
            else if (node.name == "polyline" || node.name == "polygon")
            {
                if (!Allowed(node, {"points", "transform"}) || !PointsOutline(node, node.name == "polygon", shape.path))
                {
                    return false;
                }
            }
            else
            {
                return Refuse("<" + node.name + ">, which the reader does not draw");
            }
            if (!node.children.empty())
            {
                return Refuse("<" + node.name + "> with children");
            }
            float opacity = 1.0f;
            if (!TransformOf(node, transform, inner) || !OpacityOf(node, node.Find("opacity"), "opacity", opacity) ||
                !Presentation(node, shape))
            {
                return false;
            }
            if (node.name == "line")
            {
                shape.fill.reset(); // a line covers no area
            }
            if (shape.path.IsEmpty() || (!shape.fill && !shape.stroke_paint))
            {
                return true; // nothing to draw
            }
            shape.transform  = inner;
            const bool layer = opacity < 1.0f;
            if (layer)
            {
                steps.push_back({Step::Kind::kBeginLayer, {}, opacity});
            }
            steps.push_back({Step::Kind::kShape, std::move(shape), 1.0f});
            if (layer)
            {
                steps.push_back({Step::Kind::kEndLayer, {}, 1.0f});
            }
            return true;
        }

        std::map<std::string, GradientDef> gradients_; ///< the gradients defined, by id
    };

    std::vector<Step> steps_;          ///< the drawing, in order
    RectF             view_box_;       ///< the drawing's own units
    float             align_x_{0.5f};  ///< where the viewBox sits across a wider area: 0 left, 0.5 centred, 1 right
    float             align_y_{0.5f};  ///< where it sits down a taller area: 0 top, 0.5 centred, 1 bottom
    bool              slice_{false};   ///< true to fill the area, cutting the viewBox, rather than fit inside it
    bool              stretch_{false}; ///< true to stretch the viewBox to the area, its aspect not kept
};
} // namespace reckless::gfx
