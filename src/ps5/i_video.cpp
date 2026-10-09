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
/// \file  ps5/i_video.cpp
/// \brief Video interface - PlayStation 5
///
/// sdl/i_video.cpp with EGL in place of SDL's window and GL context. The
/// OpenGL underneath is ps5-opengl (Mesa over the console's own graphics
/// driver), and the game drives it exactly as the PC build does: the GL2 RHI
/// for everything, plus the legacy OpenGL renderer when it is selected.
///
/// What a console changes is the window. There is one surface, the size of
/// the display mode picked at startup (4K unless the player asks for 1080p
/// or 1440p, see ParseDisplayMode), and it never moves or resizes. The
/// game's resolution setting still works: it sets the size the game renders
/// at, and the RHI scales that to the surface, which is what the PC build
/// does in fullscreen too.

#include <cstdlib>
#include <cstring>
#include <memory>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <ps5_opengl_display_modes.h>

#include <imgui.h>

#include "../rhi/rhi.hpp"
#include "../rhi/gl2/gl2_rhi.hpp"
#include "rhi_gl2_platform.hpp"

#include "../doomdef.h"
#include "../doomstat.h"
#include "../i_system.h"
#include "../v_video.h"
#include "../m_argv.h"
#include "../k_menu.h"
#include "../d_main.h"
#include "../s_sound.h"
#include "../i_sound.h"
#include "../i_joy.h"
#include "../st_stuff.h"
#include "../hu_stuff.h"
#include "../g_game.h"
#include "../g_input.h"
#include "../i_video.h"
#include "../console.h"
#include "../command.h"
#include "../r_main.h"
#include "../lua_hook.h"
#ifdef HWRENDER
#include "../hardware/hw_main.h"
#include "../hardware/hw_drv.h"
#include "ogl_ps5.h"
#endif

#include "ps5_sys.h"
#include "ps5_video.h"

using namespace srb2;

#define MAXWINMODES (21)

static char vidModeName[33][32];

rendermode_t rendermode = render_soft;
rendermode_t chosenrendermode = render_none;

uint8_t graphics_started = 0;
dboolean allow_fullscreen = false;

uint16_t realwidth = BASEVIDWIDTH;
uint16_t realheight = BASEVIDHEIGHT;

static bool exposevideo = false;
static const char* fallback_resolution_name = "Fallback";

static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLSurface g_surface = EGL_NO_SURFACE;
static EGLContext g_context = EGL_NO_CONTEXT;
static EGLint g_surface_width = 1920;
static EGLint g_surface_height = 1080;
static uint32_t g_refresh_rate = 60;
static int g_swap_interval = -1;

static std::unique_ptr<rhi::Rhi> g_rhi;
static uint32_t g_rhi_generation = 0;

// The PC build's list, so the resolution menu and saved configs mean the same
// thing on both, under the console's two larger display modes, which the
// engine renders at here (MAXVIDWIDTH and MAXVIDHEIGHT in screen.h).
static int32_t windowedModes[MAXWINMODES][2] =
{
	{3840,2160}, // 1.66
	{2560,1440}, // 1.66
	{1920,1200}, // 1.60,6.00
	{1920,1080}, // 1.66
	{1680,1050}, // 1.60,5.25
	{1600,1200}, // 1.33
	{1600, 900}, // 1.66
	{1366, 768}, // 1.66
	{1440, 900}, // 1.60,4.50
	{1280,1024}, // 1.33?
	{1280, 960}, // 1.33,4.00
	{1280, 800}, // 1.60,4.00
	{1280, 720}, // 1.66
	{1152, 864}, // 1.33,3.60
	{1024,1024}, // SPECIAL, for snapshot taker
	{1024, 768}, // 1.33,3.20
	{ 800, 600}, // 1.33,2.50
	{ 640, 480}, // 1.33,2.00
	{ 640, 400}, // 1.60,2.00
	{ 320, 240}, // 1.33,1.00
	{ 320, 200}, // 1.60,1.00
};

// ---------------------------------------------------------------------------
// EGL
// ---------------------------------------------------------------------------

void PS5_VideoPresent(void)
{
	if (g_display != EGL_NO_DISPLAY && g_surface != EGL_NO_SURFACE)
		eglSwapBuffers(g_display, g_surface);
}

void PS5_VideoSetSwapInterval(int interval)
{
	if (g_display == EGL_NO_DISPLAY || interval == g_swap_interval)
		return;
	if (eglSwapInterval(g_display, interval))
		g_swap_interval = interval;
}

