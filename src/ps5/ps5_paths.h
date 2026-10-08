// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps5/ps5_paths.h
/// \brief Where the game's data and the player's files live on a PS5

#ifndef SRB2_PS5_PATHS_H
#define SRB2_PS5_PATHS_H

#ifdef __cplusplus
extern "C" {
#endif

/// A text file of extra command line arguments, read from the data
/// directory and then the home directory if either holds one.
#define PS5_ARGS_FILE "ringracers-args.txt"

/// Decide both directories. Safe to call more than once.
void PS5_InitPaths(void);

/// The directory holding bios.pk3 and data/, or NULL if none was found.
const char *PS5_DataDir(void);

/// The writable directory the game is told to use as $HOME: config, game
/// data, profiles, replays, screenshots and downloaded add-ons go under
/// <home>/.ringracers.
const char *PS5_HomeDir(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // SRB2_PS5_PATHS_H
