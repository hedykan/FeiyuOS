#pragma once

// Runs gfx ops on a Gfx: the M5GFX calls of gfx_api (Kind::Call) and
// FeiyuOS's own (drawString with options, drawQr, drawImage, progressBar,
// rotate).  Canvases and program frames are painted with it band by band
// (ui_compose); a program's queries run its state ops on a "ruler" with it.
#include <vector>

#include "ui_canvas.h"
#include "ui_kit.h"

namespace vat::gfx_exec {

using ui::Gfx;
using Blobs = std::vector<std::vector<uint8_t>>;

// Bitmaps, pixels and image files of Call ops into `blobs` (their argument
// becomes "@n"); files are read here, so call it off the UI task.  The frame's
// total stays under gfx_api::kMaxBytes: false and `error` when it would not.
bool prepare(std::vector<ui_canvas::Op>& ops, Blobs& blobs, String& error);

// The state a frame starts from: font cn12, size 1, top_left, white text on a
// transparent background, colour white, no wrap, cursor 0,0.
void reset(Gfx& g);

// The canvas's one drawImage picture (ui_live reads it), when it has one.
struct Picture {
    const std::vector<uint8_t>* bytes = nullptr;
    int x = 0, y = 0;
};

// Every op in order (state ops last until changed).
void run(Gfx& g, const std::vector<ui_canvas::Op>& ops, const Blobs& blobs, const Picture& picture = {});

}  // namespace vat::gfx_exec