void PS5_VideoDrawableSize(int* width, int* height)
{
	*width = g_surface_width;
	*height = g_surface_height;
}

void* PS5_GLGetProcAddress(const char* name)
{
	return reinterpret_cast<void*>(eglGetProcAddress(name));
}

static rhi::GlProc PS5_GLLoad(const char* name)
{
	return reinterpret_cast<rhi::GlProc>(eglGetProcAddress(name));
}

/// The display mode, from the command line (see i_main.cpp for where a
/// player puts arguments): -ps5res 1080|1440|2160 and -ps5hz 60|120.
///
/// ps5-opengl can only change the mode while EGL is down, and every GL object
/// is lost when it does, so this is chosen once, at startup, and not offered
/// in the video menu. 4K is the default: the console scales what it is given
/// to the TV's own mode, so a 4K surface is shown as it is on a 4K TV and
/// downsampled on a 1080p one, and the video menu's resolutions, up to
/// 3840x2160, all fit in it.
static void ParseDisplayMode(EGLint* width, EGLint* height, EGLint* hz)
{
	*width = 3840;
	*height = 2160;
	*hz = 60;

	if (M_CheckParm("-ps5res") && M_IsNextParm())
	{
		const int lines = atoi(M_GetNextParm());
		if (lines == 1080)
		{
			*width = 1920;
			*height = 1080;
		}
		else if (lines == 1440)
		{
			*width = 2560;
			*height = 1440;
		}
	}

	if (M_CheckParm("-ps5hz") && M_IsNextParm())
	{
		if (atoi(M_GetNextParm()) == 120)
			*hz = 120;
	}
}

static bool InitEGL(void)
{
	static const EGLint config_attributes[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24,
		EGL_STENCIL_SIZE, 8,
		EGL_NONE,
	};

	// The GL2 RHI and the legacy renderer are written for OpenGL 2.1 and
	// GLSL 1.20, which only a compatibility profile accepts. ps5-opengl
	// creates those, though only its core profile has been through the
	// conformance suite; so ask for exactly what the PC build asks SDL for,
	// then for a newer compatibility context, before giving up.
	static const EGLint context_attempts[][7] = {
		{EGL_CONTEXT_MAJOR_VERSION_KHR, 2, EGL_CONTEXT_MINOR_VERSION_KHR, 1,
			EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR, EGL_NONE},
		{EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 3,
			EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR, EGL_NONE},
		{EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 6,
			EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR, EGL_NONE},
	};

	EGLint width, height, hz;
	ParseDisplayMode(&width, &height, &hz);

	g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (g_display == EGL_NO_DISPLAY)
	{
		CONS_Alert(CONS_ERROR, "eglGetDisplay failed\n");
		return false;
	}

	if (!eglSetDisplayModePS5(g_display, width, height))
	{
		CONS_Alert(CONS_WARNING, "Display mode %dx%d refused; using 1920x1080\n", width, height);
		eglSetDisplayModePS5(g_display, 1920, 1080);
	}
	eglSetDisplayRefreshPS5(g_display, hz);

	EGLint major = 0, minor = 0;
	if (!eglInitialize(g_display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API))
	{
		CONS_Alert(CONS_ERROR, "eglInitialize failed: 0x%x\n", eglGetError());
		return false;
	}

	EGLConfig config = nullptr;
	EGLint count = 0;
	if (!eglChooseConfig(g_display, config_attributes, &config, 1, &count) || count < 1)
	{
		CONS_Alert(CONS_ERROR, "eglChooseConfig found nothing: 0x%x\n", eglGetError());
		return false;
	}

	// ps5-opengl's native window handle is always zero: there is one screen.
	g_surface = eglCreateWindowSurface(g_display, config, static_cast<EGLNativeWindowType>(0), nullptr);
	if (g_surface == EGL_NO_SURFACE)
	{
		CONS_Alert(CONS_ERROR, "eglCreateWindowSurface failed: 0x%x\n", eglGetError());
		return false;
	}

	for (const EGLint* attributes : context_attempts)
	{
		g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, attributes);
		if (g_context != EGL_NO_CONTEXT)
		{
			CONS_Printf("OpenGL %d.%d compatibility context\n", attributes[1], attributes[3]);
			break;
		}
	}
	if (g_context == EGL_NO_CONTEXT)
	{
		CONS_Alert(CONS_ERROR, "eglCreateContext failed: 0x%x\n", eglGetError());
		return false;
	}

	if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context))
	{
		CONS_Alert(CONS_ERROR, "eglMakeCurrent failed: 0x%x\n", eglGetError());
		return false;
	}

	eglQuerySurface(g_display, g_surface, EGL_WIDTH, &g_surface_width);
	eglQuerySurface(g_display, g_surface, EGL_HEIGHT, &g_surface_height);

	// What was asked for; QueryRefreshRate corrects it after the first frame.
	g_refresh_rate = static_cast<uint32_t>(hz);

	CONS_Printf("EGL %d.%d, display %dx%d, asked for %d Hz\n", major, minor,
		g_surface_width, g_surface_height, hz);

	return true;
}

