// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps5/i_keyboard.cpp
/// \brief USB keyboard - PlayStation 5
///
/// A keyboard plugged into the console works as it does on PC: chat, the
/// console, and keyboard controls. The game's own on-screen keyboard
/// (menus/transient/virtual-keyboard.c) already covers typing names and
/// addresses with a controller; this is for everything else.
///
/// libSceKeyboard reports the keys held as USB HID usage codes, which are the
/// numbers SDL uses for its scancodes, so the translation below is
/// sdl/i_video.cpp's Impl_SDL_Scancode_To_Keycode with the SDL names written
/// out as numbers. The state record's layout is the one ps5-payload-dev's SDL
/// port reads (src/video/ps5/SDL_ps5keyboard.c), read here into a larger
/// buffer so that a longer record on some firmware cannot overrun it.
///
/// The library is loaded when the game starts rather than imported by the
/// title. A title that imports a module the system will not give it does not
/// start at all; this way, a console that refuses the keyboard library only
/// loses the keyboard.

#include <cstdint>
#include <cstring>

#include "../doomdef.h"
#include "../d_event.h"
#include "../d_main.h"
#include "../i_system.h"
#include "../i_time.h"
#include "../keys.h"

#include "ps5_sys.h"

extern "C"
{
int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, uint32_t flags, const void* opt, int* result);
int sceKernelDlsym(int handle, const char* symbol, void** address);
}

namespace
{

struct KeyboardState
{
	uint64_t junk0[2];
	uint8_t available;
	uint32_t junk1[2];
	uint32_t modifiers;
	uint16_t scankey[16];
	uint64_t junk3[4];
};

union KeyboardBuffer
{
	KeyboardState state;
	uint8_t padding[512];
};

using KeyboardInit = int (*)(void);
using KeyboardOpen = int (*)(int32_t user, int32_t type, int32_t index, void* param);
using KeyboardReadState = int (*)(int32_t handle, void* state);
using KeyboardClose = int (*)(int32_t handle);

KeyboardInit pInit;
KeyboardOpen pOpen;
KeyboardReadState pReadState;
KeyboardClose pClose;

bool g_loaded = false;
bool g_load_failed = false;
int32_t g_handle = -1;
precise_t g_last_open_attempt = 0;
KeyboardState g_previous {};

// Key repeat, which a held key gets from the operating system on PC.
int32_t g_repeat_key = 0;
precise_t g_repeat_at = 0;

// HID modifier bits, in the order the boot protocol defines them.
constexpr uint32_t kLeftCtrl = 0x01, kLeftShift = 0x02, kLeftAlt = 0x04, kLeftGui = 0x08;
constexpr uint32_t kRightCtrl = 0x10, kRightShift = 0x20, kRightAlt = 0x40, kRightGui = 0x80;

constexpr uint16_t kHidCapsLock = 57;

/// sdl/i_video.cpp's Impl_SDL_Scancode_To_Keycode, by HID usage number.
int32_t HidToKey(uint16_t code)
{
	if (code >= 4 && code <= 29)   // A-Z
		return code - 4 + 'a';
	if (code >= 30 && code <= 38)  // 1-9
		return code - 30 + '1';
	if (code == 39)                // 0
		return '0';
	if (code >= 58 && code <= 67)  // F1-F10
		return KEY_F1 + (code - 58);

	switch (code)
	{
		case 68: return KEY_F11;
		case 69: return KEY_F12;

		case 98: return KEY_KEYPAD0;
		case 89: return KEY_KEYPAD1;
		case 90: return KEY_KEYPAD2;
		case 91: return KEY_KEYPAD3;
		case 92: return KEY_KEYPAD4;
		case 93: return KEY_KEYPAD5;
		case 94: return KEY_KEYPAD6;
		case 95: return KEY_KEYPAD7;
		case 96: return KEY_KEYPAD8;
		case 97: return KEY_KEYPAD9;

		case 40: return KEY_ENTER;      // RETURN
		case 41: return KEY_ESCAPE;
		case 42: return KEY_BACKSPACE;
		case 43: return KEY_TAB;
		case 44: return KEY_SPACE;
		case 45: return KEY_MINUS;
		case 46: return KEY_EQUALS;
		case 47: return '[';
		case 48: return ']';
		case 49: return '\\';
		case 50: return '#';            // NONUSHASH
		case 51: return ';';
		case 52: return '\'';
		case 53: return '`';            // GRAVE
		case 54: return ',';
		case 55: return '.';
		case 56: return '/';
		case 57: return KEY_CAPSLOCK;
		case 70: return 0;              // PRINTSCREEN
		case 71: return KEY_SCROLLLOCK;
		case 72: return KEY_PAUSE;
		case 73: return KEY_INS;
		case 74: return KEY_HOME;
		case 75: return KEY_PGUP;
		case 76: return KEY_DEL;
		case 77: return KEY_END;
		case 78: return KEY_PGDN;
		case 79: return KEY_RIGHTARROW;
		case 80: return KEY_LEFTARROW;
		case 81: return KEY_DOWNARROW;
		case 82: return KEY_UPARROW;
		case 83: return KEY_NUMLOCK;
		case 84: return KEY_KPADSLASH;
		case 85: return '*';            // KP_MULTIPLY
		case 86: return KEY_MINUSPAD;
		case 87: return KEY_PLUSPAD;
		case 88: return KEY_ENTER;      // KP_ENTER
		case 99: return KEY_KPADDEL;    // KP_PERIOD
		case 100: return '\\';          // NONUSBACKSLASH
		default: break;
	}
	return 0;
}

void PostKey(int32_t key, bool down, bool repeat)
{
	if (!key)
		return;

	event_t ev {};
	ev.type = down ? ev_keydown : ev_keyup;
	ev.data1 = key;
	ev.data2 = repeat;
	ev.device = 0; // the keyboard and mouse device, as on PC
	D_PostEvent(&ev);
}

/// The modifier keys are reported as a bitmask; the game wants them as keys.
void PostModifiers(uint32_t before, uint32_t after)
{
	static const struct { uint32_t bit; int32_t key; } kModifiers[] = {
		{kLeftCtrl, KEY_LCTRL}, {kLeftShift, KEY_LSHIFT}, {kLeftAlt, KEY_LALT}, {kLeftGui, KEY_LEFTWIN},
		{kRightCtrl, KEY_RCTRL}, {kRightShift, KEY_RSHIFT}, {kRightAlt, KEY_RALT}, {kRightGui, KEY_RIGHTWIN},
	};

	const uint32_t changed = before ^ after;
	for (const auto& m : kModifiers)
	{
		if (changed & m.bit)
			PostKey(m.key, (after & m.bit) != 0, false);
	}
}

void ReleaseAll(void)
{
	PostModifiers(g_previous.modifiers, 0);
	for (uint16_t code : g_previous.scankey)
	{
		if (code)
			PostKey(HidToKey(code), false, false);
	}
	g_previous = KeyboardState {};
	g_repeat_key = 0;
}

bool LoadLibrary(void)
{
	if (g_loaded)
		return true;
	if (g_load_failed)
		return false;

	static const char* const kPaths[] = {
		"/system/common/lib/libSceKeyboard.sprx",
		"/system_ex/common_ex/lib/libSceKeyboard.sprx",
	};

	int module = -1;
	for (const char* path : kPaths)
	{
		module = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
		if (module >= 0)
			break;
	}

	if (module < 0
		|| sceKernelDlsym(module, "sceKeyboardInit", reinterpret_cast<void**>(&pInit)) != 0
		|| sceKernelDlsym(module, "sceKeyboardOpen", reinterpret_cast<void**>(&pOpen)) != 0
		|| sceKernelDlsym(module, "sceKeyboardReadState", reinterpret_cast<void**>(&pReadState)) != 0
		|| sceKernelDlsym(module, "sceKeyboardClose", reinterpret_cast<void**>(&pClose)) != 0)
	{
		I_OutputMsg("USB keyboard support unavailable (libSceKeyboard: 0x%08x)\n", static_cast<unsigned>(module));
		g_load_failed = true;
		return false;
	}

	const int err = pInit();
	if (err < 0)
	{
		I_OutputMsg("sceKeyboardInit failed: 0x%08x\n", static_cast<unsigned>(err));
		g_load_failed = true;
		return false;
	}

	g_loaded = true;
	return true;
}

void TryOpen(void)
{
	g_last_open_attempt = I_GetPreciseTime();

	long junk = 0;
	const int32_t handle = pOpen(PS5_InitialUser(), 0, 0, &junk);
	if (handle > 0)
	{
		g_handle = handle;
		I_OutputMsg("USB keyboard ready\n");
	}
}

} // namespace

