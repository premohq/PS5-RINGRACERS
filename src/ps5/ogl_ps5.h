// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by Kart Krew.
// Copyright (C) 2020 by Sonic Team Junior.
// Copyright (C) 2000 by DooM Legacy Team.
// Copyright (C) 1996 by id Software, Inc.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps5/ogl_ps5.h
/// \brief PS5 part of the legacy OpenGL renderer
///
/// The counterpart of sdl/ogl_sdl.h. OglSdlFinishUpdate keeps its SDL name
/// because i_video_common.cpp calls it by that name on every platform that
/// builds the legacy renderer; renaming it would mean another edit to shared
/// code for no gain.

#ifndef SRB2_PS5_OGL_PS5_H
#define SRB2_PS5_OGL_PS5_H

#include <stdint.h>

#include "../doomtype.h"

#ifdef __cplusplus
extern "C" {
#endif

extern uint16_t realwidth;
extern uint16_t realheight;

/// Set up the legacy renderer's state for a mode of w x h.
dboolean OglPs5Surface(int32_t w, int32_t h);

/// Draw the legacy renderer's frame to the screen and present it.
void OglSdlFinishUpdate(dboolean vidwait);

#ifdef HWRENDER
dboolean LoadGL(void);
#endif

void *hwSym(const char *funcName, void *handle);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // SRB2_PS5_OGL_PS5_H
