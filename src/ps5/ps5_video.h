// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps5/ps5_video.h
/// \brief The EGL display, surface and context, shared by both renderers

#ifndef SRB2_PS5_VIDEO_H
#define SRB2_PS5_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

/// Present the back buffer.
void PS5_VideoPresent(void);

/// 1 to wait for the display's refresh, 0 not to.
void PS5_VideoSetSwapInterval(int interval);

/// The size of the presented image: the display mode chosen at startup.
void PS5_VideoDrawableSize(int *width, int *height);

/// Look up an OpenGL entry point.
void *PS5_GLGetProcAddress(const char *name);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // SRB2_PS5_VIDEO_H
