// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps5/ps5_sys.h
/// \brief The PlayStation 5 system interfaces this port calls.
///
/// The public payload SDK ships import stubs for these modules but no
/// headers, so the declarations live here. Each structure layout is the one
/// another public project has already run on hardware, and says which:
/// a wrong layout on this platform does not fail to link, it reads garbage.

#ifndef SRB2_PS5_SYS_H
#define SRB2_PS5_SYS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Users -------------------------------------------------------------------

#define PS5_USER_ID_INVALID (-1)
#define PS5_USER_ID_SYSTEM  0xFF
#define PS5_MAX_LOGIN_USERS 4

int sceUserServiceInitialize(void *params);
int sceUserServiceGetInitialUser(int32_t *user_id);
int sceUserServiceGetForegroundUser(int32_t *user_id);
int sceUserServiceGetLoginUserIdList(int32_t user_ids[PS5_MAX_LOGIN_USERS]);
int sceUserServiceGetUserName(int32_t user_id, char *name, size_t size);

// --- Controllers -------------------------------------------------------------
//
// The 120-byte state record of scePadReadState. Layout from ps5-payload-dev's
// SDL port (src/joystick/ps5/SDL_ps5joystick.h), cross-checked against
// ps5-opengl's controller-validated ImGui example, which pins the size at 120
// and `connected` at 0x4c.

#define PS5_PAD_PORT_TYPE_STANDARD 0

#define PS5_PAD_BUTTON_L3          0x00000002u
#define PS5_PAD_BUTTON_R3          0x00000004u
#define PS5_PAD_BUTTON_OPTIONS     0x00000008u
#define PS5_PAD_BUTTON_UP          0x00000010u
#define PS5_PAD_BUTTON_RIGHT       0x00000020u
#define PS5_PAD_BUTTON_DOWN        0x00000040u
#define PS5_PAD_BUTTON_LEFT        0x00000080u
#define PS5_PAD_BUTTON_L2          0x00000100u
#define PS5_PAD_BUTTON_R2          0x00000200u
#define PS5_PAD_BUTTON_L1          0x00000400u
#define PS5_PAD_BUTTON_R1          0x00000800u
#define PS5_PAD_BUTTON_TRIANGLE    0x00001000u
#define PS5_PAD_BUTTON_CIRCLE      0x00002000u
#define PS5_PAD_BUTTON_CROSS       0x00004000u
#define PS5_PAD_BUTTON_SQUARE      0x00008000u
#define PS5_PAD_BUTTON_TOUCH_PAD   0x00100000u
/// Set while the system has taken the controller (the PS button's menu, a
/// system dialog). Everything else in the record is stale while it is set.
#define PS5_PAD_BUTTON_INTERCEPTED 0x80000000u

typedef struct ps5_pad_touch_s
{
	uint16_t x;
	uint16_t y;
	uint8_t finger;
	uint8_t pad[3];
} ps5_pad_touch_t;

typedef struct ps5_pad_data_s
{
	uint32_t buttons;
	struct { uint8_t x, y; } left_stick;
	struct { uint8_t x, y; } right_stick;
	struct { uint8_t l2, r2; } analog_buttons;
	uint16_t padding;
	struct { float x, y, z, w; } orientation;
	struct { float x, y, z; } acceleration;
	struct { float x, y, z; } angular_velocity;
	struct
	{
		uint8_t fingers;
		uint8_t pad1[3];
		uint32_t pad2;
		ps5_pad_touch_t touch[2];
	} touch;
	uint8_t connected;
	uint64_t timestamp;
	uint8_t ext[16];
	uint8_t count;
	uint8_t unknown[15];
} ps5_pad_data_t;

#ifdef __cplusplus
static_assert(sizeof(ps5_pad_data_t) == 120, "scePadReadState record size");
static_assert(offsetof(ps5_pad_data_t, connected) == 0x4c, "scePadReadState connected offset");
#else
_Static_assert(sizeof(ps5_pad_data_t) == 120, "scePadReadState record size");
_Static_assert(offsetof(ps5_pad_data_t, connected) == 0x4c, "scePadReadState connected offset");
#endif

typedef struct ps5_pad_color_s
{
	uint8_t r, g, b, a;
} ps5_pad_color_t;

typedef struct ps5_pad_vibration_s
{
	uint8_t large_motor;
	uint8_t small_motor;
} ps5_pad_vibration_t;

int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t type, int32_t index, const void *param);
int scePadClose(int32_t handle);
int scePadReadState(int32_t handle, ps5_pad_data_t *data);
int scePadSetLightBar(int32_t handle, const ps5_pad_color_t *color);
int scePadResetLightBar(int32_t handle);
int scePadSetVibrationMode(int32_t handle, int32_t mode);
int scePadSetVibration(int32_t handle, const ps5_pad_vibration_t *vibration);

// --- Audio -------------------------------------------------------------------
//
// From ps5-payload-dev's SDL port (src/audio/ps5/SDL_ps5audio.h). The main
// port runs at 48 kHz only; sceAudioOutOutput blocks until the previous
// grain has been consumed, which is what paces the mixer thread.

#define PS5_AUDIO_OUT_PORT_TYPE_MAIN 0
#define PS5_AUDIO_OUT_PARAM_FORMAT_S16_STEREO   1
#define PS5_AUDIO_OUT_PARAM_FORMAT_FLOAT_STEREO 4

int32_t sceAudioOutInit(void);
int32_t sceAudioOutOpen(int32_t user_id, int32_t type, int32_t index, uint32_t len, uint32_t freq, uint32_t param);
int32_t sceAudioOutOutput(int32_t handle, const void *ptr);
int32_t sceAudioOutClose(int32_t handle);

// --- System ------------------------------------------------------------------

int sceSystemServiceHideSplashScreen(void);
int sceSystemServiceLoadExec(const char *path, char *const argv[]);

int sceKernelDebugOutText(int channel, const char *text);
int sceKernelUsleep(uint32_t microseconds);

/// The notification request the payload SDK's samples/hello_world sends,
/// which shows a toast on the home screen. The text sits at offset 45.
typedef struct ps5_notify_request_s
{
	char useless1[45];
	char message[3075];
} ps5_notify_request_t;

int sceKernelSendNotificationRequest(int device, ps5_notify_request_t *request, size_t size, int blocking);

// --- Platform layer, implemented in src/ps5/ ----------------------------------

/// Write to the kernel log, which klogsrv and the debugger can read.
void PS5_DebugPrint(const char *text);

/// Show a message on the home screen. For fatal errors: a title has no
/// window to put a message box in.
void PS5_Notify(const char *text);

/// The user the title was started by, or PS5_USER_ID_SYSTEM.
int32_t PS5_InitialUser(void);

/// Open the signed-in users' controllers; I_GetEvent polls them.
void PS5_StartupInput(void);
void PS5_ShutdownInput(void);

/// A USB keyboard, if the system lets the title load its library. The poll
/// posts key events and reports which modifiers are held, the way SDL's
/// keyboard state does on PC.
void PS5_StartupKeyboard(void);
void PS5_ShutdownKeyboard(void);
void PS5_PollKeyboard(uint8_t *shift, uint8_t *ctrl, uint8_t *alt, int *caps);

/// Is the system overlay (the PS button's menu) up? The game treats it as a
/// lost window focus.
int PS5_SystemHasFocus(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // SRB2_PS5_SYS_H
