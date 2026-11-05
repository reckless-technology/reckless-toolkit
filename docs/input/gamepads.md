# The controllers a program reads

A keyboard reaches a program through its window ([window/raw-keys.md](../window/raw-keys.md)). A controller does
not: a gamepad, a joystick, a flight stick, or an arcade encoder - the small board behind an arcade stick and its
buttons, which presents itself to the machine as a USB joystick - belongs to no window, and each platform has its own
way of reading one. The controller layer (`input/`, namespace `reckless::input`) reads them all the same way on
Linux, Windows and macOS, and gives a program one thing: **the edges of digital controls**.

That is a deliberate narrowing. A stick has a position and a trigger a pressure, and a program that steers by how far
a stick is pushed wants those; this layer does not give them. What it gives is what a button, a footswitch, an arcade
stick and a menu's cursor all are - something that is down or is not - and it gives it with the same promises the
raw keys keep, so a program can read a controller and a keyboard through one set of held controls.

What a control *means* is the program's business, not the toolkit's. Which action a control is bound to, how a person
chooses it, and where the choice is kept, are all the program's: the layer names the devices and their controls, tells
their edges, and says when a device comes and goes, and nothing else.

## The seam

[input/Gamepads.h](../../input/Gamepads.h) is the whole of what a program sees. It needs no platform's headers and,
but for `MakeGamepads`, is header only (`reckless_common_input`).

```cpp
#include "Gamepads.h"

auto gamepads = reckless::input::MakeGamepads({/*press_thousandths*/ 500, /*release_thousandths*/ 300});

// Once a frame, on the main thread:
std::vector<reckless::input::GamepadEdge> edges;
gamepads->Poll(edges);
for (const auto &edge : edges)
{
    controls.Heard(edge); // the program's own: what each control means to it
}
```