int PS5_VideoStart(void)
{
	if (g_display != EGL_NO_DISPLAY && g_context != EGL_NO_CONTEXT)
		return 1;
	return InitEGL() ? 1 : 0;
}

/// The display accepts or refuses 120 Hz only once a frame has been presented
/// (ps5-opengl's docs/display-modes.md); until then eglGetDisplayModePS5
/// reports the rate asked for. Asked again after the first present, so a TV
/// that cannot do 120 Hz is not reported to the frame rate cap as one.
static void QueryRefreshRate(void)
{
	EGLint refresh = 0;
	if (eglGetDisplayModePS5(g_display, nullptr, nullptr, &refresh) && refresh > 0)
		g_refresh_rate = static_cast<uint32_t>(refresh);
}

static void ShutdownEGL(void)
{
	if (g_display == EGL_NO_DISPLAY)
		return;

	eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (g_context != EGL_NO_CONTEXT)
		eglDestroyContext(g_display, g_context);
	if (g_surface != EGL_NO_SURFACE)
		eglDestroySurface(g_display, g_surface);
	eglTerminate(g_display);

	g_context = EGL_NO_CONTEXT;
	g_surface = EGL_NO_SURFACE;
	g_display = EGL_NO_DISPLAY;
	g_swap_interval = -1;
}

// ---------------------------------------------------------------------------
// The engine's video interface
// ---------------------------------------------------------------------------

static void init_imgui()
{
	if (ImGui::GetCurrentContext() != NULL)
		return;

	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = NULL;
	io.BackendFlags = ImGuiBackendFlags_RendererHasTextures;
	io.BackendRendererName = "SRB2 PS5 GL2 RHI";
	io.DisplaySize = ImVec2(static_cast<float>(g_surface_width), static_cast<float>(g_surface_height));
	io.Fonts->AddFontDefault();
	ImGui::StyleColorsDark();
}

static bool CreateRhi(void)
{
	init_imgui();

#ifdef HWRENDER
	if (rendermode == render_opengl)
		LoadGL();
#endif

	if (!g_rhi)
	{
		std::unique_ptr<rhi::Ps5Gl2Platform> platform = std::make_unique<rhi::Ps5Gl2Platform>();
		g_rhi = std::make_unique<rhi::Gl2Rhi>(std::move(platform), PS5_GLLoad);
		g_rhi_generation += 1;
	}

	return true;
}

static void SetMode(void)
{
	realwidth = vid.width;
	realheight = vid.height;

#ifdef HWRENDER
	if (rendermode == render_opengl)
		OglPs5Surface(vid.width, vid.height);
	else
#endif
		PS5_VideoSetSwapInterval(cv_vidwait.value ? 1 : 0);

	// The surface is the screen; the game's mode is what is scaled onto it.
	vid.realwidth = static_cast<uint32_t>(g_surface_width);
	vid.realheight = static_cast<uint32_t>(g_surface_height);

	if (graphics_started)
		I_UpdateNoVsync();
}

static void VideoSetupBuffer(void)
{
	vid.rowbytes = vid.width * vid.bpp;
	vid.direct = NULL;
	if (vid.buffer)
		free(vid.buffer);
	vid.buffer = static_cast<uint8_t*>(calloc(vid.rowbytes * vid.height, NUMSCREENS));
	if (!vid.buffer)
		I_Error("%s", M_GetText("Not enough memory for video buffer\n"));
}

static void VID_Command_NumModes_f(void)
{
	CONS_Printf(M_GetText("%d video mode(s) available(s)\n"), VID_NumModes());
}

