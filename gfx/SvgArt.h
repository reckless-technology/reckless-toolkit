#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
#include "DecimalText.h"
#include "Geometry.h"
#include "Path.h"
#include "SvgSyntax.h"

namespace reckless::gfx
{
/**
    The series' backlit meters' artwork - the SVG documents the series' own scripts write, assets/meters - read
    with no toolkit and drawn on any canvas as JUCE's SVG parser (juce::Drawable) reads and draws them, so a meter shows
    the face the releases built on JUCE showed. It reads exactly the subset those files hold and refuses anything else
    with the reason, so a re-export that adds what the reader does not draw as that parser does fails the suite that
    reads the files:

    - the root `<svg>`, whose width, height and viewBox change nothing drawn: with no preserveAspectRatio the parser
      fits nothing, and the document draws in its own units;
    - in `<defs>`: `<linearGradient>` (x1, y1, x2, y2 as fractions of the bounds of the shape it fills) and
      `<radialGradient>` (cx, cy and r in the document's units, gradientUnits "userSpaceOnUse"), each a list of
      `<stop>` (offset, stop-color, stop-opacity); `<clipPath>` holding rectangles; and `<filter>` holding
      `<feGaussianBlur>`, which the parser never draws, and nor does this;
    - `<g>` (clip-path, transform "rotate(angle)" or "rotate(angle cx cy)", fill, stroke, stroke-width, stroke-linecap,
      and the text attributes a group with no text may carry, which change nothing);
    - `<rect>` (x, y, width, height, rx), `<circle>` (cx, cy, r), `<line>` (x1, y1, x2, y2) and `<path>` (d of
      absolute M, L, A and Z), each with fill, stroke, stroke-width, stroke-linecap, stroke-opacity, opacity and a
      filter, ignored;
    - colours `#rgb` and `#rrggbb`, `none`, and `url(#id)` of a gradient; plain numbers; an id on anything; comments.

    And as the parser draws them: every outline is taken into the document's units through its groups' transforms,
    and a stroke's width with it, scaled by the root of the transform's determinant; an attribute a shape lacks is
    the nearest group's that has it, a clip-path among them, so each shape under a clipped group is clipped on its
    own, the clip taken through the shape's own transforms - and a group itself never clips; a shape with no fill is
    filled black where its outline closes and not at all where it does not; a fill's alpha is times the opacity, a
    stroke's times the opacity and the stroke-opacity, a gradient's at every stop; a gradient's first stop, when it
    sits past the start, is moved to the start - the parser puts a colour at the start by replacing the first one -
    and its last, short of the end, is repeated at it, so the meters' vignette darkens from the centre; an arc's
    endpoints become its centre and angles by the parser's own arithmetic, the outline carried on to the arc's end with
    a line; an outline that ends where it began is closed. Each shape is filled, then stroked, in the document's order.
*/
class SvgArt : private SvgSyntax
{
  public:
    /** The document in `text`, or none - with what refused it in `error`, where one is given - for a document outside
        the subset or not well formed.
        @param text   the document, as its bytes read as text
        @param error  where the reason is put when the document is refused; nullptr for none wanted
        @return       the document, or nothing for one outside the subset or not well formed */
    static std::optional<SvgArt> Parse(std::string_view text, std::string *error = nullptr)
    {
        SvgArt art;
        Reader reader;
        if (!reader.Read(text, art.items_))
        {
            if (error != nullptr)
            {
                *error = reader.error;
            }
            return std::nullopt;
        }
        return art;
    }

    /** The document on `c` through `t`, its units onto the canvas's; the canvas's state as it was afterwards.
        @param c  the canvas drawn on, which is left in the state it was in
        @param t  the transform from the document's units onto the canvas's */
    void Draw(Canvas &c, const Affine &t) const
    {
        const ScopedState state(c);
        c.AddTransform(t);
        for (const Item &item : items_)
        {
            if (item.clip)
            {
                const ScopedState clipped(c);
                c.ClipToPath(*item.clip);
                DrawShape(c, item);
            }
            else
            {
                DrawShape(c, item);
            }
        }
    }

