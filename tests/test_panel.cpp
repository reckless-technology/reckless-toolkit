// The series' panel on the views (panel/): the chrome laid out and drawn where the panels of the releases built
// on JUCE lay it out and draw it, the knob bank's grid, the knobs and plates added by parameter id in its order, a knob
// the panel places itself, the knob's dial the film strip's frame at the slider's angle, the header lamp on its strip,
// the frame's seconds and the window's word that the panel is on a screen, the small level meter's reading and needle,
// the gain-reduction meter's needle in seconds, its repaints and the format's layers round it, the level bargraph's
// bars and held lines, its repaints and its geometry, the PRESETS plate on a preset model - its text, its menu, its
// questions and its files - and Reset to defaults alone where a format brings no presets; the log frequency frame's
// axis, screen and figures; the spectrum waterfall's oblique projection and its one step a slice; and the clip
// indicators' bay - its cells laid over the panel and keeping no mouse, a tube's filament and a lens's colour, each
// repainting only its own light, and an indicator left out painting nothing and coming back as it would have been.
// The images are sizes with no pixels, through the context's functions, as the look's suite draws on an image
// backend that keeps none. The tone stack's schematic and a host's parameter as a control, which stand on the
// plugins' host layer, are test_plugin_panel.cpp's.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "ArtworkMeterView.h"
#include "FrameSteps.h"
#include "IndicatorBay.h"
#include "KnobLayout.h"
#include "LampView.h"
#include "LevelBarsView.h"
#include "LevelMeterView.h"
#include "LogFrequencyFrame.h"
#include "LookBallistics.h"
#include "LookMetrics.h"
#include "PanelStyle.h"
#include "SeriesPanel.h"
#include "ShellView.h"

namespace
{
namespace gfx   = reckless::gfx;
namespace view  = reckless::view;
namespace look  = reckless::look;
namespace panel = reckless::panel;
using gfx::Canvas;
using gfx::Color;
using gfx::PointF;
using gfx::RectF;
using gfx::RectI;

// ---- Images with sizes and no pixels ----------------------------------------------------------------------------

class Pixels final : public gfx::Image::Data
{
  public:
    Pixels(int w, int h) : w_(w), h_(h)
    {
    }

    int width() const noexcept override
    {
        return w_;
    }

    int height() const noexcept override
    {
        return h_;
    }

  private:
    int w_;
    int h_;
};

gfx::Image Sized(int w, int h)
{
    return gfx::Image(std::make_shared<Pixels>(w, h));
}

/** An asset whose "file" is its picture's size, two ints. */
struct Asset
{
    std::int32_t w;
    std::int32_t h;
};

const Asset kStrip{64, 64 * 128};
const Asset kMark{512, 512};
const Asset kPlate{1264, 476};

std::span<const std::uint8_t> BytesOf(const Asset &asset)
{
    return {reinterpret_cast<const std::uint8_t *>(&asset), sizeof(Asset)};
}

// ---- Controls ---------------------------------------------------------------------------------------------------

class FakeParameter final : public view::ParameterControl
{
  public:
    explicit FakeParameter(double position, double default_position = 0.5, int choices = 0)
        : position_(position), default_(default_position), choices_(choices)
    {
    }

    std::vector<std::string> log;

    double Normalized() const override
    {
        return position_;
    }

    double DefaultNormalized() const override
    {
        return default_;
    }

    std::string Text() const override
    {
        return std::to_string(static_cast<int>(position_ * 100.0 + 0.5)) + " %";
    }

    void BeginGesture() override
    {
        log.push_back("begin");
    }

    void SetNormalized(double value) override
    {
        position_ = value;
        log.push_back("set");
    }

    void EndGesture() override
    {
        log.push_back("end");
    }

    int ChoiceCount() const override
    {
        return choices_;
    }

    std::string ChoiceText(int index) const override
    {
        return "Choice " + std::to_string(index);
    }

  private:
    double position_;
    double default_;
    int    choices_;
};

// ---- A canvas that records what the chrome draws ----------------------------------------------------------------

class RecordingCanvas final : public Canvas
{
  public:
    struct Image
    {
        int   w;
        int   h;
        RectF dest;
        bool  high;
    };

    std::vector<Image>       images;
    std::vector<std::string> texts;
    int                      paths{0}; ///< the paths filled
    std::vector<RectF>       rounded;
    std::vector<Color>       rounded_colours;
    std::vector<Color>       ellipse_colours; ///< every FillEllipse's colour, the last SetColor's
    int                      vertical_lines{0};
    bool                     high{false};

    void Save() override
    {
        stack_.push_back(dx_);
    }

    void Restore() override
    {
        dx_ = stack_.back();
        stack_.pop_back();
    }

    void AddTransform(const gfx::Affine &t) override
    {
        dx_ += t.m02;
    }

    bool ClipToRect(RectI) override
    {
        return true;
    }

    void ClipToPath(const gfx::Path &, const gfx::Affine &) override
    {
    }

    RectI ClipBounds() const override
    {
        return {0, 0, 10000, 10000};
    }

    float PhysicalPixelScale() const override
    {
        return 1.0f;
    }

    void SetColor(Color c) override
    {
        colour_ = c;
    }

    void SetGradient(const gfx::Gradient &) override
    {
    }

    void SetOpacity(float) override
    {
    }

    void SetFont(const gfx::Font &f) override
    {
        font_ = f;
    }

    void SetResampling(gfx::Resampling r) override
    {
        high = high || r == gfx::Resampling::kHigh;
    }

    const gfx::Font &CurrentFont() const override
    {
        return font_;
    }

    void FillAll() override
    {
    }

    void FillAll(Color) override
    {
    }

    void FillRect(RectI) override
    {
    }

    void FillRect(RectF) override
    {
    }

    void DrawRect(RectF, float) override
    {
    }

    void FillRoundedRect(RectF, float) override
    {
    }

    void DrawRoundedRect(RectF r, float, float) override
    {
        rounded.push_back(r);
        rounded_colours.push_back(colour_);
    }

    void FillEllipse(RectF) override
    {
        ellipse_colours.push_back(colour_);
    }

    void DrawEllipse(RectF, float) override
    {
    }

    void DrawLine(PointF, PointF, float) override
    {
    }

    void DrawHorizontalLine(int, float, float) override
    {
    }

    void DrawVerticalLine(int, float, float) override
    {
        ++vertical_lines;
    }

    void FillPath(const gfx::Path &, const gfx::Affine &) override
    {
        ++paths;
    }

    void StrokePath(const gfx::Path &, const gfx::Stroke &, const gfx::Affine &) override
    {
    }

    void DrawText(std::string_view text, RectF, gfx::Justify, bool) override
    {
        texts.emplace_back(text);
    }

    void DrawFittedText(std::string_view text, RectI, gfx::Justify, int, float) override
    {
        texts.emplace_back(text);
    }

    float TextWidth(const gfx::Font &f, std::string_view text) const override
    {
        return 0.5f * f.height * static_cast<float>(text.size());
    }

    float Ascent(const gfx::Font &f) const override
    {
        return f.height * 0.8f;
    }

    float Descent(const gfx::Font &f) const override
    {
        return f.height * 0.2f;
    }

    void DrawImage(const gfx::Image &image, RectF dest, bool) override
    {
        images.push_back({image.width(), image.height(), dest.Translated(dx_, 0.0f), high});
    }

    void DrawImageTransformed(const gfx::Image &, const gfx::Affine &) override
    {
    }

    void BeginTransparencyLayer(float) override
    {
    }

    void EndTransparencyLayer() override
    {
    }

  private:
    float              dx_{0.0f};
    std::vector<float> stack_;
    gfx::Font          font_;
    Color              colour_;
};

// ---- A window: popups held, the loop's calls queued, a file dialog answered ----------------------------------------

class FakeWindow final : public view::Host
{
  public:
    struct Opened final : view::Popup
    {
        view::View *content;

        bool Contains(gfx::PointI) const override
        {
            return false;
        }
    };

    std::vector<std::function<void()>> queue;
    std::vector<view::View *>          popups;
    std::string                        picked{"/presets/Lead.json"};
    std::vector<view::FileRequest>     requests;
    std::vector<RectI>                 invalid;
    float                              zoom{1.0f};

    std::unique_ptr<view::Popup> OpenPopup(view::View &content, gfx::PointI, std::function<void(gfx::PointI)>,
                                           bool) override
    {
        popups.push_back(&content);
        auto popup     = std::make_unique<Opened>();
        popup->content = &content;
        return popup;
    }

    void Invalidate(RectI area) override
    {
        invalid.push_back(area);
    }

    void CallSoon(std::function<void()> call) override
    {
        queue.push_back(std::move(call));
    }

    void ChooseFile(const view::FileRequest &request, std::function<void(const std::string &)> answer) override
    {
        requests.push_back(request);
        answer(picked);
    }

    float Zoom() const override
    {
        return zoom;
    }

    void Drain()
    {
        while (!queue.empty())
        {
            auto call = std::move(queue.front());
            queue.erase(queue.begin());
            call();
        }
    }
};

// ---- A preset model that records what it is asked ---------------------------------------------------------------

class FakePresets final : public view::PresetModel
{
  public:
    std::vector<std::string>         names{"Bright", "Squash"};
    std::string                      current{"Squash"};
    bool                             modified{true};
    std::vector<std::string>         log;
    std::string                      problem;
    mutable std::vector<std::string> listed; ///< the names as Names() last listed them, which Load reads by index
    mutable int                      listings{0};

    std::vector<std::string> Names() const override
    {
        listed = names;
        ++listings;
        return names;
    }

    std::string Current() const override
    {
        return current;
    }

    bool Modified() const override
    {
        return modified;
    }

    std::string Load(std::size_t index) override
    {
        if (index >= listed.size())
        {
            return "That preset is no longer in the folder.";
        }
        log.push_back("load " + listed[index]);
        current  = listed[index];
        modified = false;
        return problem;
    }

    std::string LoadFile(const std::string &path) override
    {
        log.push_back("file " + path);
        return problem;
    }

    std::string Save(const std::string &name) override
    {
        log.push_back("save " + name);
        current  = name;
        modified = false;
        return problem;
    }

    std::string Delete() override
    {
        log.push_back("delete " + current);
        if (problem.empty())
        {
            current.clear();
        }
        return problem;
    }

    void ResetToDefaults() override
    {
        log.push_back("reset");
    }

    std::string Folder() const override
    {
        return "/presets";
    }

    void ShowFolder() override
    {
        log.push_back("show");
    }

    std::string FilePattern() const override
    {
        return "*.json";
    }

    std::vector<view::MenuItem> ExtensionItems() const override
    {
        view::MenuItem item;
        item.text = "View";
        view::MenuItem size;
        size.text = "150 %";
        size.id   = kExtensionBase + 13;
        item.submenu.push_back(size);
        return {item};
    }

    void PerformExtension(int id) override
    {
        log.push_back("extension " + std::to_string(id));
    }
};

// ---- Panels -----------------------------------------------------------------------------------------------------

constexpr Color kPanelColour{0xff445566};
constexpr Color kInk{0xffe8dcc8};

panel::Chrome TestChrome()
{
    return {"RECKLESS TEST", look::Palette::Series(kPanelColour, Color{0xff334455}), Color{0xff102030},
            look::kHeaderInset, "background.png"};
}

/** The Octave's arrangement: four knobs in a row under the chrome, 648 x 238. */
class RowPanel final : public panel::SeriesPanel
{
  public:
    explicit RowPanel(const view::PanelContext &context) : SeriesPanel(648, 238), face_(*this, context, TestChrome())
    {
        for (const char *id : {"a", "b", "c", "d"})
        {
            face_.AddKnob(id, std::string("KNOB ") + id, std::string("Tip ") + id);
        }
    }

    reckless::panel::SeriesFace &face() noexcept override
    {
        return face_;
    }

    void Paint(reckless::gfx::Canvas &c) override
    {
        face_.Paint(c);
    }

    void Frame(double) override
    {
        face_.Refresh();
    }

    void Resized() override
    {
        const auto area = face_.LayOut();
        face_.LayOutKnobRow(area, 4);
    }