| Name | What it is |
|---|---|
| `GamepadControl` | one digital control of a device: a `GamepadControlKind` - a button, an axis's negative or positive half, a hat's up, down, left or right - and its index among the device's controls of that kind, from 0 |
| `GamepadControlName` | a control's name for a person, counting from 1 as a device's labels do: "Button 4", "Axis 2 +", "Axis 1 −", "Hat 1 left" |
| `GamepadEdge` | one edge: the device, the control, whether it went down or was let go, and when (`time_nanoseconds`, on `std::chrono::steady_clock` - [below](#when-an-edge-happened)) |
| `GamepadClockNanoseconds` | the steady clock now, as an edge's time counts it |
| `GamepadInfo` | one device: its id, its name, its stable id, and how many buttons, axes and hats it has |
| `GamepadAxisThresholds` | where an axis's controls go down and are let go, in thousandths of its travel - the program's, with no default here |
| `Gamepads` | the controllers: `Poll` tells every edge since the last call, and finds devices plugged in and pulled out; `Devices` lists them |
| `MakeGamepads` | the platform's controllers; never null - `NoGamepads`, which has none, where the build carries no back end |

Every call is made on one thread, the one the program polls from. A program on the app shell polls from its frame
callback, which runs on the main thread on every platform.

### Building it

The headers are always there, in `reckless_common_input`. The back ends are `reckless_input_devices`, behind the
option `RECKLESS_BUILD_INPUT`: on where the toolkit is built on its own, so its CI builds them; **off where a product
adds the toolkit**, so a product that reads no controller compiles none of it. A product that wants them sets the
option and links the target:

```cmake
set(RECKLESS_BUILD_INPUT ON)
add_subdirectory(external/reckless-toolkit)
target_link_libraries(my_product PRIVATE reckless_input_devices)
```

Nothing is fetched and no third party's code is linked: each back end is on its platform's own API alone.

## What a device promises

- **Every control is digital, and its edges alternate.** Every down is followed by exactly one up, and no up arrives
  for a control whose down did not. Every edge passes through the controls a device has said are down
  (`GamepadControlsHeld`), which is what holds this whatever the platform delivers - a report that repeats every
  value, a kernel's auto-repeat, a reading that changes nothing.
- **A device that goes lets go of everything.** A controller pulled out, or one whose node can no longer be read,
  delivers an up for every control it held, oldest first, before it leaves `Devices`. Nothing a program holds down
  can stay down.
- **A device that comes tells what it holds.** A button already pressed as the device appears is told as a down at
  the next poll; so is a hat pushed and a stick pushed past its threshold. This is where a controller differs from
  the raw keys, and it is because it can: a platform says what a controller holds when it is opened, where it cannot
  say which keys were down before a window had the keyboard.
- **An id is never given twice.** `device_id` is unique for the life of the process - one count for the whole binary,
  shared by every `Gamepads` in it - and never 0. A device unplugged and plugged in again is a new device with a new
  id; its stable id is what says it is the same one.
- **One reading, ups first.** Where one reading turns one control off and another on - a stick swung from one end to
  the other, a hat turned from up to right - the up is told before the down.
- **No tuning number is the toolkit's.** The thresholds are the program's (`GamepadAxisThresholds`, no default), as
  every tuning number in the toolkit is a product's. The one number the layer keeps is how often it looks for devices
  where its platform will not say when they change - once a second - which is not a feel but a fallback.

### When an edge happened

Every edge carries `time_nanoseconds`: nanoseconds since `std::chrono::steady_clock`'s epoch, so a program compares
it with its own `steady_clock::now()` - the time from a press reaching the toolkit to the tick that reads it is
`now - edge.time_nanoseconds`. It is the time the platform says the change happened, where the platform says, and
otherwise the time the toolkit first saw it. **One device's edges' times never go backwards.** 0 is a time not known,
which no back end here gives.

| Platform | Where the time comes from |
|---|---|
| Linux | the kernel's own stamp on each event, the node's clock set to `CLOCK_MONOTONIC` as it is opened (`EVIOCSCLOCKID`) - which is the steady clock in both libstdc++ and libc++. A node that refuses keeps the kernel's default wall clock, and its events are stamped as they are read instead |
| macOS | each value's own stamp (`IOHIDValueGetTimeStamp`), mach absolute time in the machine's ticks, turned into nanoseconds with `mach_timebase_info` and carried onto the steady clock by how long ago it was (`OnTheGamepadClock`): libc++'s steady clock on a Mac reads `CLOCK_MONOTONIC_RAW`, which counts time asleep and mach absolute time does not, so the two are not assumed to be one clock |
| Windows, XInput | the moment the back end's own thread read the state, which it does a thousand times a second ([below](#each-platform)) |
| Windows, HID | the moment `Poll` drained the report: HID gives no time of its own, so this is honest and no better than the poll |
| every platform | a device's controls held as it appears: when it was found; a device's ups as it goes: when it was found gone; the device read again after the kernel dropped events: the time of the report that ended the drop |

A back end stamps what a reading made after telling the device it (`StampEdges`), so `GamepadDevice` itself knows no
clock and the suites hold the times without one.

### An axis is two controls

An axis becomes two controls, its negative half and its positive half, each down past a threshold and let go under a
lower one. With a press of 500 and a release of 300, a stick's positive half goes down where the stick is pushed half
its travel to the right and is let go only where it comes back under three tenths: **hysteresis**, so a stick
resting near one threshold does not chatter. The arithmetic is in whole numbers throughout (`CentredDeflection`,
`OneSidedDeflection`, `IsAxisControlDown`), so every platform gives the same edges for the same readings.
`UsableThresholds` holds a press between 1 and 1000 and a release between 1 and the press.

The directions follow every platform's own convention, which is the USB HID one: **negative is left and up, positive
is right and down.** XInput alone reports a stick's vertical axis the other way, and its back end turns it round.

**Where an axis rests decides what its controls are.** A stick rests in the middle and has two halves. A trigger, a
pedal or a throttle rests at one end; judged as a stick, it would hold its negative half down for as long as the
device was plugged in. So each axis's rest is judged from the first reading taken of it, as the device appears
(`JudgeAxisRest`): in the middle, unless the reading is so far to one side that a stick's control would be down
there. An axis resting at its minimum has only its positive half, measured over its whole range; one resting at its
maximum has only its negative half. Where a platform says what an axis is - XInput's triggers - the back end gives
the rest and nothing is judged.

That judgement has one limit, and it is named rather than hidden: **a stick held hard over at the moment its device
appears is taken for an axis resting there.** It holds nothing down - no control is left stuck - and it has the one
control that moving away from that end gives, until the device is plugged in again. On Linux and macOS the reading is
taken as the device is opened; on Windows, from the device's first report, which most controllers send at once and a
few send only once something moves.

### A hat is four controls

A hat - a joystick's thumb switch, or a pad's cross where the platform reports it as one - is four controls, up,
right, down and left, two of them down at once on a diagonal. A USB HID hat switch counts its positions clockwise
from up, eight of them or four, and any value outside its range is the hat centred (`hid::HatOfSwitch`); Linux
reports each hat as two axes of its own, from -1 to 1.

### Names and stable ids

A device's `name` is what it calls itself. Its `stable_id` is what a settings file keeps to find it again on a later
launch: its USB vendor and product ids, four lower-case hexadecimal digits each (`0079:0006`), then a slash and what
the platform gives to tell two devices of that model apart.

| Platform | What follows the slash |
|---|---|
| Linux | the device's unique id where it has one - a serial number, a Bluetooth address - then `@` and the port it is plugged into (`usb-0000:00:14.0-2/input0`) |
| Windows, HID | the device's instance, which Windows makes from the port it is plugged into, or from a wireless device's own address |
| Windows, XInput | nothing: XInput gives neither a vendor nor a product, so its devices' ids are `xinput/<slot>` instead |
| macOS | the serial number where the device has one, then `@` and its location id in hexadecimal - the port |

The part before the slash names the model, so a program that does not find a device by its whole id can look for one
of the same model - two identical encoders moved between ports, a pad on another machine. A stable id is each
platform's own: the same device has a different one on each.

A control's index is the device's own, in an order fixed for the device: on Linux, its buttons from `BTN_JOYSTICK`
up and then those from `BTN_MISC`, as Linux's other programs number them; on Windows and macOS, its Button-page usages
in order; on every platform, its axes in the order of their codes or usages. Button 1 is the first button the
platform lists, which for an arcade encoder is usually the one wired to its first input.

## Each platform

| | Linux | Windows | macOS |
|---|---|---|---|
| API | the kernel's event interface (evdev), `/dev/input/event*` read-only and non-blocking | XInput (`xinput1_4`, loaded at run time); HID (`SetupDi`, `hid.dll`, overlapped `ReadFile`) | IOKit's HID manager on the main run loop |
| Which devices | those udev would tag `ID_INPUT_JOYSTICK`: a controller's buttons, or X and Y with nothing of a pointer, a touch surface or a pen; never a motion sensor | XInput's four slots; HID top-level collections that are a joystick, a game pad or a multi-axis controller (Generic Desktop 0x04, 0x05, 0x08), passing over XInput's (`IG_` in the path) | HID joysticks, game pads and multi-axis controllers, matched by the manager |
| Hot-plug | `/dev/input` watched with inotify; looked at once a second where inotify is not there | a configuration manager notification (`CM_Register_Notification`, no window needed); XInput's empty slots asked once a second | the manager's matching and removal callbacks |
| Where the work is done | in `Poll` | HID in `Poll`; XInput on a thread of the back end's own, queued for `Poll` | the callbacks queue on the run loop; `Poll` drains the queue |

Five things are worth knowing beyond the table.

**Linux grants access to a controller a moment after it appears.** The kernel makes the node owned by root; udev then
gives the seat's user access to it (its `uaccess` rule, for a device it tags `ID_INPUT_JOYSTICK`), which changes the
node's access list and so its inode's change time. A node that would not open is passed over until its stamp changes
(`EvdevNodeStamp`), and inotify's `IN_ATTRIB` says when it has, so a controller plugged in is opened at the poll after
udev has finished rather than never. A keyboard's node, which the user cannot open, is not tried again every second.

**Linux can drop events.** Where a program does not read a node for long enough, the kernel's buffer overruns and it
says so (`SYN_DROPPED`). What follows until the end of that frame is passed over, and the device's whole state is read
again (`EVIOCGKEY`, `EVIOCGABS`) and told as one reading, so a button let go in the events the kernel dropped is let
go here too.

**Windows has two APIs, and a pad must not appear on both.** An XInput pad is also a HID device, so the HID back end
passes over every path whose hardware id carries `IG_`, which is how Windows marks an XInput pad's HID interface; XInput
reads it instead, with its triggers as axes of their own (the HID view of an XInput pad folds both into one). A HID
controller's handle is given the deepest ring of reports the class driver keeps (`HidD_SetNumInputBuffers`, 512 where
it keeps 32 unless asked: measured on Legion5i on 2026-11-04, every one of the 23 HID interfaces it would open read 32
and took 512, and refused 513), so that a controller reporting every millisecond loses no report - no press - in a
stall of the program's thread of up to half a second, a window dragged or a menu open. A HID
descriptor that gives an unsigned field's maximum in too few bytes - 255 as the byte `0xFF`, which reads as -1 - is
taken at the field's whole range (`hid::RepairedLogicalRange`), as every driver that reads such devices takes it.

**XInput keeps no queue, so the back end keeps one.** `XInputGetState` says only what a pad holds now, and a press
let go between two reads is never seen: read once a frame, a tap shorter than a frame was lost. So XInput is read on
a thread of the back end's own, every slot that holds a pad once a millisecond on a high-resolution waitable timer -
pads report every 4 to 8 ms - and each read whose packet number has moved (`xinput::IsSlotNews`) is queued with the
time it was read; `Poll` takes the queue and tells it to the devices in order (`xinput::ApplySlotReadings`), so a tap
between two polls is a down and an up, each with its own time, and every promise above still holds. The empty slots,
whose read is slow, are still asked once a second - and at once when a HID interface arrives, which is how a pad being
plugged in shows itself. With no pad in any slot the thread sleeps between those looks. The slots are read once on the
program's thread as the back end is made, so `Devices` lists a pad before the first `Poll`, and the thread is stopped
and joined as the back end goes. A queue no one takes is bounded: past 4096 readings a slot's newest state is written
over its last queued one, and a pad's coming and going are always queued.

**macOS delivers nothing until its run loop has run.** The manager's callbacks run on the main run loop, so a
`Gamepads` made on macOS lists no devices until the run loop has turned at least once, and a program polls from its
frame callback, between turns. The callbacks only queue; `Poll` drains the queue in order, so a device's values,
its removal and a new device's arrival are told in the order the system gave them.

## What is held, and where

The policy needs no controller and no platform, and it is in headers no platform's headers reach for that reason, as
the raw keys' is ([window/RawKey.h](../../window/RawKey.h)):

- [input/GamepadState.h](../../input/GamepadState.h) - the controls a device has said are down, an axis's two halves
  through its thresholds, the judgement of where an axis rests, a hat's four directions, one device's readings
  turned into edges (`GamepadDevice`), the devices a back end has open (`GamepadRoster`), the stable id, and an
  edge's time: a reading's time given to the edges it made (`StampEdges`), a platform's ticks in nanoseconds
  (`NanosecondsOfTicks`) and a stamp on a platform's own clock carried onto the steady clock (`OnTheGamepadClock`).
- [input/HidUsages.h](../../input/HidUsages.h) - the HID arithmetic the Windows and macOS back ends share: which
  collections are controllers, which values are axes and which hats, a range a descriptor got wrong, a field's sign,
  and where a hat switch points.
- [input/linux/EvdevLayout.h](../../input/linux/EvdevLayout.h) and [EvdevGamepads.h](../../input/linux/EvdevGamepads.h)
  - the Linux back end whole, behind a port (`EvdevPort`) that is the only thing to touch the kernel; the kernel's
  numbers are copied, and [EvdevSystemPort.h](../../input/linux/EvdevSystemPort.h) holds every one to the kernel's own
  with a `static_assert`.
- [input/win/XInputPads.h](../../input/win/XInputPads.h) and [HidPaths.h](../../input/win/HidPaths.h) - an XInput
  pad's buttons, axes and cross, what the XInput thread finds to be news and queues, the queue told to the devices,
  and a HID path's XInput mark and instance.

The suites hold all of that on **every platform**, in the `input` CTest: `tests/test_gamepads.cpp` (the promises,
including a long deterministic walk of random readings held to alternation), `tests/test_gamepad_formats.cpp` (each
platform's arithmetic, evdev's from synthetic events) and `tests/test_evdev_gamepads.cpp` (the Linux back end on a port
of the suite's own: devices found as it is made and as they are plugged in, nodes passed over until they change, a
device pulled out while it holds controls, the kernel's dropped events, the look once a second, and every edge's
time - an event's own, the time read where it has none, and the device read again after a drop).

`tests/test_input_devices.cpp`, the `input_devices` CTest, runs the back end this build carries on the machine's own
controllers, whatever they are: made, polled and let go of, its devices well formed and its edges alternating. On
Linux it also makes a controller of its own with uinput and reads it from its plugging in to its pulling out, where
`/dev/uinput` is writable and the node the kernel makes for it is readable - and holds that each edge's time is the
kernel's own, between the event's sending and the poll that read it, on the steady clock; where either is not so -
most desks, and CI's runners - that case skips and says why. (Run as root on WSL2's 6.6 kernel on 2026-11-04, it
passed.)

What only a person with the hardware can hold is the Windows and macOS back ends on real controllers, and the
platform calls of each: they are compiled by CI on their platforms, and their decisions are the shared headers', but
nothing in a suite plugs a controller into them. The XInput thread's start, stop and join are held on any Windows by
`input_devices`, which makes and lets go of the back end twenty times over; that a light tap of a few milliseconds
on a real pad arrives as a down and an up in one `Poll`, each with its time, needs the pad.
