#pragma once

#include <string>

#include "Canvas.h"
#include "LookDrawing.h"
#include "PanelView.h"
#include "ParameterControl.h"
#include "SeriesPanel.h"

namespace reckless::app
{
/**
    What every view of an app on the toolkit draws with, made once a process: the series' context on Skia (its serif
    handed to Skia, images decoded and resampled, the files the binary carries - window/ModulePanel.h), the look's
    image backend on it, the company mark the header draws, and Steinberg's ASIO-compatible logo where the binary
    carries it. An app has no host parameters, so a control asked for by id is the still one.
*/
class AppResources
{
  public:
    /** The resources, the serif registered and the mark decoded from the binary's own files
        (RecklessRtLogo.png, and AsioCompatibleLogo.png where the binary carries it).
        @param version  the app's version, as a header stamps it */
    explicit AppResources(std::string version);

    /** Resources are not copied: the backend and the mark refer to the context. */
    AppResources(const AppResources &) = delete;
    /** Resources are not assigned.
        @return  never */
    AppResources &operator=(const AppResources &) = delete;

    /** @return  the series' context the views are made with */
    const view::PanelContext &context() const noexcept
    {
        return context_;
    }

    /** @return  the look's image backend on the context */
    const look::ImageBackend &images() const noexcept
    {
        return images_;
    }

    /** @return  the company mark, as the header draws it */
    const look::CompanyMark &mark() const noexcept
    {
        return mark_;
    }

    /** @return  Steinberg's ASIO-compatible logo, where the binary carries it (a Windows build with ASIO); invalid
                 elsewhere */
    const gfx::Image &asio_logo() const noexcept
    {
        return asio_logo_;
    }

    /** @return  the app's version, "MAJOR.MINOR.BUILD" */
    const std::string &version() const noexcept
    {
        return context_.version;
    }

  private:
    view::StillControl   still_;     ///< the control for any id: an app has no host parameters
    view::PanelContext   context_;   ///< the series' context on Skia and the binary's files
    panel::ContextImages images_;    ///< the look's image backend on the context
    look::CompanyMark    mark_;      ///< the company mark
    gfx::Image           asio_logo_; ///< the ASIO-compatible logo, or an invalid image
};
} // namespace reckless::app