  private:
    reckless::panel::SeriesFace face_;
};

/** The pedal arrangement: four knobs over the switch strip, a plate between the second and third. */
class PedalPanel final : public panel::SeriesPanel
{
  public:
    explicit PedalPanel(const view::PanelContext &context) : SeriesPanel(632, 284), face_(*this, context, TestChrome())
    {
        for (const char *id : {"a", "b", "c", "d"})
        {
            face_.AddKnob(id, std::string("KNOB ") + id, std::string("Tip ") + id);
        }
        law_ = &face_.AddPlate("law", "CLIP", "The curve.");
    }

    reckless::panel::SeriesFace &face() noexcept override
    {
        return face_;
    }

    void Paint(reckless::gfx::Canvas &c) override
    {
        face_.Paint(c);
    }

    void Frame(double) override
    {
        face_.Refresh();
    }

    void Resized() override
    {
        face_.LayOutKnobRowOverStrip(face_.LayOut(), 4, {{law_, 2.0}});
    }

    reckless::panel::SeriesFace face_;
    view::Switch               *law_{nullptr};
};

/** The Mod's arrangement in small: three knobs over the strip with a plate, a fourth knob outside the row laid over
    the second's cell, and a lamp on the header's strip stepped once a frame toward what `level` says. */
class LampPanel final : public panel::SeriesPanel
{
  public:
    explicit LampPanel(const view::PanelContext &context)
        : SeriesPanel(632, 284), face_(*this, context, LampChrome()),
          lamp_(Color{0xff7d1200}, Color{0xff102030}, 0.002, panel::LampView::Filament{})
    {
        AddChild(lamp_);
        for (const char *id : {"a", "b", "c"})
        {
            face_.AddKnob(id, std::string("KNOB ") + id, std::string("Tip ") + id);
        }
        law_   = &face_.AddPlate("law", "CLIP", "The curve.");
        extra_ = &face_.AddKnobOutsideRow("d", "MIX", "The extra.");
    }

    reckless::panel::SeriesFace &face() noexcept override
    {
        return face_;
    }

    void Paint(reckless::gfx::Canvas &c) override
    {
        face_.Paint(c);
    }

    void Frame(double seconds) override
    {
        face_.Refresh();
        lamp_.Step(level, seconds);
    }

    void Resized() override
    {
        lamp_.PlaceIn(face_.HeaderBounds());
        face_.LayOutKnobRowOverStrip(face_.LayOut(), 3, {{law_, 1.5}});
        extra_->SetBounds(face_.KnobFor("b")->bounds());
        extra_->caption().SetBounds(face_.KnobFor("b")->caption().bounds());
    }

    double                      level{0.0};
    reckless::panel::SeriesFace face_;
    panel::LampView             lamp_;
    view::Switch               *law_{nullptr};
    view::Knob                 *extra_{nullptr};

  private:
    /** The test's chrome with the header's right end kept for the lamp. */
    static panel::Chrome LampChrome()
    {
        panel::Chrome chrome        = TestChrome();
        chrome.header_right_reserve = look::kLampStrip;
        return chrome;
    }
};

/** A panel that changes its own size and its plate with it, as the Oldsverb's SHIMMER row and a drawer do: 632 x 284 on
    the test's plate, 632 x 400 on a tall one. */
class GrowingPanel final : public panel::SeriesPanel
{
  public:
    explicit GrowingPanel(const view::PanelContext &context)
        : SeriesPanel(632, 284), face_(*this, context, TestChrome())
    {
        face_.AddKnob("a", "KNOB a", "Tip a");
    }

    reckless::panel::SeriesFace &face() noexcept override
    {
        return face_;
    }

    void Paint(reckless::gfx::Canvas &c) override
    {
        face_.Paint(c);
    }

    void Frame(double) override
    {
        face_.Refresh();
    }

    void Grow(bool tall)
    {
        face_.SetPlate(tall ? "background_tall.png" : "background.png");
        SetDesignSize(632, tall ? 400 : 284);
    }

    void Resized() override
    {
        ++layouts;
        face_.LayOutKnobRow(face_.LayOut(), 1);
    }

    int layouts{0};

  private:
    reckless::panel::SeriesFace face_;
};

/** The context every test's panel is made with: its controls by id, the version, the sized images, the assets. */
struct Fixture
{
    std::map<std::string, std::unique_ptr<FakeParameter>> controls;
    FakePresets                                           presets;
    FakeWindow                                            window;
    view::PanelContext                                    context;

    explicit Fixture(bool with_presets = true)
    {
        for (const char *id : {"a", "b", "c", "d"})
        {
            controls[id] = std::make_unique<FakeParameter>(0.25);
        }
        controls["law"]   = std::make_unique<FakeParameter>(0.0, 0.5, 3);
        context.parameter = [this](std::string_view id) -> view::ParameterControl & {
            return *controls.at(std::string(id));
        };
        context.version      = "1.2.3";
        context.decode_image = [](std::span<const std::uint8_t> bytes) {
            Asset asset{};
            std::memcpy(&asset, bytes.data(), sizeof(Asset));
            return Sized(asset.w, asset.h);
        };
        context.sub_image     = [](const gfx::Image &, RectI area) { return Sized(area.width(), area.height()); };
        context.rescale_image = [](const gfx::Image &, int w, int h, gfx::Resampling) { return Sized(w, h); };
        context.asset         = [](std::string_view name) -> std::span<const std::uint8_t> {
            if (name == "RecklessChickenHead.png")
            {
                return BytesOf(kStrip);
            }
            if (name == "RecklessRtLogo.png")
            {
                return BytesOf(kMark);
            }
            if (name == "background.png")
            {
                return BytesOf(kPlate);
            }
            return {};
        };
        context.presets = with_presets ? &presets : nullptr;
    }

    template <typename PanelT> std::unique_ptr<PanelT> Open()
    {
        auto p = std::make_unique<PanelT>(context);
        p->SetHost(&window);
        p->SetBounds({p->DesignWidth(), p->DesignHeight()});
        return p;
    }
};

/** The sizes every series panel lays its bank out by: the look's. */
view::KnobMetrics Series()
{
    return panel::kKnobMetrics;
}
} // namespace

TEST(SeriesPanel, LaysItsChromeAndKnobsOutWhereTheJucePanelDoes)
{
    Fixture    f;
    const auto p = f.Open<RowPanel>();
    EXPECT_EQ(p->presets().bounds(), (RectI{500, 15, 96, 28}))
        << "the PRESETS plate kResetGap short of the inset clear of the corner screw, centred on the header";
    EXPECT_EQ(p->presets().button().bounds(), (RectI{0, 0, 96, 28})) << "its button over the whole of it";

    const RectI body{32, 68, 584, 160}; // under the header, inside the series' margins
    const auto  cells = view::RowCells(body, 4, 4);
    ASSERT_EQ(p->frames().size(), 4u);
    for (std::size_t i = 0; i < 4; ++i)
    {
        const auto place = view::PlaceKnob(cells[i], Series());
        EXPECT_EQ(p->knobs()[i]->bounds(), place.dial) << i;
        EXPECT_EQ(p->knobs()[i]->caption().bounds(), place.caption) << i;
        EXPECT_EQ(p->frames()[i], place.frame) << i;
    }
    EXPECT_EQ(p->knobs()[0]->bounds(), (RectI{45, 96, 120, 122})) << "the Octave's first knob, as its JUCE slider sat";
    EXPECT_EQ(p->knobs()[0]->caption().text(), "KNOB a");
    EXPECT_EQ(p->knobs()[0]->tooltip, "Tip a");
}

TEST(KnobLayout, AGridFillsItsCellsRowByRowAsManyRowsAsTheCountNeeds)
{
    // Seven knobs in three columns: three rows of equal cells, the last row one cell long; no columns asked is one;
    // no knobs, no cells.
    const RectI area{10, 20, 300, 200};
    const auto  cells = view::GridCells(area, 3, 7);
    ASSERT_EQ(cells.size(), 7u);
    EXPECT_EQ(cells[0], (RectI{10, 20, 100, 66}));
    EXPECT_EQ(cells[2], (RectI{210, 20, 100, 66}));
    EXPECT_EQ(cells[3], (RectI{10, 86, 100, 66})) << "the second row";
    EXPECT_EQ(cells[6], (RectI{10, 152, 100, 66})) << "the third, one cell long";
    const auto stacked = view::GridCells(area, 0, 2);
    ASSERT_EQ(stacked.size(), 2u);
    EXPECT_EQ(stacked[1], (RectI{10, 120, 300, 100})) << "no columns asked: one, the rows stacked";
    EXPECT_TRUE(view::GridCells(area, 4, 0).empty());
}

TEST(KnobLayout, AKnobAndAPlateFallInTheirCellWhereTheSeriesPutsThem)
{
    // The knob bank's own numbers (view/KnobLayout.h), as the knob bank of the releases built on JUCE laid its
    // components out. The frame round a group: 2 px in at each side, 5 px of air above and below, then 0.6 in so the
    // 1.2 px stroke lands on the half pixel - round a 140 x 140 group at (10, 50), 12.6, 45.6, 134.8 by 148.8.
    const view::KnobMetrics m     = Series();
    const RectF             frame = view::FrameAround({10, 50, 140, 140}, m);
    EXPECT_FLOAT_EQ(frame.x(), 12.6f);
    EXPECT_FLOAT_EQ(frame.y(), 45.6f);
    EXPECT_FLOAT_EQ(frame.width(), 134.8f);
    EXPECT_FLOAT_EQ(frame.height(), 148.8f);

    // A knob in a 140 x 200 cell at (10, 20): the caption row over the 122 px block, 140 tall, the cell's 60 spare
    // rows 30 above and 30 below and none between - the caption on the knob - the dial its full 120 centred, and the
    // frame round the two.
    const view::KnobPlace knob = view::PlaceKnob({10, 20, 140, 200}, m);
    EXPECT_EQ(knob.caption, (RectI{10, 50, 140, 18})) << "the caption spans the cell, 30 rows down";
    EXPECT_EQ(knob.dial, (RectI{20, 68, 120, 122})) << "directly under it, centred";
    EXPECT_EQ(knob.frame, view::FrameAround({10, 50, 140, 140}, m));

    // A narrow cell: the dial the cell less 8, 4 px clear at each side. A cell shorter than the group: the group
    // centred all the same, its spare rows negative and halved toward zero, as JUCE's Rectangle halves them.
    const view::KnobPlace narrow = view::PlaceKnob({10, 20, 100, 200}, m);
    EXPECT_EQ(narrow.dial, (RectI{14, 68, 92, 122}));
    const view::KnobPlace small = view::PlaceKnob({13, 7, 47, 61}, m);
    EXPECT_EQ(small.caption, (RectI{13, -32, 47, 18})) << "(61 - 140) / 2 is -39";
    EXPECT_EQ(small.dial, (RectI{17, -14, 39, 122}));
    EXPECT_FLOAT_EQ(small.frame.x(), 15.6f);
    EXPECT_FLOAT_EQ(small.frame.y(), -36.4f);
    EXPECT_FLOAT_EQ(small.frame.width(), 41.8f);
    EXPECT_FLOAT_EQ(small.frame.height(), 148.8f);

    // A plate in a strip cell, exactly a caption row and a plate tall: the pair fills it, the caption the cell's width
    // at its top, the plate under it centred; in a taller cell the pair stays together, centred.
    const view::PlatePlace strip = view::PlacePlate({30, 300, 140, 46}, 124, m);
    EXPECT_EQ(strip.caption, (RectI{30, 300, 140, 18}));
    EXPECT_EQ(strip.plate, (RectI{38, 318, 124, 28}));
    const view::PlatePlace tall = view::PlacePlate({30, 300, 140, 150}, 124, m);
    EXPECT_EQ(tall.caption, (RectI{30, 352, 140, 18})) << "(150 - 46) / 2 rows down";
    EXPECT_EQ(tall.plate, (RectI{38, 370, 124, 28}));

    // A plate in the switch strip at column 2.5 of a five-column row - between the third and fourth knobs - 16 px
    // narrower than a column: its cell centred on 24 + 365 (2.5 x 146, rounded), the plate centred there.
    const RectI            area{24, 68, 730, 184};
    const RectI            strip_row{24, 252, 730, 46};
    const view::PlatePlace between = view::PlacePlateInStrip(area, strip_row, 2.5, 146, 16, m);
    EXPECT_EQ(between.caption, (RectI{316, 252, 146, 18}));
    EXPECT_EQ(between.plate, (RectI{324, 270, 130, 28}));
    EXPECT_EQ(between.plate.CentreX(), 389) << "centred on the column";
    const view::PlatePlace first = view::PlacePlateInStrip(area, strip_row, 0.5, 146, 16, m);
    EXPECT_EQ(first.plate, (RectI{32, 270, 130, 28})) << "under the first knob";
}

