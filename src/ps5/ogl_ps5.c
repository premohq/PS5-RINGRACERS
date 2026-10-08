// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by Kart Krew.
// Copyright (C) 2020 by Sonic Team Junior.
// Copyright (C) 2000 by DooM Legacy Team.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps5/ogl_ps5.c
/// \brief PS5 part of the legacy OpenGL renderer
///
/// sdl/ogl_sdl.c and sdl/hwsym_sdl.c, on EGL. Both are smaller here because
/// the OpenGL implementation is linked into the title rather than loaded:
/// there is no library to open and no GLU beside it, and every entry point
/// comes from eglGetProcAddress.

#include <string.h>

#include "../doomdef.h"
#include "../d_main.h"
#include "../i_system.h"
#include "../m_argv.h"
#include "../screen.h"

#ifdef HWRENDER
#include "../hardware/r_opengl/r_opengl.h"
#include "../hardware/hw_main.h"
#include "../hardware/hw_drv.h"

#include "ogl_ps5.h"
#include "ps5_video.h"

#ifndef STATIC_OPENGL
PFNglClear pglClear;
PFNglGetIntegerv pglGetIntegerv;
PFNglGetString pglGetString;
#endif

int32_t oglflags = 0;

void *GetGLFunc(const char *proc)
{
	// No GLU on this platform; r_opengl copes with these being NULL.
	if (strncmp(proc, "glu", 3) == 0)
		return NULL;

	return PS5_GLGetProcAddress(proc);
}

dboolean LoadGL(void)
{
	return SetupGLfunc();
}

dboolean OglPs5Surface(int32_t w, int32_t h)
{
	int32_t cbpp = cv_scr_depth.value < 16 ? 16 : cv_scr_depth.value;
	static dboolean first_init = false;

	oglflags = 0;

	if (!first_init)
	{
		gl_version = pglGetString(GL_VERSION);
		gl_renderer = pglGetString(GL_RENDERER);
		gl_extensions = pglGetString(GL_EXTENSIONS);

		GL_DBG_Printf("OpenGL %s\n", gl_version);
		GL_DBG_Printf("GPU: %s\n", gl_renderer);
		GL_DBG_Printf("Extensions: %s\n", gl_extensions);
	}
	first_init = true;

	if (isExtAvailable("GL_EXT_texture_filter_anisotropic", gl_extensions))
		pglGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maximumAnisotropy);
	else
		maximumAnisotropy = 1;

	SetupGLFunc4();

	glanisotropicmode_cons_t[1].value = maximumAnisotropy;

	PS5_VideoSetSwapInterval(cv_vidwait.value ? 1 : 0);

	SetModelView(w, h);
	SetStates();
	pglClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

	HWR_Startup();
	textureformatGL = cbpp > 16 ? GL_RGBA : GL_RGB5_A1;

	return true;
}

void OglSdlFinishUpdate(dboolean waitvbl)
{
	static dboolean oldwaitvbl = false;
	int w, h;

	if (oldwaitvbl != waitvbl)
		PS5_VideoSetSwapInterval(waitvbl ? 1 : 0);

	oldwaitvbl = waitvbl;

	PS5_VideoDrawableSize(&w, &h);

	HWR_MakeScreenFinalTexture();
	HWR_DrawScreenFinalTexture(w, h);
	PS5_VideoPresent();

	GClipRect(0, 0, realwidth, realheight, NZCLIP_PLANE);

	// Sryder:	We need to draw the final screen texture again into the other buffer in the original position so that
	//			effects that want to take the old screen can do so after this
	HWR_DrawScreenFinalTexture(realwidth, realheight);
}

static void OglPs5SetPalette(RGBA_t *palette)
{
	size_t palsize = (sizeof(RGBA_t) * 256);
	// on a palette change, you have to reload all of the textures
	if (memcmp(&myPaletteData, palette, palsize))
	{
		memcpy(&myPaletteData, palette, palsize);
		Flush();
	}
}

#define GETFUNC(func) \
	else if (0 == strcmp(#func, funcName)) \
		funcPointer = FUNCPTRCAST(&func) \

void *hwSym(const char *funcName, void *handle)
{
	void *funcPointer = NULL;

	(void)handle;

	if (0 == strcmp("SetPalette", funcName))
		funcPointer = FUNCPTRCAST(&OglPs5SetPalette);
	GETFUNC(Init);
	GETFUNC(Draw2DLine);
	GETFUNC(DrawPolygon);
	GETFUNC(DrawIndexedTriangles);
	GETFUNC(RenderSkyDome);
	GETFUNC(SetBlend);
	GETFUNC(ClearBuffer);
	GETFUNC(SetTexture);
	GETFUNC(UpdateTexture);
	GETFUNC(DeleteTexture);
	GETFUNC(ReadRect);
	GETFUNC(GClipRect);
	GETFUNC(ClearMipMapCache);
	GETFUNC(SetSpecialState);
	GETFUNC(GetTextureUsed);
	GETFUNC(DrawModel);
	GETFUNC(CreateModelVBOs);
	GETFUNC(SetTransform);
	GETFUNC(PostImgRedraw);
	GETFUNC(FlushScreenTextures);
	GETFUNC(StartScreenWipe);
	GETFUNC(EndScreenWipe);
	GETFUNC(DoScreenWipe);
	GETFUNC(DrawIntermissionBG);
	GETFUNC(MakeScreenTexture);
	GETFUNC(MakeScreenFinalTexture);
	GETFUNC(DrawScreenFinalTexture);
	GETFUNC(CompileShaders);
	GETFUNC(CleanShaders);
	GETFUNC(SetShader);
	GETFUNC(UnSetShader);
	GETFUNC(SetShaderInfo);
	GETFUNC(LoadCustomShader);
	GETFUNC(ResetRenderState);

	if (!funcPointer)
		I_OutputMsg("hwSym for %s: not found\n", funcName);

	return funcPointer;
}

#endif // HWRENDER