    /** How many shapes the document draws, and how many of them under a clip.
        @return the number of shapes */
    int Shapes() const noexcept
    {
        return static_cast<int>(items_.size());
    }

    /** How many of the document's shapes are drawn under a clip.
        @return the number of clipped shapes */
    int ClippedShapes() const noexcept
    {
        return static_cast<int>(
            std::count_if(items_.begin(), items_.end(), [](const Item &i) { return i.clip.has_value(); }));
    }

  private:
    /** A fill or a stroke's paint: a solid colour or a gradient. */
    using Paint = std::variant<Color, Gradient>;

    /** A shape as it is drawn: its outline in the document's units, its fill and its stroke, and its clip. */
    struct Item
    {
        Path                 path;         ///< the outline, in the document's units
        std::optional<Paint> fill;         ///< the fill; none for no fill
        std::optional<Paint> stroke_paint; ///< the stroke's paint; none for no stroke
        Stroke               stroke;       ///< the stroke's width, cap and joins
        std::optional<Path>  clip; ///< the clip the shape is drawn under, in the document's units; none for no clip
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

    /** Draw a shape: its fill, then its stroke.
        @param c     the canvas, its transform and clip already set
        @param item  the shape */
    static void DrawShape(Canvas &c, const Item &item)
    {
        if (item.fill)
        {
            SetPaint(c, *item.fill);
            c.FillPath(item.path);
        }
        if (item.stroke_paint)
        {
            SetPaint(c, *item.stroke_paint);
            c.StrokePath(item.path, item.stroke);
        }
    }

    // ---- The reader ------------------------------------------------------------------------------------------------

    /** A gradient as the parser holds one before it meets the shape it fills: its geometry and its stops, in order. */
    struct GradientDef
    {
        bool  radial{false}; ///< true for a radial gradient, false for a linear one
        float x1{0.0f},      ///< the linear gradient's start x, as a fraction of the bounds of the shape it fills
            y1{0.0f},        ///< its start y, as a fraction of the bounds
            x2{1.0f},        ///< its end x, as a fraction of the bounds
            y2{0.0f};        ///< its end y, as a fraction of the bounds
        float cx{0.0f},      ///< the radial gradient's centre x, in the document's units
            cy{0.0f},        ///< its centre y, in the document's units
            r{0.0f};         ///< its radius, in the document's units
        std::vector<std::pair<double, Color>> stops; ///< the stops, as an offset from 0 to 1 and a colour, in order
    };

    /** Reads the document's text into the shapes it draws: the XML, then the definitions, then the drawing,
        refusing what it does not draw as the parser does. */
    class Reader
    {
      public:
        std::string error; ///< the reason the last Read() refused, or empty

        /** Read a document.
            @param text   the document
            @param items  the shapes it draws, in order, appended to
            @return       true when the document is within the subset; else false with `error` set */
        bool Read(std::string_view text, std::vector<Item> &items)
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
            float unused = 0.0f;
            if (!Allow(root, {"xmlns", "width", "height", "viewBox", "id"}) || !Length(root, "width", unused) ||
                !Length(root, "height", unused) || !ViewBox(root))
            {
                return false;
            }
            // The definitions first, since the shapes name them.
            for (const Node &child : root.children)
            {
                if (child.name == "defs" && !Defs(child))
                {
                    return false;
                }
            }
            for (const Node &child : root.children)
            {
                if (child.name != "defs" && !Drawn(child, Affine{}, items))
                {
                    return false;
                }
            }
            return true;
        }

      private:
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

        /** Every attribute of `node` among `allowed`, and the first that is not refused.
            @param node     the element
            @param allowed  the attribute names it may carry
            @return         true when every attribute is allowed; else false with `error` set */
        bool Allow(const Node &node, std::initializer_list<std::string_view> allowed)
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