TEST(SeriesPanel, AddsItsControlsInTheOrderTheJucePanelAddedThem)
{
    Fixture     f;
    const auto  p        = f.Open<PedalPanel>();
    const auto &children = p->children();
    ASSERT_EQ(children.size(), 2u + 4u * 2u + 2u);
    EXPECT_EQ(children[0], &p->presets()) << "the PRESETS plate first";
    EXPECT_EQ(children[1], &p->settings()) << "then the settings' gear";
    for (std::size_t i = 0; i < 4; ++i)
    {
        EXPECT_EQ(children[2 + 2 * i], p->knobs()[i]) << "each knob, then its caption over it";
        EXPECT_EQ(children[3 + 2 * i], &p->knobs()[i]->caption());
    }
    EXPECT_EQ(children[10], p->law_) << "then the plate, then its caption";
    EXPECT_EQ(children[11], &p->law_->caption());
    EXPECT_EQ(p->law_->caption().text(), "CLIP");
}

TEST(SeriesPanel, ThePedalLayoutPutsEachPlateOnItsColumnInTheStrip)
{
    Fixture    f;
    const auto p     = f.Open<PedalPanel>();
    RectI      area  = RectI{632, 284}.WithTrimmedTop(look::kHeaderHeight).Reduced(look::kMarginX, look::kMarginY);
    const auto strip = area.RemoveFromBottom(look::kSwitchStrip);
    const auto place = view::PlacePlateInStrip(area, strip, 2.0, area.width() / 4, 16, Series());
    EXPECT_EQ(p->law_->bounds(), place.plate);
    EXPECT_EQ(p->law_->caption().bounds(), place.caption);
    EXPECT_EQ(p->law_->legend().text(), "Choice 0");
    ASSERT_EQ(p->frames().size(), 4u) << "framed: the knobs, not the plate";
    EXPECT_EQ(p->knobs()[0]->bounds(), view::PlaceKnob(view::RowCells(area, 4, 4)[0], Series()).dial);
}

TEST(SeriesPanel, PaintsThePlateTheHeaderTheStampAndTheFrames)
{
    Fixture         f;
    const auto      p = f.Open<RowPanel>();
    RecordingCanvas canvas;
    p->Paint(canvas);
    ASSERT_GE(canvas.images.size(), 2u);
    EXPECT_EQ(canvas.images[0].w, 1264) << "the face plate first, at 2x";
    EXPECT_EQ(canvas.images[0].dest, (RectF{0.0f, 0.0f, 648.0f, 238.0f}));
    EXPECT_FALSE(canvas.images[0].high) << "at 1x the 2:1 reduction every review image holds the chrome to";
    EXPECT_EQ(canvas.texts, (std::vector<std::string>{"RECKLESS TEST", "v1.2.3"})) << "the title, the stamp";
    ASSERT_EQ(canvas.rounded.size(), 4u) << "a frame round each knob";
    EXPECT_EQ(canvas.rounded[2], p->frames()[2]);
    EXPECT_EQ(canvas.rounded_colours[0], kInk.WithAlpha(0.6f));

    f.window.zoom = 1.5f; // the shell's zoom
    RecordingCanvas zoomed;
    p->Paint(zoomed);
    EXPECT_TRUE(zoomed.images[0].high) << "zoomed, the better filter";
}

TEST(SeriesPanel, TheHeaderSaysDemoBesideTheStampWhileTheContextSaysSo)
{
    // Demo mode: the format's word (the context's `demo`), read once a frame; the header repaints when it moves, and
    // with no word the header is as it always was.
    Fixture f;
    bool    demo   = false;
    f.context.demo = [&demo] { return demo; };
    const auto p   = f.Open<RowPanel>();
    p->Frame(1.0 / 60.0);
    EXPECT_FALSE(p->face().demo());
    RecordingCanvas plain;
    p->Paint(plain);
    EXPECT_EQ(plain.texts, (std::vector<std::string>{"RECKLESS TEST", "v1.2.3"}));

    demo = true;
    f.window.invalid.clear();
    p->Frame(1.0 / 60.0);
    EXPECT_TRUE(p->face().demo());
    EXPECT_FALSE(f.window.invalid.empty()) << "the header painted again";
    RecordingCanvas badged;
    p->Paint(badged);
    EXPECT_EQ(badged.texts, (std::vector<std::string>{"RECKLESS TEST", "v1.2.3", "DEMO"}));

    demo = false; // a licence installed
    p->Frame(1.0 / 60.0);
    EXPECT_FALSE(p->face().demo());
}

TEST(SeriesPanel, TheHeaderSaysUpdateWhileANewerReleaseIsOutAndDemoBeforeIt)
{
    // The update check's word (the context's `update_available`), read once a frame as DEMO is and in the same ink;
    // DEMO, where both hold, is the one the header says.
    Fixture f;
    bool    update             = false;
    bool    demo               = false;
    f.context.update_available = [&update] { return update; };
    f.context.demo             = [&demo] { return demo; };
    const auto p               = f.Open<RowPanel>();
    p->Frame(1.0 / 60.0);
    EXPECT_FALSE(p->face().update_available());

    update = true;
    f.window.invalid.clear();
    p->Frame(1.0 / 60.0);
    EXPECT_TRUE(p->face().update_available());
    EXPECT_FALSE(f.window.invalid.empty()) << "the header painted again";
    RecordingCanvas badged;
    p->Paint(badged);
    EXPECT_EQ(badged.texts, (std::vector<std::string>{"RECKLESS TEST", "v1.2.3", "UPDATE"}));

    demo = true;
    p->Frame(1.0 / 60.0);
    RecordingCanvas both;
    p->Paint(both);
    EXPECT_EQ(both.texts, (std::vector<std::string>{"RECKLESS TEST", "v1.2.3", "DEMO"})) << "DEMO first";
}

TEST(SeriesPanel, APanelThatChangesItsOwnSizeIsLaidOutAtItOnThePlateItChoseAndItsWindowTold)
{
    Fixture     f;
    const Asset tall{1264, 800};
    const auto  asset   = f.context.asset;
    const auto  decode  = f.context.decode_image;
    int         decodes = 0;
    f.context.asset     = [&tall, asset](std::string_view name) -> std::span<const std::uint8_t> {
        return name == "background_tall.png" ? BytesOf(tall) : asset(name);
    };
    f.context.decode_image = [&decodes, decode](std::span<const std::uint8_t> bytes) {
        ++decodes;
        return decode(bytes);
    };
    const auto p      = f.Open<GrowingPanel>();
    int        told   = 0;
    p->on_design_size = [&told] { ++told; };
    const int decoded = decodes;
    const int laid    = p->layouts;

    p->Grow(true);
    EXPECT_EQ(p->DesignHeight(), 400);
    EXPECT_EQ(p->bounds(), (RectI{632, 400})) << "laid out at its new size, in its own pixels";
    EXPECT_EQ(p->layouts, laid + 1);
    EXPECT_EQ(told, 1) << "whoever shows it told, once";
    EXPECT_EQ(p->chrome().plate, "background_tall.png");
    RecordingCanvas canvas;
    p->Paint(canvas);
    ASSERT_FALSE(canvas.images.empty());
    EXPECT_EQ(canvas.images[0].h, 800) << "the tall plate, at 2x the new size";
    EXPECT_EQ(canvas.images[0].dest, (RectF{0.0f, 0.0f, 632.0f, 400.0f}));

    p->Grow(true);
    EXPECT_EQ(told, 1) << "nothing when the size stands";
    p->Grow(false);
    EXPECT_EQ(told, 2);
    EXPECT_EQ(p->bounds(), (RectI{632, 284}));
    RecordingCanvas back;
    p->Paint(back);
    EXPECT_EQ(back.images[0].h, 476) << "the first plate again";
    p->Grow(true);
    EXPECT_EQ(decodes, decoded + 1) << "each plate decoded once, the first time it is asked for";
}

TEST(SeriesPanel, AKnobsDialIsTheFilmStripsFrameAtTheSlidersAngle)
{
    Fixture         f;
    const auto      p = f.Open<RowPanel>();
    RecordingCanvas canvas;
    p->knobs()[0]->Paint(canvas);
    ASSERT_EQ(canvas.images.size(), 1u);
    // The slider's area over its box, 120 x 105: the frame 2.8 times half its shorter side less 4, centred in the area
    // less 4 px a side, at the physical size it lands on.
    const float side = look::KnobStrip::SideFor(120, 105);
    EXPECT_FLOAT_EQ(side, 2.8f * 48.5f);
    EXPECT_EQ(canvas.images[0].dest, RectF(side, side).WithCentre({60.0f, 52.5f}));
    EXPECT_EQ(canvas.images[0].w, gfx::RoundToInt(side));
}

TEST(SeriesPanel, FrameBringsEveryControlAndThePlateUpToTheHost)
{
    Fixture    f;
    const auto p = f.Open<PedalPanel>();
    EXPECT_EQ(p->presets().button().text(), "Squash *");
    f.controls["a"]->SetNormalized(0.75);
    f.controls["law"]->SetNormalized(1.0);
    f.presets.modified = false;
    p->Frame(1.0 / 60.0);
    EXPECT_EQ(p->knobs()[0]->value_box().text(), "75 %");
    EXPECT_EQ(p->law_->legend().text(), "Choice 2");
    EXPECT_EQ(p->presets().button().text(), "Squash");
    f.presets.current.clear();
    p->Frame(1.0 / 60.0);
    EXPECT_EQ(p->presets().button().text(), "PRESETS");
}

TEST(SeriesPanel, ThePresetsMenuIsThePresetsTheGearTheWindowsAndEachActsOnTheModel)
{
    Fixture                  f;
    const auto               p     = f.Open<RowPanel>();
    const auto               items = p->presets().MenuItems();
    std::vector<std::string> texts;
    for (const auto &item : items)
    {
        texts.push_back(item.separator ? "---" : item.text);
    }
    EXPECT_EQ(texts,
              (std::vector<std::string>{"Bright", "Squash", "---", "Save", "Save as...", "Delete \"Squash\"...", "---",
                                        "Load from file...", "Show presets folder", "---", "Reset to defaults"}))
        << "the presets alone, no window's item";
    EXPECT_TRUE(items[1].ticked) << "the current preset";
    EXPECT_EQ(items[0].id, 1);
    EXPECT_EQ(items[3].id, panel::PresetPlateView::kSave);

    // The window's own items behind the settings' gear, left of PRESETS on the header's centre line, a plate high.
    auto &gear = p->settings();
    EXPECT_TRUE(gear.visible());
    EXPECT_EQ(gear.bounds().right() + look::kResetGap, p->presets().bounds().x());
    EXPECT_EQ(gear.bounds().CentreY(), p->presets().bounds().CentreY());
    EXPECT_EQ(gear.bounds().height(), look::kPlateHeight);
    const auto settings = gear.MenuItems();
    ASSERT_EQ(settings.size(), 1u);
    EXPECT_EQ(settings.front().text, "View");
    ASSERT_EQ(settings.front().submenu.size(), 1u);
    EXPECT_EQ(gear.button().tooltip, panel::SettingsPlateView::kTooltip);

    auto &plate = p->presets();
    plate.Choose(1);
    plate.Choose(panel::PresetPlateView::kSave);
    plate.Choose(panel::PresetPlateView::kShowFolder);
    plate.Choose(panel::PresetPlateView::kResetToDefaults);
    gear.Choose(view::PresetModel::kExtensionBase + 13);
    plate.Choose(panel::PresetPlateView::kLoadFromFile);
    EXPECT_EQ(f.presets.log, (std::vector<std::string>{"load Bright", "save Bright", "show", "reset", "extension 2014",
                                                       "file /presets/Lead.json"}));
    ASSERT_EQ(f.window.requests.size(), 1u);
    EXPECT_EQ(f.window.requests[0].folder, "/presets") << "the dialog opens in the presets' folder";
    EXPECT_EQ(f.window.requests[0].patterns, (std::vector<std::string>{"*.json"}));

    // Save as asks for a name over the panel, filled with the current one; Delete asks first.
    f.presets.log.clear();
    plate.Choose(panel::PresetPlateView::kSaveAs);
    ASSERT_NE(plate.dialog(), nullptr);
    EXPECT_EQ(plate.dialog()->text(), "Bright");
    EXPECT_EQ(plate.dialog()->parent(), p.get()) << "over the whole panel";
    plate.dialog()->KeyPressed({view::Key::kReturn, 0, {}});
    f.window.Drain();
    EXPECT_EQ(plate.dialog(), nullptr);
    plate.Choose(panel::PresetPlateView::kDelete);
    ASSERT_NE(plate.dialog(), nullptr);
    plate.dialog()->KeyPressed({view::Key::kEscape, 0, {}});
    f.window.Drain();
    plate.Choose(panel::PresetPlateView::kDelete);
    plate.dialog()->KeyPressed({view::Key::kReturn, 0, {}});
    f.window.Drain();
    EXPECT_EQ(f.presets.log, (std::vector<std::string>{"save Bright", "delete Bright"}))
        << "the Escape deleted nothing";
    EXPECT_EQ(plate.button().text(), "PRESETS");

    // What goes wrong is said over the panel.
    f.presets.problem = "Lead.json is not a preset.";
    plate.Choose(panel::PresetPlateView::kLoadFromFile);
    ASSERT_NE(plate.dialog(), nullptr);
    plate.dialog()->KeyPressed({view::Key::kReturn, 0, {}});
    f.window.Drain();
    EXPECT_EQ(plate.dialog(), nullptr);
}

