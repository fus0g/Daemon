// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app.options - command line options of the application.
export module daemon.app.options;
import std;

export namespace ide {

struct Options {
    std::vector<std::string> files;
    std::string screenshot;   // write a PNG after a few frames, then exit
    std::string keys;         // scripted input, e.g. "ctrl+f|t:foo|enter" (for tests and screenshots)
    int screenshotFrames = 4;
    float fontSize = 14;      // code font, logical pixels (em size)
};

// Release version ("0.1.0") and the full build identifier ("0.1.0+3f2a1bc" for development builds).
std::string_view version();
std::string_view versionFull();

}  // namespace ide
