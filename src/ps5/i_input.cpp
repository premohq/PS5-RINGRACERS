// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps5/i_input.cpp
/// \brief Controller input - PlayStation 5
///
/// The engine numbers gamepad buttons and axes the way SDL's game controller
/// API does (g_input.h: JOYBUTTONS is 21 "to match SDL_GameControllerButton"),
/// and the default bindings are written in those numbers. So each DualSense
/// control is reported as the index SDL gives the same control on a PC: Cross
/// is A, Circle B, Square X, Triangle Y, Options Start, the touch pad click
/// TOUCHPAD, L2 and R2 the trigger axes. With that, the shipped defaults and
/// every profile a player made on PC mean the same thing here.
///
/// A PS5 controller belongs to a signed-in user, and each user's controller
/// is reported as its own device. Splitscreen for up to four is the PC
/// build's: sign in a user per controller and press a button on each.

#include <cstdio>
#include <cstring>

#include "../doomdef.h"
#include "../doomstat.h"
#include "../d_event.h"
#include "../d_main.h"
#include "../g_input.h"
#include "../i_joy.h"
#include "../i_system.h"
#include "../i_time.h"

#include "ps5_sys.h"

extern "C" int scePadGetHandle(int32_t user_id, int32_t type, int32_t index);

namespace
{

/// scePadOpen's answer when this process already holds the pad.
constexpr int kPadAlreadyOpened = static_cast<int>(0x80920004u);

struct ButtonMap
{
	uint32_t mask;
	int32_t index; // SDL_GamepadButton
};

const ButtonMap kButtons[] = {
	{PS5_PAD_BUTTON_CROSS,     0},  // A        -- Accelerate / confirm
	{PS5_PAD_BUTTON_CIRCLE,    1},  // B        -- Look back / cancel
	{PS5_PAD_BUTTON_SQUARE,    2},  // X        -- Brake
	{PS5_PAD_BUTTON_TRIANGLE,  3},  // Y        -- Spindash
	{PS5_PAD_BUTTON_OPTIONS,   6},  // START    -- Pause
	{PS5_PAD_BUTTON_L3,        7},  // LEFTSTICK
	{PS5_PAD_BUTTON_R3,        8},  // RIGHTSTICK
	{PS5_PAD_BUTTON_L1,        9},  // LB       -- Ring bail
	{PS5_PAD_BUTTON_R1,       10},  // RB       -- Action
	{PS5_PAD_BUTTON_UP,       11},  // DPAD_UP
	{PS5_PAD_BUTTON_DOWN,     12},  // DPAD_DOWN
	{PS5_PAD_BUTTON_LEFT,     13},  // DPAD_LEFT
	{PS5_PAD_BUTTON_RIGHT,    14},  // DPAD_RIGHT
	{PS5_PAD_BUTTON_TOUCH_PAD, 20}, // TOUCHPAD
	// The Create button and the PS button belong to the system and are never
	// reported to a title, so SDL's BACK (4) and GUIDE (5) have no source.
};

struct Pad
{
	int32_t user = PS5_USER_ID_INVALID;
	int32_t handle = -1;
	bool announced = false;   // the engine has been sent device_added
	bool intercepted = false; // the system has the controller
	uint32_t buttons = 0;
	int32_t axis[JOYAXISSETS][2] = {};
	bool axis_known = false;
	char name[64] = {};
};

Pad g_pads[PS5_MAX_LOGIN_USERS];
int32_t g_initial_user = PS5_USER_ID_SYSTEM;
precise_t g_last_user_scan = 0;
bool g_started = false;

int32_t DeviceFor(int slot)
{
	return slot + 1;
}

bool SlotFor(int32_t device_id, int* slot)
{
	const int s = device_id - 1;
	if (s < 0 || s >= PS5_MAX_LOGIN_USERS || g_pads[s].handle < 0)
		return false;
	*slot = s;
	return true;
}

void PostDeviceEvent(int slot, evtype_t type)
{
	event_t ev {};
	ev.type = type;
	ev.device = DeviceFor(slot);
	D_PostEvent(&ev);
}

void PostButton(int slot, int32_t index, bool down)
{
	event_t ev {};
	ev.type = down ? ev_keydown : ev_keyup;
	ev.data1 = KEY_JOY1 + index;
	ev.data2 = 0;
	ev.device = DeviceFor(slot);
	D_PostEvent(&ev);
}

/// One axis pair; INT32_MAX is the engine's "this half did not change".
void PostAxis(int slot, int32_t set, int32_t x, int32_t y)
{
	Pad& pad = g_pads[slot];
	int32_t* last = pad.axis[set];

	if (pad.axis_known && last[0] == x && last[1] == y)
		return;

	event_t ev {};
	ev.type = ev_gamepad_axis;
	ev.data1 = set;
	ev.data2 = x;
	ev.data3 = y;
	ev.device = DeviceFor(slot);
	D_PostEvent(&ev);

	last[0] = x;
	last[1] = y;
}

/// A stick byte (0..255, 128 at rest; down and right are high, as in SDL) in
/// the engine's -JOYAXISRANGE..JOYAXISRANGE. The dead zone is the player's
/// own setting, applied by the engine, as on PC.
int32_t StickAxis(uint8_t raw)
{
	const int32_t centred = static_cast<int32_t>(raw) - 128;
	int32_t value = centred >= 0 ? (centred * JOYAXISRANGE) / 127 : (centred * JOYAXISRANGE) / 128;
	if (value > JOYAXISRANGE)
		value = JOYAXISRANGE;
	if (value < -JOYAXISRANGE)
		value = -JOYAXISRANGE;
	return value;
}

int32_t TriggerAxis(uint8_t raw)
{
	return (static_cast<int32_t>(raw) * JOYAXISRANGE) / 255;
}

/// Let go of everything a pad was holding, so nothing sticks down while the
/// system has it or after it has gone.
void ReleaseAll(int slot)
{
	Pad& pad = g_pads[slot];

	for (const ButtonMap& b : kButtons)
	{
		if (pad.buttons & b.mask)
			PostButton(slot, b.index, false);
	}
	pad.buttons = 0;

	for (int32_t set = 0; set < JOYAXISSETS; set++)
		PostAxis(slot, set, 0, 0);
}

void ClosePad(int slot)
{
	Pad& pad = g_pads[slot];

	if (pad.announced)
	{
		ReleaseAll(slot);
		PostDeviceEvent(slot, ev_gamepad_device_removed);
	}

	if (pad.handle >= 0)
		scePadClose(pad.handle);

	pad = Pad {};
}

void OpenPad(int slot, int32_t user)
{
	Pad& pad = g_pads[slot];

	int32_t handle = scePadOpen(user, PS5_PAD_PORT_TYPE_STANDARD, 0, nullptr);
	if (handle == kPadAlreadyOpened)
		handle = scePadGetHandle(user, PS5_PAD_PORT_TYPE_STANDARD, 0);
	if (handle < 0)
	{
		I_OutputMsg("scePadOpen for user 0x%08x failed: 0x%08x\n",
			static_cast<unsigned>(user), static_cast<unsigned>(handle));
		return;
	}

	pad = Pad {};
	pad.user = user;
	pad.handle = handle;

	// The mode ps5-payload-dev's SDL port sets before rumbling.
	scePadSetVibrationMode(handle, 2);

	char username[32] = {};
	if (sceUserServiceGetUserName(user, username, sizeof username) == 0 && username[0])
		snprintf(pad.name, sizeof pad.name, "DualSense (%s)", username);
	else
		snprintf(pad.name, sizeof pad.name, "DualSense %d", slot + 1);

	I_OutputMsg("Controller %d: %s\n", slot + 1, pad.name);
}

/// Who is signed in, and therefore whose controller to read. Users sign in
/// and out from the PS button's menu while the game runs; this is how a
/// second player joins.
void ScanUsers(void)
{
	int32_t users[PS5_MAX_LOGIN_USERS];

	for (int32_t& u : users)
		u = PS5_USER_ID_INVALID;

	if (sceUserServiceGetLoginUserIdList(users) != 0)
		return;

	// Close pads whose user left; keep a user on the slot they had.
	for (int slot = 0; slot < PS5_MAX_LOGIN_USERS; slot++)
	{
		Pad& pad = g_pads[slot];
		if (pad.user == PS5_USER_ID_INVALID)
			continue;

		bool present = false;
		for (int32_t u : users)
			present = present || (u == pad.user);
		if (!present)
			ClosePad(slot);
	}

	for (int32_t u : users)
	{
		if (u == PS5_USER_ID_INVALID)
			continue;

		bool have = false;
		for (const Pad& pad : g_pads)
			have = have || (pad.user == u);
		if (have)
			continue;

		for (int slot = 0; slot < PS5_MAX_LOGIN_USERS; slot++)
		{
			if (g_pads[slot].user == PS5_USER_ID_INVALID)
			{
				OpenPad(slot, u);
				break;
			}
		}
	}
}

void ReadPad(int slot)
{
	Pad& pad = g_pads[slot];
	ps5_pad_data_t data;

	memset(&data, 0, sizeof data);
	if (scePadReadState(pad.handle, &data) < 0)
		return;

	if (!data.connected)
	{
		// The user is still signed in but the controller is off or
		// unplugged. Tell the engine, as SDL would on a disconnect.
		if (pad.announced)
		{
			ReleaseAll(slot);
			PostDeviceEvent(slot, ev_gamepad_device_removed);
			pad.announced = false;
			pad.axis_known = false;
		}
		return;
	}

	if (!pad.announced)
	{
		PostDeviceEvent(slot, ev_gamepad_device_added);
		pad.announced = true;
	}

	pad.intercepted = (data.buttons & PS5_PAD_BUTTON_INTERCEPTED) != 0;
	if (pad.intercepted)
	{
		// The record is stale while the system has the controller.
		if (pad.buttons || pad.axis_known)
			ReleaseAll(slot);
		return;
	}

	const uint32_t changed = pad.buttons ^ data.buttons;
	if (changed)
	{
		for (const ButtonMap& b : kButtons)
		{
			if (changed & b.mask)
				PostButton(slot, b.index, (data.buttons & b.mask) != 0);
		}
	}
	pad.buttons = data.buttons;

	PostAxis(slot, 0, StickAxis(data.left_stick.x), StickAxis(data.left_stick.y));
	PostAxis(slot, 1, StickAxis(data.right_stick.x), StickAxis(data.right_stick.y));
	PostAxis(slot, 2, TriggerAxis(data.analog_buttons.l2), TriggerAxis(data.analog_buttons.r2));
	pad.axis_known = true;
}

} // namespace