TEST(SeriesPanel, APresetsRowLoadsThePresetItShowedAndWhatGoesWrongIsSaid)
{
    // The menu lists the folder as it opens; another instance saving a preset while it is open moves no row onto
    // another preset, since a row loads from the listing the menu showed. A folder of more presets than the plate's
    // own items leave room for shows the first thousand, so Save is Save. A delete the system refuses is said.
    Fixture    f;
    const auto p     = f.Open<RowPanel>();
    auto      &plate = p->presets();
    (void)plate.MenuItems();                                  // the menu opens on Bright, Squash
    f.presets.names.insert(f.presets.names.begin(), "Alpha"); // and another instance saves Alpha
    plate.Choose(2);
    EXPECT_EQ(f.presets.log, (std::vector<std::string>{"load Squash"})) << "the row the menu showed";
    EXPECT_EQ(f.presets.listings, 1) << "the folder not listed again under the menu";

    f.presets.names.resize(1001, "More");
    f.presets.log.clear();
    const auto items = plate.MenuItems();
    EXPECT_EQ(std::count_if(
                  items.begin(), items.end(),
                  [](const view::MenuItem &item) { return item.id >= 1 && item.id < panel::PresetPlateView::kSave; }),
              1000)
        << "the first thousand";
    plate.Choose(panel::PresetPlateView::kSave);
    EXPECT_EQ(f.presets.log, (std::vector<std::string>{"save Squash"})) << "Save, not the thousand and first preset";

    f.presets.log.clear();
    f.presets.problem = "Squash.json could not be deleted from the presets folder.";
    plate.Choose(panel::PresetPlateView::kDelete);
    ASSERT_NE(plate.dialog(), nullptr);
    plate.dialog()->KeyPressed({view::Key::kReturn, 0, {}}); // yes, delete it
    f.window.Drain();
    ASSERT_NE(plate.dialog(), nullptr) << "what went wrong, said over the panel";
    EXPECT_EQ(plate.dialog()->question(), f.presets.problem);
    EXPECT_EQ(plate.button().text(), "Squash") << "still on the preset";
}

TEST(SeriesPanel, AClickOnThePlateOpensItsMenuUnderIt)
{
    Fixture           f;
    const auto        p = f.Open<RowPanel>();
    view::MouseRouter router(*p, &f.window);
    router.Down({532.0f, 29.0f}, 1, false, {});
    EXPECT_TRUE(f.window.popups.empty()) << "a button acts on its release";
    router.Up({532.0f, 29.0f}, {});
    ASSERT_EQ(f.window.popups.size(), 1u);
    EXPECT_TRUE(p->presets().menu_open());
    auto &menu = dynamic_cast<view::Menu &>(*f.window.popups[0]);
    EXPECT_EQ(menu.items().size(), p->presets().MenuItems().size());
    EXPECT_GE(menu.width(), 96) << "at least as wide as the plate";
}

TEST(SeriesPanel, WithNoPresetsTheMenuIsResetToDefaultsAlone)
{
    Fixture    f(false);
    const auto p     = f.Open<PedalPanel>();
    const auto items = p->presets().MenuItems();
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].text, "Reset to defaults");
    EXPECT_EQ(p->presets().button().text(), "PRESETS");
    p->presets().Choose(panel::PresetPlateView::kResetToDefaults);
    for (const char *id : {"a", "b", "c", "d", "law"})
    {
        EXPECT_DOUBLE_EQ(f.controls[id]->Normalized(), 0.5) << id;
        EXPECT_EQ(f.controls[id]->log, (std::vector<std::string>{"begin", "set", "end"})) << id << ": one gesture";
    }
    EXPECT_EQ(p->knobs()[0]->value_box().text(), "50 %") << "and the panel shows it";
}

TEST(PanelStyle, TheButtonsTextSitsWhereTheJuceButtonPutsIt)
{
    // A 96 x 28 plate: the button font held to 12.1, four pixels above and below, and at each side the font's six
    // tenths rounded (7) under two and a quarter of the plate's shorter side (9): the text in 7, 4, 82 x 20. A toggle's
    // lamp at the left and a menu's arrow at the right each take that side's indent to the legend's room from the
    // arrow (kArrowInset + kArrowHalfWidth + kLegendClearance, 18), the text keeping clear of them.
    struct Fitted final : Canvas
    {
        RectI area{};
        int   lines{0};
        float font{0.0f};
        bool  bold{false};
        int   ellipses{0}; // a lamp's lens and its glow
        int   paths{0};    // a menu's arrow

        void Save() override
        {
        }

        void Restore() override
        {
        }

        void AddTransform(const gfx::Affine &) override
        {
        }

        bool ClipToRect(RectI) override
        {
            return true;
        }

        void ClipToPath(const gfx::Path &, const gfx::Affine &) override
        {
        }

        RectI ClipBounds() const override
        {
            return {0, 0, 1000, 1000};
        }

        float PhysicalPixelScale() const override
        {
            return 1.0f;
        }

        void SetColor(Color) override
        {
        }

        void SetGradient(const gfx::Gradient &) override
        {
        }

        void SetOpacity(float) override
        {
        }

        void SetFont(const gfx::Font &f) override
        {
            font  = f.height;
            bold  = f.bold;
            font_ = f;
        }

        void SetResampling(gfx::Resampling) override
        {
        }

        const gfx::Font &CurrentFont() const override
        {
            return font_;
        }

        void FillAll() override
        {
        }

        void FillAll(Color) override
        {
        }

        void FillRect(RectI) override
        {
        }

        void FillRect(RectF) override
        {
        }

        void DrawRect(RectF, float) override
        {
        }

        void FillRoundedRect(RectF, float) override
        {
        }

        void DrawRoundedRect(RectF, float, float) override
        {
        }

        void FillEllipse(RectF) override
        {
            ++ellipses;
        }

        void DrawEllipse(RectF, float) override
        {
        }

        void DrawLine(PointF, PointF, float) override
        {
        }

        void DrawHorizontalLine(int, float, float) override
        {
        }

        void DrawVerticalLine(int, float, float) override
        {
        }

        void FillPath(const gfx::Path &, const gfx::Affine &) override
        {
            ++paths;
        }

        void StrokePath(const gfx::Path &, const gfx::Stroke &, const gfx::Affine &) override
        {
        }

        void DrawText(std::string_view, RectF, gfx::Justify, bool) override
        {
        }

        void DrawFittedText(std::string_view, RectI r, gfx::Justify, int n, float) override
        {
            area  = r;
            lines = n;
        }

        float TextWidth(const gfx::Font &, std::string_view) const override
        {
            return 0.0f;
        }

        float Ascent(const gfx::Font &) const override
        {
            return 0.0f;
        }

        float Descent(const gfx::Font &) const override
        {
            return 0.0f;
        }

        void DrawImage(const gfx::Image &, RectF, bool) override
        {
        }

        void DrawImageTransformed(const gfx::Image &, const gfx::Affine &) override
        {
        }

        void BeginTransparencyLayer(float) override
        {
        }

        void EndTransparencyLayer() override
        {
        }

        gfx::Font font_;
    } canvas;

    const auto palette = look::Palette::Series(kPanelColour, Color{0xff334455});
    panel::ButtonPainter(palette)(canvas, {0, 0, 96, 28}, "PRESETS", {});
    EXPECT_EQ(canvas.area, (RectI{7, 4, 82, 20}));
    EXPECT_EQ(canvas.lines, 2);
    EXPECT_FLOAT_EQ(canvas.font, look::kButtonFontSize * look::kTextScale);
    EXPECT_TRUE(canvas.bold);
    EXPECT_EQ(canvas.ellipses, 0) << "a plain button carries no lamp";
    EXPECT_EQ(canvas.paths, 0) << "and no arrow";

    constexpr int marked = look::kArrowInset + look::kArrowHalfWidth + look::kLegendClearance;
    static_assert(marked == 18);
    view::Button::Look toggle;
    toggle.lamp = true;
    panel::ButtonPainter(palette)(canvas, {0, 0, 96, 28}, "ON", toggle);
    EXPECT_EQ(canvas.area, (RectI{marked, 4, 96 - marked - 7, 20})) << "a toggle's text clear of its lamp";
    EXPECT_EQ(canvas.ellipses, 1) << "the dark lens alone, off";
    toggle.on       = true;
    canvas.ellipses = 0;
    panel::ButtonPainter(palette)(canvas, {0, 0, 96, 28}, "ON", toggle);
    EXPECT_EQ(canvas.ellipses, 3) << "on, the lens and the two rings of its glow";

    panel::MenuButtonPainter(palette)(canvas, {0, 0, 96, 28}, "PRESETS", {});
    EXPECT_EQ(canvas.area, (RectI{7, 4, 96 - 7 - marked, 20})) << "a menu plate's text clear of its arrow";
    EXPECT_EQ(canvas.paths, 1) << "the arrow";
}

TEST(SeriesPanel, ThePresetsPlateCarriesTheArrowOfAPlateThatOpensAMenu)
{
    Fixture         f;
    const auto      p = f.Open<RowPanel>();
    RecordingCanvas c;
    auto           &button = p->presets().button();
    button.Paint(c);
    EXPECT_EQ(c.paths, 1) << "the arrow every plate that opens a menu carries";
}

TEST(PanelStyle, AKnobsCaptionIsTheSeriesBoldCaptionAndItsBoxTheWidthAsked)
{
    // The knob's dress: the caption in the caption font, 12.1 under the 1.1 text scale, bold, centred, in the ink; the
    // value box in the value font, not bold, opening the series' editor in the same font; the box the series' 62 px,
    // or the width a panel asks - 84 for a box with a suffix - centred at the foot of the 120 x 122 block.
    const auto palette = look::Palette::Series(kPanelColour, Color{0xff334455});
    const auto style   = panel::KnobStyle(palette);
    EXPECT_FLOAT_EQ(style.caption.font.height, 12.1f * 1.1f);
    EXPECT_TRUE(style.caption.font.bold);
    EXPECT_EQ(style.caption.justify, gfx::Justify::kCentred);
    EXPECT_EQ(style.caption.text, look::colors::kInk);
    EXPECT_FLOAT_EQ(style.value.font.height, look::kValueBoxFontSize * look::kTextScale);
    EXPECT_FALSE(style.value.font.bold);
    EXPECT_EQ(style.value.justify, gfx::Justify::kCentred);
    EXPECT_FLOAT_EQ(style.value.editor.font.height, style.value.font.height);
    EXPECT_EQ(style.box_width, 62);

    const auto    wide = panel::KnobStyle(palette, 84);
    FakeParameter hold(0.83);
    view::Knob    knob(hold, "HOLD", [](Canvas &, RectI, float) {}, wide, " hold");
    knob.SetBounds({0, 0, 120, 122});
    EXPECT_EQ(knob.BoxBounds(), (RectI{18, 105, 84, 17})) << "an 84 px box centred at the foot";
    EXPECT_EQ(knob.value_box().text(), "83 % hold") << "the parameter's text with the suffix";
}