void PS5_StartupKeyboard(void)
{
	if (LoadLibrary())
		TryOpen();
}

void PS5_ShutdownKeyboard(void)
{
	if (g_handle > 0 && pClose)
		pClose(g_handle);
	g_handle = -1;
}

void PS5_PollKeyboard(uint8_t* shift, uint8_t* ctrl, uint8_t* alt, int* caps)
{
	static bool caps_on = false;

	*shift = *ctrl = *alt = 0;
	*caps = caps_on;

	if (!g_loaded)
		return;

	const precise_t now = I_GetPreciseTime();

	if (g_handle <= 0)
	{
		// The handle belongs to a user; try again now and then in case the
		// first attempt came too early.
		if (now - g_last_open_attempt > I_GetPrecisePrecision() * 5)
			TryOpen();
		return;
	}

	KeyboardBuffer buffer;
	memset(&buffer, 0, sizeof buffer);
	if (pReadState(g_handle, &buffer) != 0)
		return;

	const KeyboardState& current = buffer.state;

	if (!current.available)
	{
		ReleaseAll();
		return;
	}

	PostModifiers(g_previous.modifiers, current.modifiers);

	// A key is down if it is in the list now; it went down if it was not in
	// the list before. Order in the list is not meaningful.
	for (uint16_t code : g_previous.scankey)
	{
		if (!code)
			continue;
		bool still = false;
		for (uint16_t now_code : current.scankey)
			still = still || now_code == code;
		if (!still)
		{
			const int32_t key = HidToKey(code);
			PostKey(key, false, false);
			if (key == g_repeat_key)
				g_repeat_key = 0;
		}
	}

	for (uint16_t code : current.scankey)
	{
		if (!code)
			continue;
		bool was = false;
		for (uint16_t before : g_previous.scankey)
			was = was || before == code;
		if (!was)
		{
			const int32_t key = HidToKey(code);
			PostKey(key, true, false);
			if (code == kHidCapsLock)
				caps_on = !caps_on;
			g_repeat_key = key;
			g_repeat_at = now + I_GetPrecisePrecision() / 2;
		}
	}

	if (g_repeat_key && now >= g_repeat_at)
	{
		PostKey(g_repeat_key, true, true);
		g_repeat_at = now + I_GetPrecisePrecision() / 30;
	}

	g_previous = current;

	const uint32_t m = current.modifiers;
	*shift = ((m & kLeftShift) ? 1 : 0) | ((m & kRightShift) ? 2 : 0);
	*ctrl = ((m & kLeftCtrl) ? 1 : 0) | ((m & kRightCtrl) ? 2 : 0);
	*alt = ((m & kLeftAlt) ? 1 : 0) | ((m & kRightAlt) ? 2 : 0);
	*caps = caps_on;
}