static void VID_Command_Info_f(void)
{
	CONS_Printf("\x82" "Current Engine Mode\n %dx%d\n", vid.width, vid.height);
	CONS_Printf("\x82" "Display\n %dx%d at %u Hz\n", g_surface_width, g_surface_height, g_refresh_rate);
}

static void VID_Command_ModeList_f(void)
{
	for (int32_t i = 0; i < MAXWINMODES; i++)
		CONS_Printf("%2d: %dx%d\n", i, windowedModes[i][0], windowedModes[i][1]);
}

static void VID_Command_Mode_f(void)
{
	if (COM_Argc() != 2)
	{
		CONS_Printf(M_GetText("vid_mode <modenum> : set video mode, current video mode %i\n"), vid.modenum);
		return;
	}

	const int32_t modenum = atoi(COM_Argv(1));

	if (modenum >= VID_NumModes())
		CONS_Printf(M_GetText("Video mode not present\n"));
	else
		setmodeneeded = modenum + 1;
}

void I_UpdateNoBlit(void)
{
	if (rendermode == render_none)
		return;
	if (exposevideo)
	{
#ifdef HWRENDER
		if (rendermode == render_opengl)
			OglSdlFinishUpdate(cv_vidwait.value);
#endif
	}
	exposevideo = false;
}

void I_UpdateNoVsync(void)
{
	const int32_t real_vidwait = cv_vidwait.value;
	cv_vidwait.value = 0;
	I_FinishUpdate();
	cv_vidwait.value = real_vidwait;
}

void I_ReadScreen(uint8_t* scr)
{
	if (rendermode == render_opengl)
		I_Error("I_ReadScreen: called while in Legacy GL mode");
	else
		VID_BlitLinearScreen(screens[0], scr, vid.width * vid.bpp, vid.height, vid.rowbytes, vid.rowbytes);
}

void I_SetPalette(RGBA_t* palette)
{
	(void)palette;
}

int32_t VID_NumModes(void)
{
	return MAXWINMODES;
}

const char* VID_GetModeName(int32_t modeNum)
{
	if (modeNum == -1)
		return fallback_resolution_name;
	if (modeNum < 0 || modeNum >= MAXWINMODES)
		return NULL;

	snprintf(&vidModeName[modeNum][0], sizeof(vidModeName[modeNum]), "%dx%d",
		windowedModes[modeNum][0], windowedModes[modeNum][1]);
	return &vidModeName[modeNum][0];
}

int32_t VID_GetModeForSize(int32_t w, int32_t h)
{
	for (int32_t i = 0; i < MAXWINMODES; i++)
	{
		if (windowedModes[i][0] == w && windowedModes[i][1] == h)
			return i;
	}
	return -1;
}

void VID_PrepareModeList(void)
{
	allow_fullscreen = true;
}

void VID_CheckGLLoaded(rendermode_t oldrender)
{
	(void)oldrender;
}

dboolean VID_CheckRenderer(void)
{
	dboolean rendererchanged = false;

	if (dedicated)
		return false;

	if (setrenderneeded)
	{
		rendermode = static_cast<rendermode_t>(setrenderneeded);
		rendererchanged = true;

		CreateRhi();
#ifdef HWRENDER
		if (rendermode == render_opengl)
		{
			VID_StartupOpenGL();
			if (vid.glstate != VID_GL_LIBRARY_LOADED)
				rendererchanged = false;
		}
#endif

		setrenderneeded = 0;
	}

	SetMode();
	VideoSetupBuffer();

	if (rendermode == render_soft)
	{
		SCR_SetDrawFuncs();
	}
#ifdef HWRENDER
	else if (rendermode == render_opengl && rendererchanged)
	{
		HWR_Switch();
		V_SetPalette(0);
	}
#endif

	M_RefreshAdvancedVideoOptions();

	return rendererchanged;
}

int32_t VID_SetMode(int32_t modeNum)
{
	vid.recalc = 1;
	vid.bpp = 1;

	if (modeNum < 0)
		modeNum = 0;
	if (modeNum >= MAXWINMODES)
		modeNum = MAXWINMODES - 1;

	vid.width = windowedModes[modeNum][0];
	vid.height = windowedModes[modeNum][1];
	vid.realwidth = vid.width;
	vid.realheight = vid.height;
	vid.modenum = modeNum;

	VID_CheckRenderer();
	return true;
}

extern "C" CVarList* cvlist_graphics_driver;

