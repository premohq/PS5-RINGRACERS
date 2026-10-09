// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps5/ps5_paths.cpp
/// \brief Where the game's data and the player's files live on a PS5
///
/// A native title sees its own folder read-only at /app0 and a private
/// writable volume at /download0 (sized by downloadDataSize in param.json).
/// Those two always work. Whether a title can also reach /data, where a
/// player's FTP client puts files, depends on the loader that mounted it and
/// has not been established here. So the order is: probe the places that are
/// convenient for the player, and fall back to the ones that are guaranteed.

#include "ps5_paths.h"

#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ps5_sys.h"

namespace
{

char g_data_dir[256];
char g_home_dir[256];
bool g_initialised = false;

/// Places bios.pk3 and data/ may be, in the order they are tried.
///
/// /data/ringracers comes first so that a player can keep one copy of the
/// game's archives on the console's storage and update the title without
/// re-uploading 700 MB. /app0 is where tools/ps5/package.sh puts them when
/// asked to bundle them. The USB paths are where payload-era homebrew finds
/// external drives. /download0 is where the first boot installs them when the
/// title cannot write to /data.
const char* const kDataCandidates[] = {
	"/data/ringracers",
	"/app0",
	"/mnt/usb0/ringracers",
	"/mnt/usb1/ringracers",
	"/mnt/ext0/ringracers",
	"/download0",
};

/// Writable homes, in order. /download0 is the title's own volume and the
/// one that is always there; /data/ringracers is preferred when the title
/// can write to it, because there the player can reach their replays,
/// screenshots and add-ons over FTP.
const char* const kHomeCandidates[] = {
	"/data/ringracers",
	"/download0",
};

bool FileExists(const char* path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool DirectoryWritable(const char* dir)
{
	struct stat st;
	if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
		return false;

	char probe[300];
	snprintf(probe, sizeof probe, "%s/.ringracers-write-test", dir);
	const int fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return false;
	close(fd);
	unlink(probe);
	return true;
}

} // namespace

void PS5_InitPaths(void)
{
	if (g_initialised)
		return;
	g_initialised = true;

	for (const char* dir : kDataCandidates)
	{
		char bios[300];
		snprintf(bios, sizeof bios, "%s/bios.pk3", dir);
		if (FileExists(bios))
		{
			snprintf(g_data_dir, sizeof g_data_dir, "%s", dir);
			break;
		}
	}

	// The probe needs the folder to be there; where the title may not write,
	// this fails and the probe moves on.
	mkdir(kHomeCandidates[0], 0777);

	for (const char* dir : kHomeCandidates)
	{
		if (DirectoryWritable(dir))
		{
			snprintf(g_home_dir, sizeof g_home_dir, "%s", dir);
			break;
		}
	}
	if (!g_home_dir[0])
		snprintf(g_home_dir, sizeof g_home_dir, "%s", "/download0");

	char line[700];
	snprintf(line, sizeof line, "[ringracers] data: %s, home: %s\n",
		g_data_dir[0] ? g_data_dir : "(not found)", g_home_dir);
	PS5_DebugPrint(line);
}

void PS5_RescanPaths(void)
{
	g_initialised = false;
	g_data_dir[0] = '\0';
	g_home_dir[0] = '\0';
	PS5_InitPaths();
}

const char* PS5_DataDir(void)
{
	return g_data_dir[0] ? g_data_dir : nullptr;
}

const char* PS5_HomeDir(void)
{
	return g_home_dir;
}