TEST(ShellStyle, ASeriesPanelsWindowIsLetterboxedInItsColourAndItsTipsAreInItsPalette)
{
    // What the window dresses a series panel's shell in, as the look of the releases built on JUCE coloured its
    // tooltip window: the strip beside a letterboxed panel in the panel's colour; the tip on the panel's colour a shade
    // brighter, edged in the accent at half strength, in the ink. A panel of no series keeps the defaults.
    Fixture    f;
    auto       row   = f.Open<RowPanel>();
    const auto style = reckless::window::ShellStyleOf(*row);
    EXPECT_EQ(style.letterbox, kPanelColour);
    EXPECT_EQ(style.tooltip.background, kPanelColour.Brighter(0.08f));
    EXPECT_EQ(style.tooltip.outline, look::colors::kAmber.WithAlpha(0.5f));
    EXPECT_EQ(style.tooltip.text, look::colors::kInk);

    struct Plain final : view::Panel
    {
        Plain() : view::Panel(10, 10)
        {
        }
    } plain;

    const reckless::window::ShellStyle defaults;
    EXPECT_EQ(reckless::window::ShellStyleOf(plain).letterbox, defaults.letterbox);
    EXPECT_EQ(reckless::window::ShellStyleOf(plain).tooltip.background, defaults.tooltip.background);
}

// ---- What the views' panels move once a frame ------------------------------------------------------------------

TEST(Panel, HearsWhenItsWindowFindsItOnAScreenAndOffOne)
{
    // The window's word each frame: off a screen until one finds it, each change heard once, the same word again no
    // change at all.
    struct Watched final : view::Panel
    {
        Watched() : view::Panel(10, 10)
        {
        }

        void ShownChanged(bool on_screen) override
        {
            heard.push_back(on_screen);
        }

        std::vector<bool> heard;
    };

    Watched panel;
    EXPECT_FALSE(panel.shown());
    panel.SetShown(false);
    EXPECT_TRUE(panel.heard.empty()) << "off a screen to begin with";
    panel.SetShown(true);
    panel.SetShown(true);
    EXPECT_TRUE(panel.shown());
    panel.SetShown(false);
    EXPECT_EQ(panel.heard, (std::vector<bool>{true, false})) << "each change once";
}

TEST(FrameSteps, AFrameIsTheTimeSinceTheOneBeforeTheFirstANominalThirtiethAndNoneLongerThanATenth)
{
    view::FrameSteps steps;
    EXPECT_DOUBLE_EQ(steps.Next(100.0), view::FrameSteps::kFirstStepSeconds);
    EXPECT_DOUBLE_EQ(view::FrameSteps::kFirstStepSeconds, 1.0 / 30.0);
    const double later = 100.0 + 1.0 / 144.0;
    EXPECT_DOUBLE_EQ(steps.Next(later), later - 100.0);
    EXPECT_DOUBLE_EQ(steps.Next(102.0), view::FrameSteps::kMaxStepSeconds) << "after a stall, a tenth";
    EXPECT_DOUBLE_EQ(steps.Next(101.0), 0.0) << "a clock that went back: no time";
    EXPECT_EQ(steps.frames(), 4);
    view::FrameSteps steady;
    const double     first = steady.Next();
    const double     next  = steady.Next();
    EXPECT_DOUBLE_EQ(first, view::FrameSteps::kFirstStepSeconds);
    EXPECT_GE(next, 0.0);
    EXPECT_LE(next, view::FrameSteps::kMaxStepSeconds) << "the steady clock's own seconds";
}

TEST(LevelMeterView, TheNeedleReachesAPeakIn70MsTrailsTheDischargeByADecibelAndLeavesTheStopsInTime)
{
    // The small IN / OUT meter's timings, at 60 and at 144 Hz alike: a -6 dBFS peak reads 0 in the frame it arrives
    // and the needle, a second-order mechanism at 8 Hz up and a damping of 0.8, is 90 % of the way there by 70 ms,
    // under a quarter of a decibel past it at its furthest and within a twentieth of a decibel by 300 ms, then still;
    // full scale reads +6 at once and has the needle on the right stop within 300 ms. From the next frame silence
    // discharges the reading as e^(-t / 0.75 s) of the linear level - 8.69 dB down after one time constant, 17.37
    // after two - with the needle, at 3 Hz down, behind it by its lag alone, under a decibel and a half, and off the
    // scale's left end 3.2 s after full scale. A +6 dBFS peak holds the needle over the top 0.4 s on and lets it under
    // by 0.9 s.
    using Meter                  = panel::LevelMeterView;
    constexpr double kMinus6Dbfs = 0.5011872336272722; // 10^(-6 / 20)
    constexpr double kDbPerTau   = 8.685889638065037;  // 20 log10(e): one time constant of discharge, in dB
    Fixture          f;
    double           at_three_quarters[2];
    int              rate = 0;
    for (const double frame : {1.0 / 60.0, 1.0 / 144.0})
    {
        Meter meter(f.context, "IN");
        meter.SetBounds({Meter::kWidth, Meter::kHeight}); // the repaint test is in pixels of tip travel
        EXPECT_TRUE(meter.Advance(kMinus6Dbfs, frame)) << "the needle moved: a repaint";
        EXPECT_NEAR(meter.reading_db(), 0.0, 1.0e-9) << "a -6 dBFS peak reads 0 in the frame it arrives";
        EXPECT_LT(meter.needle_db(), -3.0) << "the needle has only started toward it";
        double t          = frame;
        double furthest   = -100.0;
        auto   advance_to = [&](double until, double peak) {
            for (; t < until - 1.0e-9; t += frame)
            {
                meter.Advance(peak, frame);
                furthest = std::max(furthest, meter.needle_db());
            }
        };
        advance_to(0.07, kMinus6Dbfs);
        EXPECT_GT(meter.needle_db(), -1.0) << 1.0 / frame << " Hz: 90 % of the way by 70 ms";
        advance_to(0.3, kMinus6Dbfs);
        EXPECT_LT(furthest, 0.3) << "under 3 % (a quarter of a decibel) over at its furthest";
        EXPECT_NEAR(meter.needle_db(), 0.0, 0.05) << "on the peak within a twentieth of a decibel by 300 ms";
        advance_to(0.6, kMinus6Dbfs);
        EXPECT_FALSE(meter.Advance(kMinus6Dbfs, frame)) << "held there and settled: no repaint";
        EXPECT_TRUE(meter.Advance(1.0, frame));
        EXPECT_NEAR(meter.reading_db(), Meter::kMaxDb, 1.0e-9) << "full scale reads +6 at once";
        t = frame;
        advance_to(0.3, 1.0);
        EXPECT_NEAR(meter.needle_db(), Meter::kMaxDb, 0.05) << "the needle at the top within 300 ms";
        EXPECT_NEAR(Meter::AngleDegrees(meter.needle_db()), Meter::kSweepDegrees, 0.1) << "the right stop";

        t = 0.0;
        advance_to(0.75, 0.0);
        const double after_one    = meter.reading_db();
        at_three_quarters[rate++] = after_one;
        EXPECT_NEAR(after_one, Meter::kMaxDb - kDbPerTau, 1.0e-6) << "8.69 dB down after one time constant";
        EXPECT_GT(meter.needle_db(), after_one) << "the needle trails, never leads";
        EXPECT_LT(meter.needle_db(), after_one + 1.5) << "by its lag alone, about a decibel";
        advance_to(1.5, 0.0);
        const double after_two = meter.reading_db();
        EXPECT_NEAR(after_two, Meter::kMaxDb - 2.0 * kDbPerTau, 1.0e-6) << "17.37 dB down after two";
        EXPECT_GT(meter.needle_db(), after_two);
        EXPECT_LT(meter.needle_db(), after_two + 1.5);
        advance_to(3.2, 0.0);
        EXPECT_LT(meter.needle_db(), Meter::kMinDb) << "off the scale's left end by 3.2 s: the reading "
                                                    << meter.reading_db() << ", the needle " << meter.needle_db();

        t = 0.0;
        advance_to(0.3, 2.0);
        EXPECT_NEAR(meter.reading_db(), Meter::kMaxDb + 20.0 * std::log10(2.0), 1.0e-9) << "over full scale, as it is";
        EXPECT_NEAR(meter.needle_db(), meter.reading_db(), 0.2);
        EXPECT_DOUBLE_EQ(Meter::AngleDegrees(meter.needle_db()), Meter::kSweepDegrees) << "the stop";
        t = 0.0;
        advance_to(0.4, 0.0);
        EXPECT_GT(meter.needle_db(), Meter::kMaxDb) << "still over the top 0.4 s on";
        advance_to(0.9, 0.0);
        EXPECT_LT(meter.needle_db(), Meter::kMaxDb) << "under it by 0.9 s";
    }
    EXPECT_NEAR(at_three_quarters[0], at_three_quarters[1], 1.0e-6) << "the same place after the same time either way";
}

TEST(LampView, SitsOnTheHeadersStripTakesNoMouseAndRepaintsOnlyItselfWhenItsGlowMoves)
{
    Fixture    f;
    const auto p = f.Open<LampPanel>();
    EXPECT_EQ(p->lamp_.bounds(), look::LampStrip(RectI{632, look::kHeaderHeight}));
    EXPECT_EQ(p->lamp_.bounds().width(), look::kLampStrip);
    EXPECT_EQ(p->presets().bounds().right(), 632 - look::kLampStrip - look::kResetGap) << "PRESETS left of it";
    EXPECT_EQ(view::ViewAt(*p, p->lamp_.bounds().Centre()), p.get())
        << "paint, not a control: the press is the panel's";

    EXPECT_DOUBLE_EQ(p->lamp_.glow(), 0.0) << "behind a filament, dark until its first frame";
    f.window.invalid.clear();
    p->level = 1.0;
    p->Frame(1.0 / 60.0);
    const double lit = 1.0 - std::exp(-(1.0 / 60.0) / look::kLampRiseSeconds);
    EXPECT_NEAR(p->lamp_.glow(), lit, 1.0e-12) << "the filament's rise over the frame's seconds";
    ASSERT_EQ(f.window.invalid.size(), 1u) << "nothing else moved";
    EXPECT_EQ(f.window.invalid[0], p->lamp_.bounds()) << "its own strip, and only that";

    f.window.invalid.clear();
    p->level = p->lamp_.glow();
    p->Frame(1.0 / 60.0);
    EXPECT_TRUE(f.window.invalid.empty()) << "a glow that did not move is no repaint";

    panel::LampView fed(Color{0xffe0a038}, Color{0xff3a322b}, 0.002, 0.5);
    EXPECT_DOUBLE_EQ(fed.glow(), 0.5) << "a lamp that shows what it is fed starts where it is told";
    fed.Step(0.25, 1.0 / 60.0);
    EXPECT_DOUBLE_EQ(fed.glow(), 0.25) << "and takes a move whole";
}