void I_StartupGraphics(void)
{
	if (dedicated)
	{
		rendermode = render_none;
		return;
	}
	if (graphics_started)
		return;

	COM_AddCommand("vid_nummodes", VID_Command_NumModes_f);
	COM_AddCommand("vid_info", VID_Command_Info_f);
	COM_AddCommand("vid_modelist", VID_Command_ModeList_f);
	COM_AddCommand("vid_mode", VID_Command_Mode_f);
	CV_RegisterList(cvlist_graphics_driver);

	keyboard_started = true;

	if (M_CheckParm("-renderer"))
	{
		int32_t i = 0;
		CV_PossibleValue_t* renderer_list = cv_renderer_t;
		const char* modeparm = M_GetNextParm();
		while (renderer_list[i].strvalue)
		{
			if (!stricmp(modeparm, renderer_list[i].strvalue))
			{
				chosenrendermode = static_cast<rendermode_t>(renderer_list[i].value);
				break;
			}
			i++;
		}
	}
	else if (M_CheckParm("-software"))
		chosenrendermode = render_soft;
#ifdef HWRENDER
	else if (M_CheckParm("-opengl"))
		chosenrendermode = render_opengl;

	if (M_CheckParm("-nogl"))
	{
		vid.glstate = VID_GL_LIBRARY_ERROR;
		if (chosenrendermode == render_opengl)
			chosenrendermode = render_none;
	}
#endif

	if (chosenrendermode != render_none)
		rendermode = chosenrendermode;

	if (!PS5_VideoStart())
		I_Error("Could not start OpenGL on this console.\nSee ringracers-stdout.txt for details.");

	CreateRhi();

#ifdef HWRENDER
	if (rendermode == render_opengl)
		VID_StartupOpenGL();
#endif

	VID_SetMode(VID_GetModeForSize(BASEVIDWIDTH, BASEVIDHEIGHT));

	vid.width = BASEVIDWIDTH;
	vid.height = BASEVIDHEIGHT;
	vid.recalc = true;
	vid.direct = NULL;
	vid.bpp = 1;

	VID_SetMode(VID_GetModeForSize(BASEVIDWIDTH, BASEVIDHEIGHT));

	realwidth = static_cast<uint16_t>(vid.width);
	realheight = static_cast<uint16_t>(vid.height);

	graphics_started = true;

	PS5_VideoPresent();
	QueryRefreshRate();

	VID_Command_Info_f();
}

