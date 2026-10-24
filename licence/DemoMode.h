#pragma once

#ifndef RECKLESS_DEMO_MODE
#define RECKLESS_DEMO_MODE 0
#endif

namespace reckless::core
{
/** Whether this build plays in demo mode while no licence holds: the configure's RECKLESS_DEMO_MODE, a compile
    definition every translation unit that reads it shares (the licence's target carries it), off until the suite is
    sold. What a demo build gates is the gate's business (core::DemoGate, beside the plugins' host layer); this is the
    one switch the gate, the formats and the licence's card read. */
inline constexpr bool kDemoModeBuild = RECKLESS_DEMO_MODE != 0;
} // namespace reckless::core