int32_t PS5_InitialUser(void)
{
	// Asked for by I_GetUserName during startup, before the engine starts
	// input; sceUserServiceInitialize has run by then (I_StartupSystem).
	static bool known = false;
	if (!known)
	{
		if (sceUserServiceGetInitialUser(&g_initial_user) != 0)
			g_initial_user = PS5_USER_ID_SYSTEM;
		known = true;
	}
	return g_initial_user;
}

void PS5_StartupInput(void)
{
	if (g_started)
		return;

	PS5_InitialUser();

	const int err = scePadInit();
	if (err < 0)
	{
		I_OutputMsg("scePadInit failed: 0x%08x\n", static_cast<unsigned>(err));
		return;
	}

	g_started = true;
	ScanUsers();
	g_last_user_scan = I_GetPreciseTime();
}

void PS5_ShutdownInput(void)
{
	if (!g_started)
		return;

	for (int slot = 0; slot < PS5_MAX_LOGIN_USERS; slot++)
	{
		if (g_pads[slot].handle >= 0)
		{
			scePadResetLightBar(g_pads[slot].handle);
			scePadClose(g_pads[slot].handle);
		}
		g_pads[slot] = Pad {};
	}

	PS5_ShutdownKeyboard();
	g_started = false;
}

int PS5_SystemHasFocus(void)
{
	for (const Pad& pad : g_pads)
	{
		if (pad.handle >= 0 && pad.intercepted)
			return 1;
	}
	return 0;
}