TEST(LevelMeterView, ReadsAPeakAtOnceDischargesItInSecondsAndCarriesTheNeedleBehind)
{
    // The small peak level meter as a view: the face's decibels six over the digital level, on the half-power law
    // from -30 at the left stop to +6 at the right; a peak read in the frame it arrives and discharged as an RC with a
    // 750 ms time constant from the next - 8.7 dB down one time constant later and 20 dB after 2.3 of them, the same
    // at any frame rate - and the needle following the reading through its mechanism: short of a new peak in its
    // first frame, there within a fifth of a second and never more than a few percent past it, gliding down behind
    // the discharge, held on the right stop by a level over full scale; a frame that moves the tip less than a tenth
    // of a pixel asks for no repaint.
    using Meter = panel::LevelMeterView;
    EXPECT_DOUBLE_EQ(Meter::ReadingDb(-6.0), 0.0) << "-6 dBFS stands on the 0";
    EXPECT_DOUBLE_EQ(Meter::ReadingDb(0.0), 6.0) << "full scale on the +6";
    EXPECT_DOUBLE_EQ(Meter::ScalePosition(Meter::kMinDb), 0.0);
    EXPECT_DOUBLE_EQ(Meter::ScalePosition(Meter::kMaxDb), 1.0);
    EXPECT_DOUBLE_EQ(Meter::ScalePosition(Meter::kMaxDb + 10.0), 1.0) << "clamped at the stops";
    EXPECT_DOUBLE_EQ(Meter::ScalePosition(Meter::kMinDb - 10.0), 0.0);
    EXPECT_NEAR(Meter::ScalePosition(0.0), 2.0 / 3.0, 0.02) << "the 0 two thirds of the way across";
    EXPECT_DOUBLE_EQ(Meter::AngleDegrees(Meter::kMinDb), -Meter::kSweepDegrees);
    EXPECT_DOUBLE_EQ(Meter::AngleDegrees(Meter::kMaxDb), Meter::kSweepDegrees);
    EXPECT_NEAR(Meter::AngleDegrees(0.0), 11.3, 0.05) << "the artwork's reference";

    Fixture f;
    Meter   meter(f.context, "OUT");
    meter.SetBounds({Meter::kWidth, Meter::kHeight});
    EXPECT_LT(meter.reading_db(), Meter::kMinDb) << "silence reads under the scale";
    EXPECT_LT(meter.needle_db(), Meter::kMinDb) << "and the needle rests";
    EXPECT_FALSE(meter.Advance(0.0, 1.0 / 60.0)) << "nothing moved, nothing to paint";

    constexpr double kFrame = 1.0 / 60.0;
    const double     peak   = 20.0 * std::log10(0.5) + 6.0; // a -6.02 dBFS peak on the face
    EXPECT_TRUE(meter.Advance(0.5, kFrame));
    EXPECT_NEAR(meter.reading_db(), peak, 1.0e-12) << "the peak in the frame it arrives";
    EXPECT_LT(meter.needle_db(), peak - 1.0) << "the needle on its way";
    double furthest = meter.needle_db();
    for (int i = 0; i < 11; ++i) // 0.2 s of the peak renewed each frame
    {
        meter.Advance(0.5, kFrame);
        furthest = std::max(furthest, meter.needle_db());
    }
    EXPECT_NEAR(meter.needle_db(), peak, 0.3) << "there within a fifth of a second";
    EXPECT_LT(furthest, peak + 20.0 * std::log10(1.05)) << "and never more than a few percent past it";
    for (int i = 0; i < 120; ++i)
    {
        meter.Advance(0.5, kFrame);
    }
    EXPECT_FALSE(meter.Advance(0.5, kFrame)) << "a needle at rest on a held level repaints nothing";

    // The discharge from the next frame: one time constant later 8.7 dB down, 2.3 of them 20 dB down; the needle
    // gliding down behind it.
    for (int i = 0; i < 45; ++i) // 0.75 s
    {
        meter.Advance(0.0, kFrame);
    }
    EXPECT_NEAR(meter.reading_db(), peak + 20.0 * std::log10(std::exp(-1.0)), 1.0e-9) << "8.7 dB down after 0.75 s";
    EXPECT_NEAR(peak - meter.reading_db(), 8.69, 0.01);
    EXPECT_GT(meter.needle_db(), meter.reading_db()) << "the slower fall: the needle behind the discharge";
    for (int i = 0; i < 59; ++i) // to 2.3 time constants, 1.725 s, at the frame that reaches it
    {
        meter.Advance(0.0, kFrame);
    }
    EXPECT_NEAR(peak - meter.reading_db(), 20.0 * 104.0 * kFrame / Meter::kFallSeconds * std::log10(std::exp(1.0)),
                1.0e-9);
    EXPECT_NEAR(peak - meter.reading_db(), 20.0, 0.1) << "20 dB down after 2.3 time constants";

    // The same seconds at another frame rate: the same reading.
    Meter fast(f.context, "IN");
    fast.SetBounds({Meter::kWidth, Meter::kHeight});
    fast.Advance(0.5, 1.0 / 144.0);
    for (int i = 0; i < 144; ++i) // a second of discharge at 144 Hz
    {
        fast.Advance(0.0, 1.0 / 144.0);
    }
    Meter slow(f.context, "IN");
    slow.SetBounds({Meter::kWidth, Meter::kHeight});
    slow.Advance(0.5, 1.0 / 30.0);
    for (int i = 0; i < 30; ++i)
    {
        slow.Advance(0.0, 1.0 / 30.0);
    }
    EXPECT_NEAR(fast.reading_db(), slow.reading_db(), 1.0e-9) << "the discharge is in seconds, not frames";
    EXPECT_NEAR(fast.needle_db(), slow.needle_db(), 0.5) << "and the needle, to a slow frame's worth of the fall";

    // Over full scale: read as it is, the needle on the right stop.
    for (int i = 0; i < 60; ++i)
    {
        meter.Advance(2.0, kFrame);
    }
    EXPECT_NEAR(meter.reading_db(), 20.0 * std::log10(2.0) + 6.0, 1.0e-12) << "read as it is";
    EXPECT_DOUBLE_EQ(Meter::AngleDegrees(meter.needle_db()), Meter::kSweepDegrees) << "the needle on the right stop";
}

TEST(GainReductionMeterView, TheNeedleIsASecondOrderMechanismInSecondsAtAnyFrameRate)
{
    // The meter's needle is the series' second-order mechanism (look::NeedleBallistics) on the view's own constants -
    // 6 Hz rising, 3 Hz falling, a damping of 0.75 - stepped by the display's seconds: rising to a 10 dB reading it is
    // 90 % of the way inside 100 ms, runs a few percent past the mark as a VU's needle does and is within 2 % of it
    // from 300 ms; falling it is slower, down to a tenth by 300 ms, and comes to rest on the stop without a bounce. At
    // 60 Hz and at 144 the needle is in the same place after the same time, to a hundredth of a decibel.
    using Meter = panel::GainReductionMeterView;
    EXPECT_EQ(Meter::kRiseHz, 6.0);
    EXPECT_EQ(Meter::kFallHz, 3.0);
    EXPECT_EQ(Meter::kDamping, 0.75);
    EXPECT_EQ(Meter::kFullScaleDb, 30.0) << "the artwork's scale";
    EXPECT_EQ(Meter::ScalePosition(0.0), 0.0);
    EXPECT_EQ(Meter::ScalePosition(Meter::kFullScaleDb), 1.0);
    EXPECT_EQ(Meter::ScalePosition(Meter::kFullScaleDb + 40.0), 1.0) << "pinned at the end of the scale";
    EXPECT_GT(Meter::ScalePosition(3.0), 3.0 / Meter::kFullScaleDb) << "the first decibels get more arc";
    double last = -1.0;
    for (double db = 0.0; db <= Meter::kFullScaleDb; db += 0.5)
    {
        EXPECT_GT(Meter::ScalePosition(db), last) << db << " dB";
        last = Meter::ScalePosition(db);
    }

    struct Run
    {
        double frame;
        double peak{0.0};
        double t_90{-1.0};
        double settled_from{-1.0};
        double at_half_second{-1.0};
        double fall_to_tenth{-1.0};
        double lowest{0.0};
    };

    std::vector<Run> runs{{1.0 / 60.0}, {1.0 / 144.0}};
    for (auto &run : runs)
    {
        Meter meter(view::PanelContext{});
        meter.SetBounds({420, 328});
        look::NeedleBallistics reference; // the series' mechanism on the view's constants
        double                 t = 0.0;
        while (t < 1.0 - 1.0e-9)
        {
            meter.Advance(10.0, run.frame);
            reference.Step(10.0, run.frame, 6.0, 3.0, 0.75);
            ASSERT_EQ(meter.needle_db(), reference.position()) << "the mechanism, bit for bit";
            t += run.frame;
            const double db = meter.needle_db();
            run.peak        = std::max(run.peak, db);
            if (run.t_90 < 0.0 && db >= 9.0)
            {
                run.t_90 = t;
            }
            if (std::fabs(db - 10.0) > 0.2)
            {
                run.settled_from = -1.0;
            }
            else if (run.settled_from < 0.0)
            {
                run.settled_from = t;
            }
            if (t >= 0.5 - 1.0e-9 && run.at_half_second < 0.0)
            {
                run.at_half_second = db;
            }
        }
        t = 0.0;
        while (t < 1.0 - 1.0e-9)
        {
            meter.Advance(0.0, run.frame);
            t += run.frame;
            run.lowest = std::min(run.lowest, meter.needle_db());
            if (run.fall_to_tenth < 0.0 && meter.needle_db() <= 1.0)
            {
                run.fall_to_tenth = t;
            }
        }
        std::printf("    needle at %4.0f Hz: 90 %% at %3.0f ms, peak %.2f dB (%+.1f %%), settled from %3.0f ms, "
                    "%.4f dB at 0.5 s, down to a tenth in %3.0f ms\n",
                    1.0 / run.frame, run.t_90 * 1000.0, run.peak, (run.peak - 10.0) * 10.0, run.settled_from * 1000.0,
                    run.at_half_second, run.fall_to_tenth * 1000.0);
        EXPECT_LT(run.t_90, 0.1);
        EXPECT_GT(run.peak, 10.2) << "a hint of overshoot, as a VU has";
        EXPECT_LT(run.peak, 10.8) << "a hint, not a bounce";
        EXPECT_LT(run.settled_from, 0.3);
        EXPECT_GT(run.fall_to_tenth, run.t_90) << "the fall is slower than the rise";
        EXPECT_LT(run.fall_to_tenth, 0.3);
        EXPECT_GE(run.lowest, -0.5) << "the fall does not bounce off the stop";
    }
    EXPECT_NEAR(runs[0].at_half_second, runs[1].at_half_second, 0.01) << "the same place at 60 and 144 Hz";
    EXPECT_NEAR(runs[0].peak, runs[1].peak, 0.05);
}

TEST(GainReductionMeterView, RepaintsWhenTheTipMovesATenthOfAPixelFromWhereItWasPainted)
{
    // A frame is worth a repaint when the needle's tip has moved a tenth of a pixel from where it was last painted -
    // measured against the paint, not the last frame, so a reading that creeps two thousandths of a decibel a frame,
    // a fiftieth of a pixel at 10 dB on a 420 px meter, still moves the needle on the screen every few frames; a
    // needle at rest asks for nothing.
    panel::GainReductionMeterView meter(view::PanelContext{});
    meter.SetBounds({420, 328});
    EXPECT_FALSE(meter.Advance(0.0, 1.0 / 60.0)) << "at rest on the stop";
    EXPECT_TRUE(meter.Advance(10.0, 1.0 / 60.0)) << "on its way";
    for (int i = 0; i < 240; ++i)
    {
        meter.Advance(10.0, 1.0 / 60.0);
    }
    EXPECT_FALSE(meter.Advance(10.0, 1.0 / 60.0)) << "settled on a held reading";
    int    repaints = 0;
    double target   = 10.0;
    for (int i = 0; i < 200; ++i)
    {
        target += 0.002;
        repaints += meter.Advance(target, 1.0 / 60.0) ? 1 : 0;
    }
    std::printf("    a creep of 0.002 dB a frame: %d repaints in 200 frames\n", repaints);
    EXPECT_GE(repaints, 20) << "the creep crosses the threshold against what was painted";
    EXPECT_LT(repaints, 100) << "and not every frame";
}

TEST(GainReductionMeterView, PaintsTheNeedleAloneWithNoArtworkAndBetweenTheFormatsLayersWithIt)
{
    // The face and the glass are the format's to draw (view::PanelContext::draw_artwork): a context with none - a
    // panel rendered with no toolkit - paints the scale's figures and the needle alone, its shadow, its blade and its
    // red tip, and nothing of the artwork. With one, the face's document goes first with the housing's shadow under
    // it, then the needle, then the glass's document over it with no shadow, each the layer's file by name.
    Fixture                       f;
    panel::GainReductionMeterView bare(f.context);
    bare.SetBounds({420, 328});
    RecordingCanvas plain;
    bare.Paint(plain);
    EXPECT_EQ(plain.paths, 3) << "the needle's shadow, its blade and its tip";
    EXPECT_NE(std::find(plain.texts.begin(), plain.texts.end(), "10"), plain.texts.end()) << "the scale's figures";

    const std::uint8_t face_svg[]  = {'f'};
    const std::uint8_t glass_svg[] = {'g'};
    view::PanelContext context     = f.context;
    context.asset                  = [&](std::string_view name) -> std::span<const std::uint8_t> {
        if (name == "vu_meter_face.svg")
        {
            return face_svg;
        }
        if (name == "vu_meter_glass.svg")
        {
            return glass_svg;
        }
        return {};
    };

    struct Drawn
    {
        std::uint8_t first_byte;
        bool         shadow;
        int          paths_before;
    };

    std::vector<Drawn> drawn;
    RecordingCanvas    canvas;
    context.draw_artwork = [&](gfx::Canvas &, const gfx::ArtworkLayer &layer) {
        drawn.push_back({layer.svg.empty() ? std::uint8_t{0} : layer.svg[0], layer.shadow.has_value(), canvas.paths});
    };
    panel::GainReductionMeterView meter(context);
    meter.SetBounds({420, 328});
    meter.Paint(canvas);
    ASSERT_EQ(drawn.size(), 2u) << "the face and the glass";
    EXPECT_EQ(drawn[0].first_byte, 'f');
    EXPECT_TRUE(drawn[0].shadow) << "the housing's shadow under the face";
    EXPECT_EQ(drawn[0].paths_before, 0) << "under the needle";
    EXPECT_EQ(drawn[1].first_byte, 'g');
    EXPECT_FALSE(drawn[1].shadow);
    EXPECT_EQ(drawn[1].paths_before, 3) << "over the needle";
    EXPECT_EQ(canvas.paths, 3);
}

