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
/// \file  ps5/i_main.cpp
/// \brief Main program - PlayStation 5
///
/// The same shape as sdl/i_main.cpp: start the system layer, run the game,
/// turn an escaping C++ exception into I_Error. What differs is where the
/// command line comes from. A title is launched from the home screen with no
/// arguments, so the ones every PS5 build needs are supplied here, and a
/// player who wants more (-server, -connect, -warp, -skipintro...) writes
/// them into a text file the game reads at startup.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <typeinfo>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "../doomdef.h"
#include "../m_argv.h"
#include "../d_main.h"
#include "../i_system.h"
#include "../core/string.h"

#include "firstboot.h"
#include "ps5_sys.h"
#include "ps5_paths.h"

FILE* logstream = NULL;

/// The engine's log, where the PC build keeps it: in the game's directory
/// under $HOME. One file, replaced each run, so a player looking for it over
/// FTP finds the run that just went wrong.
static void InitLogging(void)
{
	if (M_CheckParm("-nolog"))
		return;

	const std::string dir = std::string(PS5_HomeDir()) + "/" DEFAULTDIR;
	mkdir(dir.c_str(), 0755);

	const std::string path = dir + "/latest-log.txt";
	logstream = fopen(path.c_str(), "w");
	if (logstream)
		I_OutputMsg("Logfile: %s\n", path.c_str());
}

static void walk_exception_stack(srb2::String& accum, bool nested)
{
	if (nested)
		accum.append("\n  Caused by: Unknown exception");
	else
		accum.append("Uncaught exception: Unknown exception");
}

static void walk_exception_stack(srb2::String& accum, const std::exception& ex, bool nested)
{
	if (nested)
		accum.append("\n  Caused by: ");
	else
		accum.append("Uncaught exception: ");

	accum.append("(");
	accum.append(typeid(ex).name());
	accum.append(") ");
	accum.append(ex.what());

	try
	{
		std::rethrow_if_nested(ex);
	}
	catch (const std::exception& ex)
	{
		walk_exception_stack(accum, ex, true);
	}
	catch (...)
	{
		walk_exception_stack(accum, true);
	}
}

/// Split one line of the arguments file the way a shell would, near enough:
/// whitespace separates, double quotes group, # starts a comment.
static void ParseArgsLine(const char* line, std::vector<std::string>& out)
{
	std::string current;
	bool quoted = false;
	bool have = false;

	for (const char* p = line; *p; p++)
	{
		const char c = *p;

		if (!quoted && c == '#')
			break;
		if (c == '"')
		{
			quoted = !quoted;
			have = true;
			continue;
		}
		if (!quoted && (c == ' ' || c == '\t' || c == '\r' || c == '\n'))
		{
			if (have)
				out.push_back(current);
			current.clear();
			have = false;
			continue;
		}
		current.push_back(c);
		have = true;
	}

	if (have)
		out.push_back(current);
}

static void ReadArgsFile(const char* path, std::vector<std::string>& out)
{
	FILE* f = fopen(path, "r");
	if (!f)
		return;

	char line[1024];
	while (fgets(line, sizeof line, f))
		ParseArgsLine(line, out);

	fclose(f);
	I_OutputMsg("Read command line arguments from %s\n", path);
}

static bool HasArg(const std::vector<std::string>& args, const char* name)
{
	for (const std::string& a : args)
	{
		if (strcasecmp(a.c_str(), name) == 0)
			return true;
	}
	return false;
}

int main(int argc, char** argv)
{
	PS5_InitPaths();

	// Arguments: whatever the loader passed, then the player's file, then
	// the ones this platform always needs unless the player set them.
	static std::vector<std::string> args;
	args.push_back(argc > 0 && argv[0] ? argv[0] : "ringracers");
	for (int i = 1; i < argc; i++)
	{
		if (argv[i])
			args.push_back(argv[i]);
	}

	// Also the title folder, which an FTP client reaches whichever loader
	// started the title, as /data/homebrew/<title ID>.
	std::vector<std::string> read;
	for (const char* dir : {PS5_DataDir(), PS5_HomeDir(), "/app0"})
	{
		if (!dir || !*dir || std::find(read.begin(), read.end(), dir) != read.end())
			continue;
		read.push_back(dir);
		ReadArgsFile((std::string(dir) + "/" PS5_ARGS_FILE).c_str(), args);
	}

	if (!HasArg(args, "-home"))
	{
		args.push_back("-home");
		args.push_back(PS5_HomeDir());
	}

	static std::vector<char*> argp;
	for (std::string& a : args)
		argp.push_back(a.data());
	argp.push_back(nullptr);

	myargc = static_cast<int>(argp.size() - 1);
	myargv = argp.data();

	InitLogging();
	I_StartupSystem();

	// The launch screen stays up until the title says otherwise. On the
	// first boot, with no game data anywhere, the screen that downloads it
	// takes over from the launch screen once it has a frame to show; on
	// every other boot the launch screen goes now, as it always has.
	if (!PS5_DataDir())
		PS5_FirstBoot();
	sceSystemServiceHideSplashScreen();

	try
	{
		CONS_Printf("Setting up Dr. Robotnik's Ring Racers...\n");
		D_SRB2Main();
		CONS_Printf("Entering main game loop...\n");
		// never returns
		D_SRB2Loop();
	}
	catch (const std::exception& ex)
	{
		srb2::String exception;
		walk_exception_stack(exception, ex, false);
		I_Error("%s", exception.c_str());
	}
	catch (...)
	{
		srb2::String exception;
		walk_exception_stack(exception, false);
		I_Error("%s", exception.c_str());
	}

	return 0;
}
