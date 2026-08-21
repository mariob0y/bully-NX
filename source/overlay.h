/* overlay.h -- lightweight on-screen FPS/resolution overlay
 *
 * Copyright (C) 2026 Andrii Romaniuk
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __OVERLAY_H__
#define __OVERLAY_H__

// Call once per frame, right before the real eglSwapBuffers() call, while
// the game's GL context is still current. Draws a small "NN FPS  WxH" panel
// in a screen corner directly onto the already-rendered frame, then restores
// every bit of GL state it touched so the game's own rendering is unaffected.
//
// Stays inactive until enough frames have gone by (see OVERLAY_WARMUP_SWAPS
// in overlay.c) so it never competes with the game's own startup/loading GL
// setup for state or GPU time.
void overlay_render(void);

#endif
