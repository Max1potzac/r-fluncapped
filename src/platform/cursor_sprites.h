#pragma once

// The glove cursor (assets/cursor.png): drawn for the controller's cursor, and optionally the
// mouse's in place of the system one.

#include "platform/key_hints.h"
#include "platform/png.h"

#include <vector>

namespace rfu::platform {

struct CursorSprite {
    const Image& image;
    int hot_x, hot_y;  // the fingertip, in the image's pixels
};

// The cursor, or the cursor while it presses (clicking).
CursorSprite cursor_sprite(bool pressed);

// The cursor scaled so the plain one is `height` pixels tall, with the fingertip at (x, y), as
// runs of one colour for scissored clears (its edges are cut at half transparency).
std::vector<HintFill> layout_cursor(const HintCursor& cursor);

// The sprite scaled to `height` (of the plain one), the hot spot scaled with it.
Image scaled_cursor(bool pressed, int height, int& hot_x, int& hot_y);

}  // namespace rfu::platform
