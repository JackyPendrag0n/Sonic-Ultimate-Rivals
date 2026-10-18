// Shim for PPSSPP's Core/Reporting.h (rcsm GPU extraction): reporting disabled. ShouldLogNTimes and
// ReportMessage (declared in Common/LogReporting.h) are implemented in stubs/stubs.cpp. GPL-2.0+.
#pragma once
#include <string>
#include <cstdint>
#include "Common/CommonTypes.h"
#include "Common/LogReporting.h"
namespace Reporting {
	inline void NotifyDebugger() {}
	inline bool IsEnabled() { return false; }
	inline void UpdateConfig() {}
}