TEST(LevelBarsView, IsAPeakBargraphWithAHeldLineInSeconds)
{
    // The bargraph's face is linear in decibels from -30 at the bottom to +6 at the top, read six decibels over the
    // digital level - a -6 dBFS peak on the 0, full scale on the +6 - and red from the 0, five sixths of the way up.
    // Each bar is two peak programme readings in the linear domain: the bar takes a new peak in the frame it arrives
    // and discharges from it with no hold, e^(-t / 0.25 s), 8.69 dB down a quarter of a second on and 34.7 after one;
    // the line takes the same peak, holds it a second and then discharges at the same rate. In seconds, so the same at
    // 60 and 144 Hz. A held tone stands still and asks for no repaint, a level over full scale is read as it is with
    // the bar clamped at the top, and the two bars are read apart.
    using Bars = panel::LevelBarsView;
    EXPECT_EQ(Bars::Position(Bars::kMinDb), 0.0);
    EXPECT_EQ(Bars::Position(Bars::kMaxDb), 1.0);
    EXPECT_EQ(Bars::Position(20.0), 1.0) << "pinned at the top";
    EXPECT_EQ(Bars::Position(-100.0), 0.0) << "pinned at the floor";
    EXPECT_NEAR(Bars::Position(-12.0), 0.5, 1.0e-12) << "linear in decibels: -12 is halfway";
    EXPECT_NEAR(Bars::Position(Bars::kRedFromDb), 5.0 / 6.0, 1.0e-12) << "the red zone's foot";
    EXPECT_EQ(Bars::kMinDb, -30.0);
    EXPECT_EQ(Bars::kMaxDb, 6.0);
    EXPECT_EQ(Bars::kRedFromDb, 0.0);
    EXPECT_EQ(Bars::kZeroDbfs, -6.0);
    EXPECT_EQ(Bars::kBarFallSeconds, 0.25);
    EXPECT_EQ(Bars::kPeakHoldSeconds, 1.0);
    EXPECT_EQ(Bars::ReadingDb(-6.0), 0.0) << "a -6 dBFS peak reads 0";
    EXPECT_EQ(Bars::ReadingDb(0.0), Bars::kMaxDb) << "full scale reads +6, the top of the bar";
    EXPECT_EQ(Bars::ReadingDb(-36.0), Bars::kMinDb) << "the face's -30 is -36 dBFS";

    constexpr double kMinus6Dbfs = 0.5011872336272722; // 10^(-6 / 20)
    constexpr double kDbPerTau   = 8.685889638065037;  // 20 log10(e): one time constant of discharge, in dB
    double           at_quarter_second[2];
    int              i = 0;
    for (const double frame : {1.0 / 60.0, 1.0 / 144.0})
    {
        Bars bars({"IN", "OUT"}, gfx::colors::kBlack);
        bars.SetBounds({160, 400}); // the repaint test is in pixels of the bars' height
        ASSERT_EQ(bars.bar_count(), 2);
        EXPECT_LT(bars.bar_db(0), Bars::kMinDb) << "empty to start";
        EXPECT_TRUE(bars.Advance({kMinus6Dbfs, 0.0}, frame)) << "the bar rose: a repaint";
        EXPECT_NEAR(bars.bar_db(0), 0.0, 1.0e-9) << "a -6 dBFS peak reads 0 in the frame it arrives";
        EXPECT_NEAR(bars.peak_db(0), 0.0, 1.0e-9) << "and the line is on it";
        EXPECT_LT(bars.bar_db(1), Bars::kMinDb) << "the other bar is its own";
        EXPECT_FALSE(bars.Advance({kMinus6Dbfs, 0.0}, frame)) << "a held tone stands still: no repaint";
        EXPECT_TRUE(bars.Advance({1.0, kMinus6Dbfs}, frame));
        EXPECT_NEAR(bars.bar_db(0), Bars::kMaxDb, 1.0e-9) << "full scale reads +6 at once";
        EXPECT_NEAR(bars.bar_db(1), 0.0, 1.0e-9);
        EXPECT_EQ(Bars::Position(bars.bar_db(0)), 1.0) << "the top of the bar";

        // Silence: the bar falls from the frame after the crest, the line holds a second and then follows.
        double t = 0.0;
        for (; t < 0.25 - 1.0e-9; t += frame)
        {
            bars.Advance({0.0, 0.0}, frame);
        }
        at_quarter_second[i++] = bars.bar_db(0);
        EXPECT_NEAR(bars.bar_db(0), Bars::kMaxDb - kDbPerTau, 1.0e-6) << "8.69 dB down a quarter of a second on";
        EXPECT_NEAR(bars.peak_db(0), Bars::kMaxDb, 1.0e-9) << "the line still on the crest";
        for (; t < 1.0 - 1.0e-9; t += frame)
        {
            bars.Advance({0.0, 0.0}, frame);
        }
        EXPECT_LT(bars.bar_db(0), Bars::kMaxDb - 4.0 * kDbPerTau + 1.0e-6) << "34.7 dB down after a second";
        EXPECT_NEAR(bars.peak_db(0), Bars::kMaxDb, 1.0e-9) << "the line held the whole second";
        for (; t < 1.25 - 1.0e-9; t += frame)
        {
            bars.Advance({0.0, 0.0}, frame);
        }
        EXPECT_NEAR(bars.peak_db(0), Bars::kMaxDb - kDbPerTau, 1.0e-6)
            << "a quarter of a second of discharge after the hold";
        EXPECT_LT(bars.bar_db(0), Bars::kMinDb) << "the bar is off the bottom";
        for (; t < 2.4 - 1.0e-9; t += frame)
        {
            bars.Advance({0.0, 0.0}, frame);
        }
        EXPECT_LT(bars.peak_db(0), Bars::kMinDb) << "the line off the bottom by 2.4 s";
        EXPECT_FALSE(bars.Advance({0.0, 0.0}, frame)) << "empty and still: no repaint";

        // Over full scale: read as it is, the bar and the line clamped at the top.
        EXPECT_TRUE(bars.Advance({2.0, 0.0}, frame));
        EXPECT_NEAR(bars.bar_db(0), Bars::kMaxDb + 20.0 * std::log10(2.0), 1.0e-9);
        EXPECT_EQ(Bars::Position(bars.bar_db(0)), 1.0);
        EXPECT_EQ(Bars::Position(bars.peak_db(0)), 1.0);
    }
    EXPECT_NEAR(at_quarter_second[0], at_quarter_second[1], 1.0e-6)
        << "the same place after the same time at either rate";
}

TEST(LevelBarsView, RepaintsHalfAPixelFromWhatItPaintedAndCentresItsBarsAboutTheScale)
{
    // The bars stand in the screen's padding, the caption row off the bottom and half a figure's row off the top and
    // the bottom; bar 0, the scale column and bar 1 one group centred on the screen. A frame is worth a repaint when a
    // bar's top or a line moved half a pixel from where it was last painted: a peak rising a hundredth of a decibel a
    // frame - a tenth of a pixel on a 400 px screen - still repaints every few frames, and not every one.
    using Bars = panel::LevelBarsView;
    Bars bars({"IN", "OUT"}, gfx::colors::kBlack);
    bars.SetBounds({160, 400});
    const auto area = bars.BarsArea();
    EXPECT_FLOAT_EQ(area.x(), static_cast<float>(Bars::kPadding));
    EXPECT_FLOAT_EQ(area.y(), static_cast<float>(Bars::kPadding + Bars::kFigureReach));
    EXPECT_FLOAT_EQ(area.height(), 400.0f - 2.0f * Bars::kPadding - Bars::kCaptionRow - 2.0f * Bars::kFigureReach);
    const auto in_column  = bars.BarColumn(0);
    const auto out_column = bars.BarColumn(1);
    EXPECT_FLOAT_EQ(in_column.width(), static_cast<float>(Bars::kBarWidth));
    EXPECT_FLOAT_EQ(out_column.x() - in_column.right(), static_cast<float>(Bars::kScaleWidth))
        << "the scale between the two bars";
    EXPECT_FLOAT_EQ((in_column.x() + out_column.right()) / 2.0f, 80.0f) << "the pair centred on the screen";
    EXPECT_FLOAT_EQ(in_column.y(), area.y());
    EXPECT_FLOAT_EQ(in_column.height(), area.height()) << "a bar the area's height, -30 to +6";

    double dbfs     = -20.0;
    int    repaints = 0;
    bars.Advance({std::pow(10.0, dbfs / 20.0), 0.0}, 1.0 / 60.0);
    for (int frame = 0; frame < 100; ++frame)
    {
        dbfs += 0.01;
        repaints += bars.Advance({std::pow(10.0, dbfs / 20.0), 0.0}, 1.0 / 60.0) ? 1 : 0;
    }
    std::printf("    a peak rising 0.01 dB a frame: %d repaints in 100 frames\n", repaints);
    EXPECT_NEAR(bars.bar_db(0), Bars::ReadingDb(dbfs), 1.0e-9);
    EXPECT_GE(repaints, 10) << "the rise crosses half a pixel against what was painted";
    EXPECT_LT(repaints, 50) << "and not every frame";
}

TEST(SeriesPanel, AKnobOutsideTheRowIsThePanelsToPlaceAndTheRestIsTheBanks)
{
    Fixture    f;
    const auto p    = f.Open<LampPanel>();
    RectI      area = RectI{632, 284}.WithTrimmedTop(look::kHeaderHeight).Reduced(look::kMarginX, look::kMarginY);
    area.RemoveFromBottom(look::kSwitchStrip);
    const auto cells = view::RowCells(area, 3, 3);
    ASSERT_EQ(p->frames().size(), 3u) << "a frame round each knob of the row, none for the one outside it";
    for (std::size_t i = 0; i < 3; ++i)
    {
        EXPECT_EQ(p->knobs()[i]->bounds(), view::PlaceKnob(cells[i], Series()).dial) << i;
    }
    EXPECT_EQ(p->KnobFor("d"), p->extra_) << "found by its parameter like any other";
    EXPECT_EQ(p->extra_->bounds(), p->knobs()[1]->bounds()) << "where the panel put it";
    EXPECT_EQ(p->PlateFor("law"), p->law_);
    EXPECT_EQ(p->PlateFor("d"), nullptr);
    ASSERT_EQ(p->plates().size(), 1u);

    f.controls["d"]->SetNormalized(0.9);
    p->Frame(1.0 / 60.0);
    EXPECT_EQ(p->extra_->value_box().text(), "90 %") << "brought up to the host with the rest";
    Fixture    bare(false);
    const auto q = bare.Open<LampPanel>();
    q->presets().Choose(panel::PresetPlateView::kResetToDefaults);
    EXPECT_DOUBLE_EQ(bare.controls["d"]->Normalized(), 0.5) << "and back to its default with the rest";
}

// ---- The log frequency frame ------------------------------------------------------------------------------

