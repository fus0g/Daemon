// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app.options implementation unit: build-time version information.
module daemon.app.options;
import std;

namespace ide {
std::string_view version() { return DAEMON_VERSION; }
std::string_view versionFull() { return DAEMON_VERSION_FULL; }
}  // namespace ide
