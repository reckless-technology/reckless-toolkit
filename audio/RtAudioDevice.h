#pragma once

// The name a product's sources included the device layer's functions by while RtAudio served them - AudioApis,
// DefaultAudioApi, MakeAudioSystem, MakeMidiSystem and AudioLibraryVersions - which are DeviceBackends.h's and come in
// with it. Kept so that a product including it builds unchanged; a product includes DeviceBackends.h instead, and this
// goes once none includes it.
#include "DeviceBackends.h"