void VID_StartupOpenGL(void)
{
#ifdef HWRENDER
	static dboolean glstartup = false;
	if (!glstartup)
	{
		CONS_Printf("VID_StartupOpenGL()...\n");
		*(void**)&HWD.pfnInit             = hwSym("Init",NULL);
		*(void**)&HWD.pfnFinishUpdate     = NULL;
		*(void**)&HWD.pfnDraw2DLine       = hwSym("Draw2DLine",NULL);
		*(void**)&HWD.pfnDrawPolygon      = hwSym("DrawPolygon",NULL);
		*(void**)&HWD.pfnDrawIndexedTriangles = hwSym("DrawIndexedTriangles",NULL);
		*(void**)&HWD.pfnRenderSkyDome    = hwSym("RenderSkyDome",NULL);
		*(void**)&HWD.pfnSetBlend         = hwSym("SetBlend",NULL);
		*(void**)&HWD.pfnClearBuffer      = hwSym("ClearBuffer",NULL);
		*(void**)&HWD.pfnSetTexture       = hwSym("SetTexture",NULL);
		*(void**)&HWD.pfnUpdateTexture    = hwSym("UpdateTexture",NULL);
		*(void**)&HWD.pfnDeleteTexture    = hwSym("DeleteTexture",NULL);
		*(void**)&HWD.pfnReadRect         = hwSym("ReadRect",NULL);
		*(void**)&HWD.pfnGClipRect        = hwSym("GClipRect",NULL);
		*(void**)&HWD.pfnClearMipMapCache = hwSym("ClearMipMapCache",NULL);
		*(void**)&HWD.pfnSetSpecialState  = hwSym("SetSpecialState",NULL);
		*(void**)&HWD.pfnSetPalette       = hwSym("SetPalette",NULL);
		*(void**)&HWD.pfnGetTextureUsed   = hwSym("GetTextureUsed",NULL);
		*(void**)&HWD.pfnDrawModel        = hwSym("DrawModel",NULL);
		*(void**)&HWD.pfnCreateModelVBOs  = hwSym("CreateModelVBOs",NULL);
		*(void**)&HWD.pfnSetTransform     = hwSym("SetTransform",NULL);
		*(void**)&HWD.pfnPostImgRedraw    = hwSym("PostImgRedraw",NULL);
		*(void**)&HWD.pfnFlushScreenTextures=hwSym("FlushScreenTextures",NULL);
		*(void**)&HWD.pfnStartScreenWipe  = hwSym("StartScreenWipe",NULL);
		*(void**)&HWD.pfnEndScreenWipe    = hwSym("EndScreenWipe",NULL);
		*(void**)&HWD.pfnDoScreenWipe     = hwSym("DoScreenWipe",NULL);
		*(void**)&HWD.pfnDrawIntermissionBG=hwSym("DrawIntermissionBG",NULL);
		*(void**)&HWD.pfnMakeScreenTexture= hwSym("MakeScreenTexture",NULL);
		*(void**)&HWD.pfnMakeScreenFinalTexture=hwSym("MakeScreenFinalTexture",NULL);
		*(void**)&HWD.pfnDrawScreenFinalTexture=hwSym("DrawScreenFinalTexture",NULL);

		*(void**)&HWD.pfnCompileShaders   = hwSym("CompileShaders",NULL);
		*(void**)&HWD.pfnCleanShaders     = hwSym("CleanShaders",NULL);
		*(void**)&HWD.pfnSetShader        = hwSym("SetShader",NULL);
		*(void**)&HWD.pfnUnSetShader      = hwSym("UnSetShader",NULL);

		*(void**)&HWD.pfnSetShaderInfo    = hwSym("SetShaderInfo",NULL);
		*(void**)&HWD.pfnLoadCustomShader = hwSym("LoadCustomShader",NULL);
		*(void**)&HWD.pfnResetRenderState = hwSym("ResetRenderState",NULL);
		glstartup = true;
	}

	vid.glstate = HWD.pfnInit() ? VID_GL_LIBRARY_LOADED : VID_GL_LIBRARY_ERROR;

	if (vid.glstate == VID_GL_LIBRARY_ERROR)
	{
		rendermode = render_soft;
		setrenderneeded = 0;
	}
#endif
}

void I_ShutdownGraphics(void)
{
	rendermode = render_none;

	I_OutputMsg("I_ShutdownGraphics(): ");

	if (!graphics_started)
		return;
	graphics_started = false;

	g_rhi.reset();
	g_rhi_generation = 0;

	ShutdownEGL();
}

rhi::Rhi* srb2::sys::get_rhi(rhi::Handle<rhi::Rhi> handle)
{
	(void)handle;
	return g_rhi.get();
}

uint32_t I_GetRefreshRate(void)
{
	return g_refresh_rate;
}

// ---------------------------------------------------------------------------
// Polling and focus
// ---------------------------------------------------------------------------

/// The PS button's menu takes the controllers away from the title; treat it
/// the way the PC build treats the window losing focus, so cv_bgaudio and
/// the game's own pause-on-unfocus behave the same.
static void UpdateFocus(void)
{
	static bool hadfocus = true;
	const bool focus = !PS5_SystemHasFocus();

	if (focus == hadfocus)
		return;
	hadfocus = focus;

	if (focus)
	{
		window_notinfocus = false;
		S_SetMusicVolume();
		g_voice_disabled = cv_voice_selfdeafen.value;
	}
	else
	{
		window_notinfocus = true;
		if (!(cv_bgaudio.value & 1))
			I_SetMusicVolume(0);
		if (!(cv_bgaudio.value & 2))
			S_StopSounds();
		if (!(cv_bgaudio.value & 4))
			g_voice_disabled = true;

		G_ResetAllDeviceGameKeyDown();
		G_ResetAllDeviceResponding();
	}
}

void I_OsPolling(void)
{
	if (!graphics_started)
		return;

	I_GetEvent();

	int caps = 0;
	PS5_PollKeyboard(&shiftdown, &ctrldown, &altdown, &caps);
	capslock = caps ? true : false;

	UpdateFocus();
}

namespace srb2::cvarhandler
{
void on_set_vid_wait();
}

void srb2::cvarhandler::on_set_vid_wait()
{
	PS5_VideoSetSwapInterval(cv_vidwait.value > 0 ? 1 : 0);
}
