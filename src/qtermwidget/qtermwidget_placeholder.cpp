// Placeholder translation unit for the qtermwidget static library.
//
// The real C++ sources are being ported incrementally from the Python
// `qtermwidget` package (itself ported from upstream QTermWidget 2.2.0).
// This file lets the library target configure and link while that work is
// in progress. Delete it once the real sources listed in CMakeLists.txt are
// enabled.

namespace cubeshell {
namespace qtermwidget {

// Returns the upstream QTermWidget version this port is based on.
int placeholder_version() { return 0x020200; }

} // namespace qtermwidget
} // namespace cubeshell
