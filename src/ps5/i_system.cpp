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
/// \file  ps5/i_system.cpp
/// \brief System interface - PlayStation 5
///
/// sdl/i_system.cpp with SDL taken out. Where the PC build asks SDL, this
/// asks FreeBSD's libc (which is what the console's libc is) or a system
/// module; where the PC build talks to a terminal or a window manager, this
/// writes to a log file, because a console has neither.

#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../doomdef.h"
#include "../m_misc.h"
#include "../i_time.h"
#include "../i_video.h"
#include "../i_sound.h"
#include "../i_system.h"
#include "../i_threads.h"
#include "../i_joy.h"
#include "../d_net.h"
#include "../d_main.h"
#include "../g_game.h"
#include "../g_demo.h"
#include "../m_argv.h"
#include "../w_wad.h"
#include "../s_sound.h"
#include "../r_main.h"
#include "../r_fps.h"
#include "../k_kart.h"
#include "../k_menu.h"
#include "../core/thread_pool.h"
#include "../d_clisrv.h"
#include "../d_netfil.h"

#include "ps5_sys.h"
#include "ps5_paths.h"

static std::thread::id g_main_thread_id;

uint8_t keyboard_started = false;
dboolean g_in_exiting_signal_handler = false;

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

void PS5_DebugPrint(const char* text)
{
	sceKernelDebugOutText(0, text);
}

void PS5_Notify(const char* text)
{
	ps5_notify_request_t request;
	memset(&request, 0, sizeof request);
	snprintf(request.message, sizeof request.message, "%s", text);
	sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
}

