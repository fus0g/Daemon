// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.ui.geometry - basic geometry used by the UI framework.
export module daemon.ui.geometry;
import std;

export namespace ide {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    float right() const { return x + w; }
    float bottom() const { return y + h; }
};

}  // namespace ide