void I_GetEvent(void)
{
	if (!g_started)
		return;

	// Signing in is rare and the user list is an IPC round trip; four
	// times a second is plenty.
	const precise_t now = I_GetPreciseTime();
	if (now - g_last_user_scan > I_GetPrecisePrecision() / 4)
	{
		ScanUsers();
		g_last_user_scan = now;
	}

	for (int slot = 0; slot < PS5_MAX_LOGIN_USERS; slot++)
	{
		if (g_pads[slot].handle >= 0)
			ReadPad(slot);
	}
}

// ---------------------------------------------------------------------------
// The engine's controller interface
// ---------------------------------------------------------------------------

void I_StartupInput(void)
{
	PS5_StartupInput();
	PS5_StartupKeyboard();
}

void I_InitJoystick1(void) {}
void I_InitJoystick2(void) {}
void I_InitJoystick3(void) {}
void I_InitJoystick4(void) {}

void I_JoyScale(void) {}
void I_JoyScale2(void) {}
void I_JoyScale3(void) {}
void I_JoyScale4(void) {}

void I_GetJoystickEvents(uint8_t index)
{
	(void)index;
}

void I_Tactile(FFType Type, const JoyFF_t* Effect)  { (void)Type; (void)Effect; }
void I_Tactile2(FFType Type, const JoyFF_t* Effect) { (void)Type; (void)Effect; }
void I_Tactile3(FFType Type, const JoyFF_t* Effect) { (void)Type; (void)Effect; }
void I_Tactile4(FFType Type, const JoyFF_t* Effect) { (void)Type; (void)Effect; }

