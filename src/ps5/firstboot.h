// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps5/firstboot.h
/// \brief First boot: fetch the game data the title does not carry
///
/// The title ships without bios.pk3 and data/. The first time it starts and
/// finds them nowhere (ps5_paths.cpp), it downloads the archive Kart Krew
/// attach to their 2.4 release, checks it against the SHA-256 the build
/// pinned (tools/ps5/game-data.sh pins the same), and unpacks it into
/// /data/ringracers, with a screen of its own showing how far it has got.
/// Every later start finds the data there and never sees that screen.
///
/// firstboot_install.c and firstboot_screen.c are plain C with no PS5
/// dependency, so they also build and run on a desktop for testing;
/// firstboot.cpp is the PS5 side.

#ifndef __PS5_FIRSTBOOT_H__
#define __PS5_FIRSTBOOT_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Kart Krew's 2.4 game data, as tools/ps5/game-data.sh pins it.
#define FB_DATA_URL "https://github.com/KartKrewDev/RingRacers/releases/download/v2.4/Dr.Robotnik.s-Ring-Racers-v2.4-Assets.zip"
#define FB_DATA_SHA256 "eebad71b872c20f3323425bc4f4321b9b6256472bc485010d0a73c43fe0af120"
#define FB_DATA_SIZE 752021749ull
/// The archive's name while it is on disk, in the install directory.
#define FB_DATA_ZIP "ringracers-v2.4-assets.zip"

typedef enum
{
	FB_STAGE_DOWNLOAD,
	FB_STAGE_VERIFY,
	FB_STAGE_EXTRACT,
	FB_STAGE_DONE,
	FB_STAGE_FAILED,
} fb_stage_t;

typedef struct
{
	/// Called from the installing thread, often; done and total are bytes
	/// of the current stage, rate is bytes per second while downloading.
	void (*progress)(void* user, fb_stage_t stage, uint64_t done, uint64_t total, uint64_t rate);
	/// One line for the log, without the newline.
	void (*log)(void* user, const char* line);
	void* user;
} fb_callbacks_t;

/// Download, check and unpack the game data into dir, which must exist.
/// ca_bundle is passed to curl, or NULL for its default. On failure,
/// returns 0 with a short message for the player in error, in capitals
/// as the game's own font draws them; the log has the details. Running it
/// again picks up where it stopped: a partial download resumes, and a
/// checked archive is not downloaded again.
int FB_Install(const char* dir, const char* ca_bundle, const fb_callbacks_t* cb,
	char* error, size_t error_size);

// --- The screen --------------------------------------------------------------

#define FB_SCREEN_W 640
#define FB_SCREEN_H 360

typedef struct
{
	fb_stage_t stage;
	uint64_t done;
	uint64_t total;
	uint64_t rate;
	const char* error;
	uint32_t frame;
} fb_view_t;

/// Load the background and the game's console font, which the packager
/// writes to the title folder (tools/ps5/firstboot_art.py). Without them
/// the screen is drawn in plain shapes; returns 0 then.
int FB_LoadArt(const char* path);

/// Draw the screen into rgba, FB_SCREEN_W x FB_SCREEN_H pixels of four
/// bytes (R, G, B, A), top row first.
void FB_Render(uint8_t* rgba, const fb_view_t* view);

#ifdef __cplusplus
}

/// The PS5 side: run the first boot if the game data is missing. Returns
/// once it is installed; on failure it stays on its error screen until the
/// player closes the title.
void PS5_FirstBoot(void);
#endif

#endif
