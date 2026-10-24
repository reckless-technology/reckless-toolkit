#include "AppResources.h"

#include <string_view>
#include <utility>

#include "Embedded.h"
#include "ModulePanel.h"

namespace reckless::app
{
namespace
{
/** The series' context on Skia, every parameter id answered by `still`.
    @param still    the control every id is answered with; outlives the context
    @param version  the version a header stamps
    @return         the context */
view::PanelContext MakeContext(view::StillControl &still, std::string version)
{
    return window::ModulePanelContext([&still](std::string_view) -> view::ParameterControl & { return still; },
                                      std::move(version));
}
} // namespace

AppResources::AppResources(std::string version)
    : context_(MakeContext(still_, std::move(version))), images_(context_),
      mark_(images_, context_.decode_image(embedded::Find("RecklessRtLogo.png")))
{
    if (const auto logo = embedded::Find("AsioCompatibleLogo.png"); !logo.empty())
    {
        asio_logo_ = context_.decode_image(logo);
    }
}
} // namespace reckless::app