        /** An attribute that is a plain number, `otherwise` where it is absent.
            @param node       the element
            @param key        the attribute's name
            @param out        the number, in the document's units; set to `otherwise` when absent
            @param otherwise  the value for an absent attribute
            @return           true when the attribute is absent or a plain number */
        bool Length(const Node &node, std::string_view key, float &out, float otherwise = 0.0f)
        {
            const std::string *text = node.Find(key);
            if (text == nullptr)
            {
                out = otherwise;
                return true;
            }
            if (!Number(*text, out))
            {
                return Refuse("<" + node.name + ">'s " + std::string(key) + " is not a plain number: " + *text);
            }
            return true;
        }

        /** An attribute that must be there, a plain number.
            @param node  the element
            @param key   the attribute's name
            @param out   the number, in the document's units
            @return      true when the attribute is there and a plain number */
        bool Required(const Node &node, std::string_view key, float &out)
        {
            if (node.Find(key) == nullptr)
            {
                return Refuse("<" + node.name + "> with no " + std::string(key));
            }
            return Length(node, key, out);
        }

        /** Check the root's viewBox, which changes nothing drawn.
            @param root  the root element
            @return      true when it has none or four numbers */
        bool ViewBox(const Node &root)
        {
            const std::string *box = root.Find("viewBox");
            if (box == nullptr)
            {
                return true;
            }
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
            if (!text.empty())
            {
                return Refuse("a viewBox that is not four numbers: " + *box);
            }
            return true;
        }

        /** Read a definitions element: its gradients and clips are kept by id, its filters checked.
            @param defs  the element
            @return      true when everything in it is within the subset */
        bool Defs(const Node &defs)
        {
            if (!Allow(defs, {"id"}))
            {
                return false;
            }
            for (const Node &child : defs.children)
            {
                const std::string *id = child.Find("id");
                if (child.name == "linearGradient" || child.name == "radialGradient")
                {
                    GradientDef def;
                    if (id == nullptr)
                    {
                        return Refuse("<" + child.name + "> with no id");
                    }
                    if (!GradientOf(child, def))
                    {
                        return false;
                    }
                    gradients_[*id] = std::move(def);
                }
                else if (child.name == "clipPath")
                {
                    if (id == nullptr)
                    {
                        return Refuse("<clipPath> with no id");
                    }
                    if (!Allow(child, {"id"}))
                    {
                        return false;
                    }
                    Path outline;
                    if (!ClipOutline(child, outline))
                    {
                        return false;
                    }
                    clips_[*id] = std::move(outline);
                }
                else if (child.name == "filter")
                {
                    // Never drawn by the parser: what it holds is checked, and it is left alone.
                    if (!Allow(child, {"id", "x", "y", "width", "height"}))
                    {
                        return false;
                    }
                    for (const Node &effect : child.children)
                    {
                        if (effect.name != "feGaussianBlur")
                        {
                            return Refuse("<filter> with <" + effect.name + ">");
                        }
                        if (!Allow(effect, {"stdDeviation"}))
                        {
                            return false;
                        }
                        if (!effect.children.empty())
                        {
                            return Refuse("<feGaussianBlur> with children");
                        }
                    }
                }
                else
                {
                    return Refuse("<defs> with <" + child.name + ">");
                }
            }
            return true;
        }