void I_OutputMsg(const char* fmt, ...)
{
	char txt[8192];
	va_list argptr;

	va_start(argptr, fmt);
	vsnprintf(txt, sizeof(txt), fmt, argptr);
	va_end(argptr);

	// stdout and stderr both go to ringracers-stdout.txt (where, see
	// native/runtime_shims.c), unbuffered, so this survives a crash. The
	// kernel log is for a developer watching with klogsrv.
	fputs(txt, stderr);
	sceKernelDebugOutText(0, txt);

	if (logstream)
	{
		fputs(txt, logstream);
		fflush(logstream);
	}
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

precise_t I_GetPreciseTime(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<precise_t>(ts.tv_sec) * 1000000000ULL + static_cast<precise_t>(ts.tv_nsec);
}

uint64_t I_GetPrecisePrecision(void)
{
	return 1000000000ULL;
}

static uint32_t frame_rate;
static double frame_frequency;
static uint64_t frame_epoch;
static double elapsed_frames;

static void I_InitFrameTime(const uint64_t now, const uint32_t cap)
{
	frame_rate = cap;
	frame_epoch = now;

	if (frame_rate == 0)
	{
		frame_frequency = 1.0;
		return;
	}

	frame_frequency = I_GetPrecisePrecision() / (double)frame_rate;
}

double I_GetFrameTime(void)
{
	const uint64_t now = I_GetPreciseTime();
	const uint32_t cap = R_GetFramerateCap();

	if (cap != frame_rate)
		I_InitFrameTime(now, cap);

	if (frame_rate == 0)
		elapsed_frames += 1.0;
	else
		elapsed_frames += (now - frame_epoch) / frame_frequency;

	frame_epoch = now;
	return elapsed_frames;
}

void I_StartupTimer(void)
{
	I_InitFrameTime(I_GetPreciseTime(), R_GetFramerateCap());
	elapsed_frames = 0.0;
}

void I_Sleep(uint32_t ms)
{
	usleep(static_cast<useconds_t>(ms) * 1000);
}

void I_WaitVBL(int32_t count)
{
	(void)count;
	usleep(1000);
}

void I_BeginRead(void) {}
void I_EndRead(void) {}

// ---------------------------------------------------------------------------
// Memory and storage
// ---------------------------------------------------------------------------

extern "C" const size_t ps5_opengl_heap_size;
extern "C" size_t ps5_opengl_heap_live_bytes(void);

uint64_t I_GetFreeMem(uint64_t* total)
{
	// malloc is a heap reserved once at startup (native/app_heap.c); report
	// that as the machine, since it is what the game has to work with. The
	// size is the one asked for: app_heap.c may have been granted less.
	const size_t live = ps5_opengl_heap_live_bytes();

	if (total)
		*total = ps5_opengl_heap_size;
	return live < ps5_opengl_heap_size ? ps5_opengl_heap_size - live : 0;
}

void I_GetDiskFreeSpace(int64_t* freespace)
{
	// statfs exists only in the system-private libkernel; a title cannot ask.
	*freespace = INT32_MAX;
}

int32_t I_mkdir(const char* dirname, int32_t unixright)
{
	return mkdir(dirname, unixright);
}

// A title cannot ask for its working directory: getcwd() (libSceLibcInternal)
// crashes it, and chdir("/app0") fails. So the working directory is the last
// one I_ChDir reached. While that is not the data directory, IdentifyVersion,
// the one caller, opens the archives by their absolute paths.
static char g_cwd[256] = "/";

int32_t I_ChDir(const char* path)
{
	const int32_t ret = chdir(path);
	if (ret == 0)
		snprintf(g_cwd, sizeof g_cwd, "%s", path);
	return ret;
}

char* I_GetCwd(char* buf, size_t size)
{
	if (!buf || !size)
		return NULL;
	snprintf(buf, size, "%s", g_cwd);
	return buf;
}

char* I_GetEnv(const char* name)
{
	return getenv(name);
}

int32_t I_PutEnv(char* variable)
{
	return putenv(variable);
}

const char* I_LocateWad(void)
{
	PS5_InitPaths();

	const char* dir = PS5_DataDir();

	I_OutputMsg("Looking for bios.pk3 in /data/ringracers, /app0, /mnt/usb0/ringracers, /mnt/usb1/ringracers, /mnt/ext0/ringracers\n");

	if (!dir)
	{
		PS5_Notify("Ring Racers: bios.pk3 not found.\nCopy bios.pk3 and data/ to /data/ringracers or into the app folder.");
		return NULL;
	}

	if (I_ChDir(dir) == -1)
		I_OutputMsg("Couldn't change working directory to %s\n", dir);

	return dir;
}

char* I_GetUserName(void)
{
	static char username[MAXPLAYERNAME + 1];
	char name[32] = {};

	const int32_t user = PS5_InitialUser();
	if (user != PS5_USER_ID_SYSTEM && sceUserServiceGetUserName(user, name, sizeof name) == 0 && name[0])
	{
		snprintf(username, sizeof username, "%s", name);
		return username;
	}

	return NULL;
}

// ---------------------------------------------------------------------------
// Desktop conveniences a console does not have
// ---------------------------------------------------------------------------

int32_t I_ClipboardCopy(const char* data, size_t size)
{
	(void)data;
	(void)size;
	return -1;
}

const char* I_ClipboardPaste(void)
{
	return NULL;
}

dboolean I_HasOpenURL(void)
{
	return false;
}

void I_OpenURL(const char* data)
{
	(void)data;
}

void I_CursedWindowMovement(int xd, int yd)
{
	(void)xd;
	(void)yd;
}

void I_UpdateMouseGrab(void) {}
void I_StartupMouse(void) {}

#ifndef NOMUMBLE
void I_UpdateMumble(const mobj_t* mobj, const listener_t listener)
{
	(void)mobj;
	(void)listener;
}
#endif

void I_RegisterSysCommands(void) {}

// ---------------------------------------------------------------------------
// Crashes
// ---------------------------------------------------------------------------

static const char* SignalName(int num)
{
	switch (num)
	{
		case SIGSEGV: return "segmentation violation";
		case SIGFPE:  return "floating point exception";
		case SIGILL:  return "illegal instruction";
		case SIGABRT: return "abort";
		case SIGBUS:  return "bus error";
		default:      return "unknown signal";
	}
}

static void CrashHandler(int num)
{
	char line[128];

	g_in_exiting_signal_handler = true;

	snprintf(line, sizeof line, "\nProcess killed by signal: %s (%d)\n", SignalName(num), num);
	fputs(line, stderr);
	sceKernelDebugOutText(0, line);

	PS5_Notify("Ring Racers crashed.\nSee ringracers-stdout.txt (in /data/ringracers if it exists).");

	signal(num, SIG_DFL);
	_exit(-1);
}

static void I_RegisterSignals(void)
{
	signal(SIGSEGV, CrashHandler);
	signal(SIGFPE, CrashHandler);
	signal(SIGILL, CrashHandler);
	signal(SIGABRT, CrashHandler);
	signal(SIGBUS, CrashHandler);
}

// ---------------------------------------------------------------------------
// Startup and shutdown
// ---------------------------------------------------------------------------

#define MAX_QUIT_FUNCS 16
static quitfuncptr quit_funcs[MAX_QUIT_FUNCS];

void I_AddExitFunc(void (*func)())
{
	for (int32_t c = 0; c < MAX_QUIT_FUNCS; c++)
	{
		if (!quit_funcs[c])
		{
			quit_funcs[c] = func;
			break;
		}
	}
}

void I_RemoveExitFunc(void (*func)())
{
	for (int32_t c = 0; c < MAX_QUIT_FUNCS; c++)
	{
		if (quit_funcs[c] == func)
		{
			while (c < MAX_QUIT_FUNCS - 1)
			{
				quit_funcs[c] = quit_funcs[c + 1];
				c++;
			}
			quit_funcs[MAX_QUIT_FUNCS - 1] = NULL;
			break;
		}
	}
}

int32_t I_StartupSystem(void)
{
	g_main_thread_id = std::this_thread::get_id();

	sceUserServiceInitialize(NULL);

#ifdef HAVE_THREADS
	I_start_threads();
	I_AddExitFunc(I_stop_threads);
	I_ThreadPoolInit();
	I_AddExitFunc(I_ThreadPoolShutdown);
#endif
	I_RegisterSignals();

	I_OutputMsg("Dr. Robotnik's Ring Racers %s for PlayStation 5\n", VERSIONSTRING);
	I_OutputMsg("Data directory: %s\n", PS5_DataDir() ? PS5_DataDir() : "(none found)");
	I_OutputMsg("Home directory: %s\n", PS5_HomeDir());

	return 0;
}

void I_ShutdownSystem(void)
{
	for (int32_t c = MAX_QUIT_FUNCS - 1; c >= 0; c--)
	{
		if (quit_funcs[c])
			(*quit_funcs[c])();
	}

	if (logstream)
	{
		I_OutputMsg("I_ShutdownSystem(): end of logstream.\n");
		fclose(logstream);
		logstream = NULL;
	}
}

FUNCNORETURN void ATTRNORETURN I_Quit(void)
{
	static bool quitting = false;

	if (quitting)
		goto death;
	quitting = true;

	M_SaveConfig(NULL);
	M_SaveJoinedIPs();

	if (Playing())
		K_PlayerForfeit(consoleplayer, true);

	G_SaveGameData();

	if (demo.recording)
		G_CheckDemoStatus();

	D_QuitNetGame();
	CL_AbortDownloadResume();
	I_ShutdownMusic();
	I_ShutdownSound();
	I_ShutdownGraphics();
	PS5_ShutdownInput();
	I_ShutdownSystem();

death:
	W_Shutdown();
	fflush(NULL);
	// Return to the home screen; see native/runtime_shims.c.
	sceSystemServiceLoadExec("exit", NULL);
	exit(0);
}

static int32_t errorcount = 0;
static dboolean shutdowning = false;

extern "C" consvar_t cv_fuzz;

FUNCIERROR void ATTRNORETURN I_Error(const char* error, ...)
{
	va_list argptr;
	char buffer[8192];

	va_start(argptr, error);
	vsnprintf(buffer, sizeof(buffer), error, argptr);
	va_end(argptr);

	if (std::this_thread::get_id() != g_main_thread_id)
	{
		// Errors off the main thread cannot be shut down from gracefully.
		I_OutputMsg("\nI_Error() off the main thread: %s\n", buffer);
		PS5_Notify(buffer);
		_exit(-2);
	}

	if (shutdowning)
	{
		errorcount++;
		if (errorcount == 2)
			I_ShutdownMusic();
		if (errorcount == 3)
			I_ShutdownSound();
		if (errorcount == 4)
			I_ShutdownGraphics();
		if (errorcount == 5)
			PS5_ShutdownInput();
		if (errorcount == 6)
			I_ShutdownSystem();
		if (errorcount == 8)
			G_DirtyGameData();
		if (errorcount > 20)
		{
			I_OutputMsg("\nRecursive I_Error(): %s\n", buffer);
			PS5_Notify(buffer);
			W_Shutdown();
			_exit(-1);
		}
	}
	else
	{
		S_StopSounds();
		S_StartSound(NULL, sfx_etexpl);
	}

	shutdowning = true;

	I_OutputMsg("\nI_Error(): %s\n", buffer);

	G_DirtyGameData();

	if (demo.recording)
		G_CheckDemoStatus();

	D_QuitNetGame();
	CL_AbortDownloadResume();

	I_ShutdownMusic();
	I_ShutdownGraphics();
	PS5_ShutdownInput();

	// No window to put a message box in: the home screen's notifications
	// are the one place a player will see it.
	if (!cv_fuzz.value)
	{
		char note[512];
		snprintf(note, sizeof note, "Ring Racers error:\n%s", buffer);
		PS5_Notify(note);
	}

	I_ShutdownSound();
	I_ShutdownSystem();
	W_Shutdown();

	fflush(NULL);
	sceSystemServiceLoadExec("exit", NULL);
	exit(-1);
}
