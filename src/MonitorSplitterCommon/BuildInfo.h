#pragma once

#ifndef MONITORSPLITTER_PRODUCT_VERSION
#define MONITORSPLITTER_PRODUCT_VERSION "0.1.0"
#endif

#ifndef MONITORSPLITTER_BUILD_TAG
#define MONITORSPLITTER_BUILD_TAG MONITORSPLITTER_PRODUCT_VERSION "+" __DATE__ "-" __TIME__
#endif

#define MONITORSPLITTER_WIDEN2(value) L##value
#define MONITORSPLITTER_WIDEN(value) MONITORSPLITTER_WIDEN2(value)

namespace MonitorSplitter
{
static constexpr const char* kProductVersion = MONITORSPLITTER_PRODUCT_VERSION;
static constexpr const char* kBuildTag = MONITORSPLITTER_BUILD_TAG;
static constexpr const wchar_t* kProductVersionWide = MONITORSPLITTER_WIDEN(MONITORSPLITTER_PRODUCT_VERSION);
static constexpr const wchar_t* kBuildTagWide = MONITORSPLITTER_WIDEN(MONITORSPLITTER_BUILD_TAG);
}