        /** Read a gradient's geometry and stops.
            @param node  the linear or radial gradient element
            @param def   the gradient, filled in
            @return      true when it is within the subset */
        bool GradientOf(const Node &node, GradientDef &def)
        {
            def.radial = node.name == "radialGradient";
            if (def.radial)
            {
                if (!Allow(node, {"id", "cx", "cy", "r", "gradientUnits"}))
                {
                    return false;
                }
                const std::string *units = node.Find("gradientUnits");
                if (units == nullptr || *units != "userSpaceOnUse")
                {
                    return Refuse("a <radialGradient> not in userSpaceOnUse");
                }
                if (!Required(node, "cx", def.cx) || !Required(node, "cy", def.cy) || !Required(node, "r", def.r))
                {
                    return false;
                }
            }
            else if (!Allow(node, {"id", "x1", "y1", "x2", "y2"}) || !Length(node, "x1", def.x1, 0.0f) ||
                     !Length(node, "y1", def.y1, 0.0f) || !Length(node, "x2", def.x2, 1.0f) ||
                     !Length(node, "y2", def.y2, 0.0f))
            {
                return false;
            }
            for (const Node &stop : node.children)
            {
                if (stop.name != "stop")
                {
                    return Refuse("<" + node.name + "> with <" + stop.name + ">");
                }
                if (!Allow(stop, {"offset", "stop-color", "stop-opacity"}))
                {
                    return false;
                }
                if (!stop.children.empty())
                {
                    return Refuse("<stop> with children");
                }
                const std::string   *colour_text = stop.Find("stop-color");
                std::optional<Color> colour =
                    colour_text != nullptr ? HexColour(*colour_text) : std::optional<Color>(colors::kBlack);
                if (!colour)
                {
                    return Refuse("a stop-color that is not #rgb or #rrggbb: " + *colour_text);
                }
                float opacity = 1.0f;
                if (!Length(stop, "stop-opacity", opacity, 1.0f))
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
                AddStop(def.stops, std::clamp(offset, 0.0f, 1.0f),
                        colour->WithMultipliedAlpha(std::clamp(opacity, 0.0f, 1.0f)));
            }
            if (def.stops.empty())
            {
                return Refuse("<" + node.name + "> with no stops");
            }
            // The ends as the parser makes them: a first stop past the start set at the start - replacing it - and a
            // last short of the end repeated at the end.
            if (def.stops.front().first > 0.0)
            {
                AddStop(def.stops, 0.0, def.stops.front().second);
            }
            if (def.stops.back().first < 1.0)
            {
                AddStop(def.stops, 1.0, def.stops.back().second);
            }
            return true;
        }

        /** A colour at `position`, as the parser's gradient adds one: at the start, the first replaced; anywhere else,
            inserted before the first stop past it.
            @param stops     the stops so far, in order of offset
            @param position  the offset, 0 to 1; 0 or less replaces the first
            @param colour    the stop's colour */
        static void AddStop(std::vector<std::pair<double, Color>> &stops, double position, Color colour)
        {
            if (position <= 0.0)
            {
                if (stops.empty())
                {
                    stops.emplace_back(0.0, colour);
                }
                else
                {
                    stops.front() = {0.0, colour};
                }
                return;
            }
            const double at = std::min(1.0, position);
            const auto   it = std::find_if(stops.begin(), stops.end(), [at](const auto &s) { return s.first > at; });
            stops.insert(it, {at, colour});
        }

        /** A clip-path's rectangles as one outline, untransformed.
            @param clip  the clip-path element
            @param out   the rectangles added to it
            @return      true when it holds rectangles alone */
        bool ClipOutline(const Node &clip, Path &out)
        {
            if (clip.children.empty())
            {
                return Refuse("an empty <clipPath>");
            }
            for (const Node &rect : clip.children)
            {
                if (rect.name != "rect")
                {
                    return Refuse("<clipPath> with <" + rect.name + ">");
                }
                if (!Allow(rect, {"x", "y", "width", "height", "rx", "id"}) || !RectOutline(rect, out))
                {
                    return false;
                }
                if (!rect.children.empty())
                {
                    return Refuse("<rect> with children");
                }
            }
            return true;
        }

        /** A rectangle element's outline: rounded where it has an rx.
            @param node  the element
            @param out   the outline, added to
            @return      true when its numbers are plain */
        bool RectOutline(const Node &node, Path &out)
        {
            float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f, rx = 0.0f;
            if (!Length(node, "x", x) || !Length(node, "y", y) || !Length(node, "width", w) ||
                !Length(node, "height", h) || !Length(node, "rx", rx))
            {
                return false;
            }
            if (node.Find("rx") != nullptr)
            {
                out.AddRoundedRect({x, y, w, h}, rx);
            }
            else
            {
                out.AddRect({x, y, w, h});
            }
            return true;
        }