int32_t I_NumJoys(void)
{
	int32_t n = 0;
	for (const Pad& pad : g_pads)
		n += pad.handle >= 0 && pad.announced;
	return n;
}

const char* I_GetJoyName(int32_t joyindex)
{
	int32_t n = 0;
	for (const Pad& pad : g_pads)
	{
		if (pad.handle >= 0 && pad.announced)
		{
			if (n == joyindex)
				return pad.name;
			n++;
		}
	}
	return NULL;
}

void I_SetGamepadPlayerIndex(int32_t device_id, int32_t index)
{
	// The DualSense's player lights are the system's to set.
	(void)device_id;
	(void)index;
}

void I_SetGamepadIndicatorColor(int32_t device_id, uint8_t red, uint8_t green, uint8_t blue)
{
	int slot;
	if (!SlotFor(device_id, &slot))
		return;

	const ps5_pad_color_t color = {red, green, blue, 0};
	scePadSetLightBar(g_pads[slot].handle, &color);
}

void I_GetGamepadGuid(int32_t device_id, char* out, int out_len)
{
	if (out_len <= 0)
		return;

	int slot;
	if (!SlotFor(device_id, &slot))
	{
		out[0] = '\0';
		return;
	}

	// The engine only prints this when a pad connects, so it names the
	// user the pad belongs to rather than imitating SDL's format.
	snprintf(out, out_len, "ps5-user-%08x", static_cast<unsigned>(g_pads[slot].user));
}

void I_GetGamepadName(int32_t device_id, char* out, int out_len)
{
	if (out_len <= 0)
		return;

	int slot;
	if (!SlotFor(device_id, &slot))
	{
		out[0] = '\0';
		return;
	}

	snprintf(out, out_len, "%s", g_pads[slot].name);
}

void I_GamepadRumble(int32_t device_id, uint16_t low_strength, uint16_t high_strength)
{
	int slot;
	if (!SlotFor(device_id, &slot))
		return;

	const ps5_pad_vibration_t vibration = {
		static_cast<uint8_t>(low_strength >> 8),
		static_cast<uint8_t>(high_strength >> 8),
	};
	scePadSetVibration(g_pads[slot].handle, &vibration);
}

void I_GamepadRumbleTriggers(int32_t device_id, uint16_t left_strength, uint16_t right_strength)
{
	// The adaptive triggers have their own effect API, which is not public.
	(void)device_id;
	(void)left_strength;
	(void)right_strength;
}