TEST(LogFrequencyFrame, MapsALogAxisInItsRoomAndDrawsTheScreenWithTheDecadesGrid)
{
    // The EQ's graph's frame: the plot inside its scales; 20 Hz at its left edge and 20 kHz at its right, the geometric
    // mean at its centre and a decade the same width wherever it falls; the screen with its rim, a line at every
    // multiple of a decade's step inside the range (8 + 9 + 9 + 2) and the round numbers figured, only those the range
    // holds on a narrower frame; the figures' text.
    constexpr panel::LogFrequencyFrame frame{20.0, 20000.0, 34, 36, 16, 8, 5};
    const RectI                        local{600, 300};
    const RectF                        plot = frame.PlotBounds(local);
    EXPECT_EQ(plot.x(), 34.0f);
    EXPECT_EQ(plot.y(), 8.0f);
    EXPECT_EQ(plot.right(), 600.0f - 36.0f);
    EXPECT_EQ(plot.bottom(), 300.0f - 16.0f);
    EXPECT_NEAR(frame.XOf(plot, 20.0), plot.x(), 1.0e-3f);
    EXPECT_NEAR(frame.XOf(plot, 20000.0), plot.right(), 1.0e-3f);
    EXPECT_NEAR(frame.XOf(plot, std::sqrt(20.0 * 20000.0)), plot.CentreX(), 1.0e-3f);
    EXPECT_NEAR(frame.XOf(plot, 2000.0) - frame.XOf(plot, 200.0), frame.XOf(plot, 200.0) - frame.XOf(plot, 20.0),
                1.0e-3f);

    RecordingCanvas canvas;
    frame.DrawScreen(canvas, local.ToFloat(), plot, look::colors::kInk, Color(0xff080c12));
    ASSERT_EQ(canvas.rounded.size(), 1u) << "the rim";
    EXPECT_EQ(canvas.rounded[0].width(), 600.0f);
    EXPECT_EQ(canvas.vertical_lines, 28);
    EXPECT_EQ(canvas.texts,
              (std::vector<std::string>{"20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k"}));

    constexpr panel::LogFrequencyFrame narrow{100.0, 10000.0, 34, 36, 16, 8, 5};
    RecordingCanvas                    narrowed;
    narrow.DrawScreen(narrowed, local.ToFloat(), narrow.PlotBounds(local), look::colors::kInk, Color(0xff080c12));
    EXPECT_EQ(narrowed.vertical_lines, 19);
    EXPECT_EQ(narrowed.texts, (std::vector<std::string>{"100", "200", "500", "1k", "2k", "5k", "10k"}));

    EXPECT_EQ(panel::LogFrequencyFrame::HzLabel(999.0), "999");
    EXPECT_EQ(panel::LogFrequencyFrame::HzLabel(1500.0), "2k") << "whole kilohertz, half to even";
    EXPECT_EQ(panel::LogFrequencyFrame::DbLabel(6.0), "+6");
    EXPECT_EQ(panel::LogFrequencyFrame::DbLabel(0.0), "0");
    EXPECT_EQ(panel::LogFrequencyFrame::DbLabel(-12.0), "-12");
}

// ---- The clip indicators' bay ----------------------------------------------------------------------------------

namespace
{
/** A bay as the Amp's panel has one: a 12AX7, an EL34 and a lens in the series' cells, laid over a 360 x 220 panel in
    a window that records what is painted again. */
struct Bay
{
    Bay()
    {
        root.SetBounds({360, 220});
        root.SetHost(&window);
        root.AddChild(bay);
    }

    /** The captions the bay paints, in order: one a cell that shows. */
    std::vector<std::string> Captions()
    {
        RecordingCanvas canvas;
        bay.Paint(canvas);
        return canvas.texts;
    }

    /** The light a cell repaints: the cell and the halo round a tube at full glow, 0.8 of the tube's height from the
        heater, with a pixel to spare. */
    static RectI LightOf(RectI cell)
    {
        const int reach = static_cast<int>(std::ceil(0.8 * look::kBayTubeHeight)) + 1;
        return cell.Expanded(reach, reach);
    }

    const std::vector<RectI> cells{{40, 30, 96, 128}, {136, 30, 96, 128}, {232, 30, look::kLedCell, 128}};
    FakeWindow               window;
    view::View               root;
    panel::IndicatorBay      bay{look::colors::kAmber};
};

constexpr Color kGreen{0xff3fc46a};
constexpr Color kRed{0xffd8261e};
} // namespace

TEST(IndicatorBay, ItsCellsAreAddedInOrderAndLaidOverThePanelWhichKeepsTheMouse)
{
    // Each indicator is a cell after the ones added before it, its index handed back; the bay is laid over the whole
    // panel with the cells in the panel's coordinates, so a lit tube's halo is never clipped to its cell, and it takes
    // no mouse - the indicators are paint, not controls. Every indicator shows as it is added, the cells together the
    // area the bay's frame goes round, and each paints its caption under it.
    Bay f;
    EXPECT_EQ(f.bay.AddTube("STAGE 1", panel::IndicatorBay::Tube::k12AX7), 0u);
    EXPECT_EQ(f.bay.AddTube("OUTPUT", panel::IndicatorBay::Tube::kEL34), 1u);
    EXPECT_EQ(f.bay.AddLens("GATE"), 2u);
    ASSERT_EQ(f.bay.size(), 3u);
    f.bay.PlaceOver(f.root.LocalBounds(), f.cells);
    EXPECT_EQ(f.bay.bounds(), f.root.LocalBounds()) << "laid over the whole panel";
    for (std::size_t i = 0; i < f.cells.size(); ++i)
    {
        EXPECT_EQ(f.bay.CellOf(i), f.cells[i]) << "cell " << i << ", in the panel's coordinates";
        EXPECT_TRUE(f.bay.indicator_shown(i)) << "cell " << i << " shows as it is added";
        EXPECT_EQ(f.bay.glow(i), 0.0) << "cell " << i << " dark";
    }
    EXPECT_EQ(f.bay.CellOf(3), RectI{}) << "no such cell";
    EXPECT_FALSE(f.bay.indicator_shown(3));
    EXPECT_EQ(f.bay.CellsArea(), (RectI{40, 30, 192 + look::kLedCell, 128}));
    EXPECT_EQ(view::ViewAt(f.root, {88, 90}), &f.root) << "the mouse passes through the bay";
    EXPECT_EQ(f.Captions(), (std::vector<std::string>{"STAGE 1", "OUTPUT", "GATE"}));
}

TEST(IndicatorBay, ATubeFollowsItsTargetThroughTheFilamentAndALensTakesItsColour)
{
    // A tube steps toward its target through the series' filament - 20 ms up, 80 ms down - and repaints only its own
    // light, and only when its glow moved by more than the threshold; a lens shows the colour and glow it is set,
    // repainting when either changed. A step is a tube's alone and a colour a lens's alone: each ignores the other.
    Bay f;
    f.bay.AddTube("STAGE 1", panel::IndicatorBay::Tube::k12AX7);
    f.bay.AddTube("OUTPUT", panel::IndicatorBay::Tube::kEL34);
    f.bay.AddLens("GATE");
    f.bay.PlaceOver(f.root.LocalBounds(), f.cells);

    look::LampLight mirror(0.005, 0.0, look::LampFilament{});
    f.window.invalid.clear();
    mirror.Step(1.0, 1.0 / 60.0);
    f.bay.Step(0, 1.0, 1.0 / 60.0);
    EXPECT_DOUBLE_EQ(f.bay.glow(0), mirror.glow()) << "one frame of the filament's rise";
    EXPECT_DOUBLE_EQ(f.bay.glow(1), 0.0) << "the other tube untouched";
    ASSERT_EQ(f.window.invalid.size(), 1u);
    EXPECT_EQ(f.window.invalid[0], Bay::LightOf(f.cells[0])) << "its own light, and only that";
    for (int frame = 0; frame < 60; ++frame)
    {
        mirror.Step(1.0, 1.0 / 60.0);
        f.bay.Step(0, 1.0, 1.0 / 60.0);
    }
    EXPECT_DOUBLE_EQ(f.bay.glow(0), mirror.glow());
    EXPECT_GT(f.bay.glow(0), 0.99) << "lit";
    f.window.invalid.clear();
    f.bay.Step(0, 1.0, 1.0 / 60.0);
    EXPECT_TRUE(f.window.invalid.empty()) << "a glow that did not move is no repaint";
    for (int frame = 0; frame < 6; ++frame)
    {
        mirror.Step(0.0, 1.0 / 60.0);
        f.bay.Step(0, 0.0, 1.0 / 60.0);
    }
    EXPECT_DOUBLE_EQ(f.bay.glow(0), mirror.glow()) << "cooling at the filament's fall";
    EXPECT_GT(f.bay.glow(0), 0.2) << "slower down than up";

    f.window.invalid.clear();
    f.bay.Step(2, 1.0, 1.0 / 60.0);
    EXPECT_EQ(f.bay.glow(2), 0.0) << "a lens takes no step";
    const double tube = f.bay.glow(0);
    f.bay.SetLens(0, kGreen, 1.0);
    EXPECT_EQ(f.bay.glow(0), tube) << "a tube takes no colour";
    EXPECT_TRUE(f.window.invalid.empty());

    f.bay.SetLens(2, kGreen, 1.0);
    EXPECT_EQ(f.bay.glow(2), 1.0);
    ASSERT_EQ(f.window.invalid.size(), 1u);
    EXPECT_EQ(f.window.invalid[0], Bay::LightOf(f.cells[2]));
    f.bay.SetLens(2, kGreen, 1.0);
    EXPECT_EQ(f.window.invalid.size(), 1u) << "the same colour and glow again: no repaint";
    f.bay.SetLens(2, kRed, 1.0);
    EXPECT_EQ(f.window.invalid.size(), 2u) << "another colour is a repaint";
    RecordingCanvas canvas;
    f.bay.Paint(canvas);
    EXPECT_NE(std::find(canvas.ellipse_colours.begin(), canvas.ellipse_colours.end(), kRed.WithAlpha(0.35f)),
              canvas.ellipse_colours.end())
        << "the lens lit in its colour";
}

TEST(IndicatorBay, AnIndicatorLeftOutPaintsNothingAndComesBackAsItWouldHaveBeen)
{
    // The Amp's SIMPLE view leaves its gate LED out of the bay: an indicator left out paints nothing, is not in the
    // indicators' area and repaints its light once as it goes and once as it comes back; a tube left out keeps
    // following its target unseen, so it comes back at the glow it would have had, and a lens keeps the colour it was
    // set.
    Bay f;
    f.bay.AddTube("STAGE 1", panel::IndicatorBay::Tube::k12AX7);
    f.bay.AddTube("OUTPUT", panel::IndicatorBay::Tube::kEL34);
    f.bay.AddLens("GATE");
    f.bay.PlaceOver(f.root.LocalBounds(), f.cells);
    f.bay.SetLens(2, kGreen, 1.0);

    f.window.invalid.clear();
    f.bay.SetIndicatorShown(2, false);
    EXPECT_FALSE(f.bay.indicator_shown(2));
    ASSERT_EQ(f.window.invalid.size(), 1u);
    EXPECT_EQ(f.window.invalid[0], Bay::LightOf(f.cells[2])) << "its light painted out";
    f.bay.SetIndicatorShown(2, false);
    EXPECT_EQ(f.window.invalid.size(), 1u) << "left out already: nothing to repaint";
    EXPECT_EQ(f.bay.CellsArea(), (RectI{40, 30, 192, 128})) << "the cells shown: the two tubes'";
    EXPECT_EQ(f.Captions(), (std::vector<std::string>{"STAGE 1", "OUTPUT"})) << "the lens paints nothing";

    f.bay.SetIndicatorShown(0, false);
    look::LampLight mirror(0.005, 0.0, look::LampFilament{});
    f.window.invalid.clear();
    for (int frame = 0; frame < 12; ++frame)
    {
        mirror.Step(1.0, 1.0 / 60.0);
        f.bay.Step(0, 1.0, 1.0 / 60.0);
    }
    EXPECT_DOUBLE_EQ(f.bay.glow(0), mirror.glow()) << "a tube left out still follows its target";
    EXPECT_GT(f.bay.glow(0), 0.9);
    EXPECT_TRUE(f.window.invalid.empty()) << "and repaints nothing while it is out";
    EXPECT_EQ(f.Captions(), (std::vector<std::string>{"OUTPUT"}));

    f.bay.SetIndicatorShown(0, true);
    f.bay.SetIndicatorShown(2, true);
    EXPECT_EQ(f.window.invalid.size(), 2u) << "each painted back in";
    EXPECT_EQ(f.bay.CellsArea(), (RectI{40, 30, 192 + look::kLedCell, 128}));
    RecordingCanvas canvas;
    f.bay.Paint(canvas);
    EXPECT_EQ(canvas.texts, (std::vector<std::string>{"STAGE 1", "OUTPUT", "GATE"}));
    EXPECT_NE(std::find(canvas.ellipse_colours.begin(), canvas.ellipse_colours.end(), kGreen.WithAlpha(0.35f)),
              canvas.ellipse_colours.end())
        << "the lens back in the colour it was set";
}