        /** An attribute of `node` or of its nearest ancestor that has it, as the parser looks one up.
            @param node  the element
            @param key   the attribute's name
            @return     the value, valid as long as the document's nodes; nullptr when none has it */
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

        /** An opacity attribute, inherited as the parser looks it up.
            @param node  the element
            @param key   the attribute's name
            @param out   the opacity, held to 0 to 1; 1 when absent
            @return      true when absent or a number */
        bool Opacity(const Node &node, std::string_view key, float &out)
        {
            out                     = 1.0f;
            const std::string *text = Inherited(node, key);
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

        /** `text` - a fill's or a stroke's - as the paint for the outline `path`, at `opacity`: none, a colour, or a
            gradient placed on the outline's bounds or in the document's units.
            @param text     the fill or stroke attribute
            @param path     the outline the paint is for
            @param opacity  the alpha it is multiplied by, 0 to 1
            @param out      the paint; none for `none`
            @return         true when the text is within the subset */
        bool PaintOf(const std::string &text, const Path &path, float opacity, std::optional<Paint> &out)
        {
            out.reset();
            if (text == "none")
            {
                return true;
            }
            if (const auto colour = HexColour(text))
            {
                out = colour->WithMultipliedAlpha(opacity);
                return true;
            }
            const auto it = gradients_.find(UrlId(text));
            if (it == gradients_.end())
            {
                return Refuse("a paint that is not #rgb, #rrggbb, none or a gradient's url: " + text);
            }
            const GradientDef                    &def   = it->second;
            std::vector<std::pair<double, Color>> stops = def.stops;
            if (opacity < 1.0f)
            {
                for (auto &stop : stops)
                {
                    stop.second = stop.second.WithMultipliedAlpha(opacity);
                }
            }
            Gradient g;
            if (def.radial)
            {
                g.radial = true;
                g.start  = {def.cx, def.cy};
                g.end    = {def.cx + def.r, def.cy};
            }
            else
            {
                const RectF bounds = path.Bounds();
                g.start            = {bounds.x() + bounds.width() * def.x1, bounds.y() + bounds.height() * def.y1};
                g.end              = {bounds.x() + bounds.width() * def.x2, bounds.y() + bounds.height() * def.y2};
                if (Same(g.start, g.end))
                {
                    out = stops.back().second; // no direction: the last colour, as the parser fills it
                    return true;
                }
            }
            g.start_color = stops.front().second;
            g.end_color   = stops.back().second;
            for (std::size_t i = 1; i + 1 < stops.size(); ++i)
            {
                g.stops.emplace_back(static_cast<float>(stops[i].first), stops[i].second);
            }
            out = std::move(g);
            return true;
        }

        /** `node`'s transform followed by `outer`: none, rotate(angle) or rotate(angle cx cy), as the parser makes
            it.
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
            SkipSeparators(t);
            while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\n' || t.back() == '\r'))
            {
                t.remove_suffix(1);
            }
            if (t.size() < 8 || t.substr(0, 7) != "rotate(" || t.back() != ')')
            {
                return Refuse("a transform other than one rotate: " + *text);
            }
            t.remove_prefix(7);
            t.remove_suffix(1);
            float numbers[3]{};
            int   count = 0;
            for (SkipSeparators(t); !t.empty(); SkipSeparators(t))
            {
                double n = 0.0;
                if (count == 3 || !TakeNumber(t, n))
                {
                    return Refuse("a rotate that is not one or three numbers: " + *text);
                }
                numbers[count++] = static_cast<float>(n);
            }
            if (count != 1 && count != 3)
            {
                return Refuse("a rotate that is not one or three numbers: " + *text);
            }
            const float radians = numbers[0] * (std::numbers::pi_v<float> / 180.0f);
            out                 = Affine::Rotation(radians, numbers[1], numbers[2]).FollowedBy(outer);
            return true;
        }

        /** The path data: absolute M, L, A and Z, walked as the parser walks it; whether the outline closes.
            @param node    the path element
            @param out     the outline, added to
            @param closed  set true when the outline is closed
            @return        true when the data is within the subset */
        bool PathData(const Node &node, Path &out, bool &closed)
        {
            const std::string *d = node.Find("d");
            if (d == nullptr)
            {
                return Refuse("a <path> with no d");
            }
            std::string_view s = *d;
            PointF           last{}, start{};
            char             command  = 0;
            bool             any      = false;
            bool             at_close = false;
            const auto       point    = [&s](PointF &p) {
                double x = 0.0, y = 0.0;
                SkipSeparators(s);
                if (!TakeNumber(s, x))
                {
                    return false;
                }
                SkipSeparators(s);
                if (!TakeNumber(s, y))
                {
                    return false;
                }
                p = {static_cast<float>(x), static_cast<float>(y)};
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
            for (SkipSeparators(s); !s.empty(); SkipSeparators(s))
            {
                const char c = s.front();
                if (c == 'M' || c == 'L' || c == 'A' || c == 'Z')
                {
                    command = c;
                    s.remove_prefix(1);
                }
                else if (!(Digit(c) || c == '-' || c == '+' || c == '.') || (command != 'L' && command != 'A'))
                {
                    // A command outside the four, or numbers after a command that does not repeat: after an M the
                    // parser draws lines, and after a Z moves, which the files never ask of it.
                    return Refuse(std::string("path data outside absolute M, L, A and Z at '") + c + "' in: " + *d);
                }
                if (command == 'Z')
                {
                    out.Close();
                    last     = start;
                    closed   = true;
                    at_close = true;
                    continue;
                }
                at_close = false;
                if (command != 'M' && !any)
                {
                    return Refuse("path data that does not start with an M: " + *d);
                }
                if (command == 'M' || command == 'L')
                {
                    PointF p;
                    if (!point(p))
                    {
                        return Refuse(std::string("an ") + command + " that is not a point in: " + *d);
                    }
                    if (command == 'M')
                    {
                        start = p;
                        out.MoveTo(p);
                        command = 0;
                    }
                    else
                    {
                        out.LineTo(p);
                    }
                    last = p;
                    any  = true;
                    continue;
                }
                // An arc: its radii, its rotation, its two flags and its end, centred as the parser centres it.
                PointF radii, end;
                double degrees = 0.0;
                bool   large = false, sweep = false;
                if (!point(radii))
                {
                    return Refuse("an A with no radii in: " + *d);
                }
                SkipSeparators(s);
                if (!TakeNumber(s, degrees) || !flag(large) || !flag(sweep) || !point(end))
                {
                    return Refuse("a malformed A in: " + *d);
                }
                const float angle = static_cast<float>(degrees) * (std::numbers::pi_v<float> / 180.0f);
                if (!Same(last, end))
                {
                    double rx = radii.x, ry = radii.y, cx = 0.0, cy = 0.0, from = 0.0, delta = 0.0;
                    EndpointToCentre(last.x, last.y, end.x, end.y, angle, large, sweep, rx, ry, cx, cy, from, delta);
                    out.AddCentredArc(static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(rx),
                                      static_cast<float>(ry), angle, static_cast<float>(from),
                                      static_cast<float>(from + delta), false);
                    out.LineTo(end);
                }
                last = end;
            }
            if (!any)
            {
                return Refuse("a <path> that draws nothing");
            }
            // An outline that ends where it began is closed, as the parser closes it.
            if (!at_close && Same(last, start))
            {
                out.Close();
                closed = true;
            }
            return true;
        }

        /** An SVG arc's endpoints as its centre, radii and angles - the start clockwise from twelve o'clock, and the
            sweep - by the parser's own arithmetic, in double.
            @param x1        the start's x
            @param y1        the start's y
            @param x2        the end's x
            @param y2        the end's y
            @param angle     the ellipse's rotation, in radians
            @param large     the large-arc flag
            @param sweep     the sweep flag
            @param rx        the horizontal radius, scaled up when the endpoints are too far apart
            @param ry        the vertical radius, scaled up with it
            @param centre_x  the centre's x
            @param centre_y  the centre's y
            @param start     the start angle, in radians clockwise from twelve o'clock
            @param delta     the sweep, in radians, signed */
        static void EndpointToCentre(double x1, double y1, double x2, double y2, double angle, bool large, bool sweep,
                                     double &rx, double &ry, double &centre_x, double &centre_y, double &start,
                                     double &delta)
        {
            constexpr double kTwoPi = 2.0 * std::numbers::pi;
            const double     mid_x  = (x1 - x2) * 0.5;
            const double     mid_y  = (y1 - y2) * 0.5;
            const double     cos_a  = std::cos(angle);
            const double     sin_a  = std::sin(angle);
            const double     xp     = cos_a * mid_x + sin_a * mid_y;
            const double     yp     = cos_a * mid_y - sin_a * mid_x;
            const double     xp2    = xp * xp;
            const double     yp2    = yp * yp;
            const double     rx2    = rx * rx;
            const double     ry2    = ry * ry;
            const double     s      = (xp2 / rx2) + (yp2 / ry2);
            double           c      = 0.0;
            if (s <= 1.0)
            {
                c = std::sqrt(std::max(0.0, ((rx2 * ry2) - (rx2 * yp2) - (ry2 * xp2)) / ((rx2 * yp2) + (ry2 * xp2))));
                if (large == sweep)
                {
                    c = -c;
                }
            }
            else
            {
                const double s2 = std::sqrt(s);
                rx *= s2;
                ry *= s2;
            }
            const double cpx = ((rx * yp) / ry) * c;
            const double cpy = ((-ry * xp) / rx) * c;
            centre_x         = ((x1 + x2) * 0.5) + (cos_a * cpx) - (sin_a * cpy);
            centre_y         = ((y1 + y2) * 0.5) + (sin_a * cpx) + (cos_a * cpy);
            const double ux  = (xp - cpx) / rx;
            const double uy  = (yp - cpy) / ry;
            const double vx  = (-xp - cpx) / rx;
            const double vy  = (-yp - cpy) / ry;
            const double len = std::hypot(ux, uy);
            start            = std::acos(std::clamp(ux / len, -1.0, 1.0));
            if (uy < 0)
            {
                start = -start;
            }
            start += std::numbers::pi / 2.0;
            delta = std::acos(std::clamp(((ux * vx) + (uy * vy)) / (len * std::hypot(vx, vy)), -1.0, 1.0));
            if ((ux * vy) - (uy * vx) < 0)
            {
                delta = -delta;
            }
            if (sweep && delta < 0)
            {
                delta += kTwoPi;
            }
            else if (!sweep && delta > 0)
            {
                delta -= kTwoPi;
            }
            delta = std::fmod(delta, kTwoPi);
        }

        /** A drawn element - a group, or a shape - into `items`, under `transform`.
            @param node       the element
            @param transform  the transform in force outside it
            @param items      the shapes, appended to
            @return           true when the element is within the subset */
        bool Drawn(const Node &node, const Affine &transform, std::vector<Item> &items)
        {
            if (node.name == "g")
            {
                if (!Allow(node, {"clip-path", "transform", "fill", "stroke", "stroke-width", "stroke-linecap",
                                  "font-family", "font-weight", "font-size", "text-anchor", "id"}))
                {
                    return false;
                }
                Affine inner;
                if (!TransformOf(node, transform, inner))
                {
                    return false;
                }
                for (const Node &child : node.children)
                {
                    if (!Drawn(child, inner, items))
                    {
                        return false;
                    }
                }
                return true;
            }

            static constexpr std::string_view kPaint[] = {"fill",           "stroke",  "stroke-width", "stroke-linecap",
                                                          "stroke-opacity", "opacity", "filter",       "id"};
            const auto                        allow    = [this, &node](std::initializer_list<std::string_view> own) {
                for (const auto &[key, value] : node.attributes)
                {
                    if (std::find(own.begin(), own.end(), key) == own.end() &&
                        std::find(std::begin(kPaint), std::end(kPaint), key) == std::end(kPaint))
                    {
                        return Refuse("<" + node.name + "> with " + key);
                    }
                }
                return true;
            };
            Item shape;
            bool closed = false;
            if (node.name == "rect")
            {
                if (!allow({"x", "y", "width", "height", "rx"}) || !RectOutline(node, shape.path))
                {
                    return false;
                }
                closed = true;
            }
            else if (node.name == "circle")
            {
                float cx = 0.0f, cy = 0.0f, r = 0.0f;
                if (!allow({"cx", "cy", "r"}) || !Length(node, "cx", cx) || !Length(node, "cy", cy) ||
                    !Length(node, "r", r))
                {
                    return false;
                }
                shape.path.AddEllipse({cx - r, cy - r, r * 2.0f, r * 2.0f});
                closed = true;
            }
            else if (node.name == "line")
            {
                float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
                if (!allow({"x1", "y1", "x2", "y2"}) || !Length(node, "x1", x1) || !Length(node, "y1", y1) ||
                    !Length(node, "x2", x2) || !Length(node, "y2", y2))
                {
                    return false;
                }
                shape.path.MoveTo(x1, y1);
                shape.path.LineTo(x2, y2);
            }
            else if (node.name == "path")
            {
                if (!allow({"d"}) || !PathData(node, shape.path, closed))
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
            if (!transform.IsIdentity())
            {
                shape.path.ApplyTransform(transform);
            }

            float opacity = 1.0f, stroke_opacity = 1.0f;
            if (!Opacity(node, "opacity", opacity) || !Opacity(node, "stroke-opacity", stroke_opacity))
            {
                return false;
            }
            // The fill: the shape's or its nearest group's, else black for a closed outline and nothing for an open
            // one. A line's fill, which has no area to cover, is left out.
            if (const std::string *fill = Inherited(node, "fill"))
            {
                if (!PaintOf(*fill, shape.path, opacity, shape.fill))
                {
                    return false;
                }
            }
            else if (closed)
            {
                shape.fill = colors::kBlack.WithMultipliedAlpha(opacity);
            }
            if (node.name == "line")
            {
                shape.fill.reset();
            }
            // The stroke, its width scaled by the transform as the parser scales it; none at no width.
            const std::string *stroke = Inherited(node, "stroke");
            if (stroke != nullptr && !PaintOf(*stroke, shape.path, opacity * stroke_opacity, shape.stroke_paint))
            {
                return false;
            }
            float width = 1.0f;
            if (const std::string *w = Inherited(node, "stroke-width"))
            {
                if (!Number(*w, width))
                {
                    return Refuse("a stroke-width that is not a plain number: " + *w);
                }
            }
            shape.stroke.thickness =
                std::sqrt(std::abs((transform.m00 * transform.m11) - (transform.m01 * transform.m10))) * width;
            if (!(shape.stroke.thickness > 0.0f))
            {
                shape.stroke_paint.reset();
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
            // The clip a shape takes from its nearest clipped group, through the shape's own transforms.
            if (const std::string *clip = Inherited(node, "clip-path"))
            {
                const auto it = clips_.find(UrlId(*clip));
                if (it == clips_.end())
                {
                    return Refuse("a clip-path that names no <clipPath>: " + *clip);
                }
                shape.clip = it->second;
                if (!transform.IsIdentity())
                {
                    shape.clip->ApplyTransform(transform);
                }
            }
            items.push_back(std::move(shape));
            return true;
        }

        std::map<std::string, GradientDef> gradients_; ///< the gradients defined, by id
        std::map<std::string, Path>        clips_;     ///< the clip outlines defined, by id
    };

    std::vector<Item> items_; ///< the shapes, in drawing order
};
} // namespace reckless::gfx
