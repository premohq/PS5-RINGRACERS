// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps5/i_net.c
/// \brief Network interface - PlayStation 5
///
/// Netplay is the PC build's: d_net.cpp asks this for a platform network
/// driver, gets none, and falls back to I_InitTcpNetwork - the engine's BSD
/// socket driver in i_tcp.c, which is exactly what a desktop build without
/// SDL_net runs. The console's libkernel provides the sockets and
/// ps5_netdb.c the name resolution.

#include "../doomdef.h"
#include "../i_net.h"
#include "../m_argv.h"

dboolean I_InitNetwork(void)
{
	if (M_CheckParm("-net"))
	{
		I_Error("-net not supported, use -server and -connect\n"
			"see docs for more\n");
	}

	return false;
}
