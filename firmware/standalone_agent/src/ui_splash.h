#pragma once

#include <Arduino.h>

// Boot screen, drawn straight to the display by its own task (stack and frames
// borrowed from the TLS reserve) so it keeps moving while the boot blocks:
//   1. bubbles at one edge of the screen, picked at random (drifting in and
//      rising; from the top sinking, parting and rising again), until the
//      card is mounted (at least 0.8 s);
//   2. the mascot enters from that edge, three times its size
//      (/vat/assets/boot.bin: enter_left/right/top/bottom, played once, each
//      frame at the offset it was drawn with; without it the corner mascot's
//      idle frames slide in), kept left of the title and above the loading line;
//   3. it plays its waiting loop (boot.bin "wait", or idle) until ready.
// "FeiyuOS" and the version sit on the right; along the bottom one loading
// line per boot step and a water line that fills with the progress, a small
// fish swimming at its front.
namespace vat::ui::splash {

void begin();         // before the card: starts the task (bubbles)
void card_ready();    // the card is mounted: reads the animation into RAM, starts the entrance
// Boot step `key` (card, config, packages, context, agent, radio, worker,
// wifi, time, ssh, web, ready): its loading line and the progress index / total.
void step(const char* key, int index, int total);
void problem(const String& text);  // a fault: the line in the warning colour
void finish();        // lets the entrance end, stops the task, frees the frame

}  // namespace vat::ui::splash
