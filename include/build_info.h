#pragma once

// The firmware's own name for itself, and when it was built.
//
// Both are DEFINED in a generated translation unit that scripts/pre_build_info.py
// writes under the build directory at the start of every cyd build — never
// into src/ or include/, so a build leaves the working tree clean and nothing
// here is ever checked in. The version is `git describe --tags --always
// --dirty`, or "unknown" when the tree is not a repository; the time is UTC,
// to the minute.
//
// One generated object recompiles when either changes. A -D flag in
// build_flags would have rebuilt every translation unit instead.
extern const char BUILD_FW_VERSION[];
extern const char BUILD_TIME_UTC[];
