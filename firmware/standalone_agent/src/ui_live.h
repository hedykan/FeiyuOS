#pragma once

// A panel or canvas on screen, and its source re-run while it stays open:
// shared by the ui package (panels, local apps) and the gfx package
// (canvases).  Only the one shown last refreshes.
#include "ui_canvas.h"
#include "ui_panel.h"

namespace vat::ui_live {

void show_panel(const String& name, const ui_panel::Spec& spec);
void show_canvas(const String& name, const ui_canvas::Spec& spec);
// From a package's loop(): queues the refresh of the live source when it is due.
void loop();

}  // namespace vat::ui_live
