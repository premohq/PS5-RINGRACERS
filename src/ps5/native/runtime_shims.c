// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps5/native/runtime_shims.c
/// \brief What the title's C runtime lacks, and the static OpenGL stack expects
///
/// Derived from ps5-opengl's native-app/runtime_shims.c (GPL-3.0-or-later,
/// BlackBearReloaded). That file also stubs __assert, mkstemps, openlog,
/// popen and pclose for Mesa; this title links the payload SDK's libc.a,
/// which has real ones. What remains is the C++ TLS init hook Mesa's glapi
/// never needs, and the two things that differ: there stdout is a test
/// receipt and a returning main holds for a harness; here stdout is a log
/// beside the save data and a returning main is the player quitting.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../ps5_sys.h"

/// Everything written to stdout or stderr - the engine's own console output
/// included, see I_OutputMsg - lands in one file, unbuffered, so that it
/// survives a crash. In /data/ringracers when the title can write there,
/// because a player can fetch it over FTP; otherwise in /download0, the
/// title's own volume, which always works but sits inside a disk image.
static const char *const ps5_stdout_logs[] = {
	"/data/ringracers/ringracers-stdout.txt",
	"/download0/ringracers-stdout.txt",
};

__attribute__((constructor)) static void ps5_open_log(void)
{
	for (size_t i = 0; i < sizeof ps5_stdout_logs / sizeof ps5_stdout_logs[0]; i++)
	{
		FILE *stream = freopen(ps5_stdout_logs[i], "w", stdout);
		if (stream == NULL)
			continue;
		setvbuf(stream, NULL, _IONBF, 0);
		stream = freopen(ps5_stdout_logs[i], "a", stderr);
		if (stream != NULL)
			setvbuf(stream, NULL, _IONBF, 0);
		return;
	}
}

/// Called by app_crt.cpp when main returns, before exit(). LoadExec("exit")
/// is how PS4 homebrew hands a quitting title back to the shell; whether the
/// PS5 shell needs it, or is content with exit(), has not been tried on
/// hardware. If the call returns, app_crt.cpp's exit() still runs.
void catchReturnFromMain(int status)
{
	fflush(NULL);
	(void)status;
	sceSystemServiceLoadExec("exit", NULL);
}

void ps5_opengl_glapi_tls_context_init(void) __asm__("_ZTH23_mesa_glapi_tls_Context");
void ps5_opengl_glapi_tls_context_init(void) {}

/// OpenSSL's dynamic-engine loader (libcrypto's dso_dlfcn) asks dladdr which
/// module an address belongs to. No system module exports it, and the
/// payload SDK's libc.a version reaches for loader internals a title does
/// not have, which the native converter rejects. A title loads no OpenSSL
/// engines, so "no such address" is the true answer.
struct dl_info_s;
int dladdr(const void *address, struct dl_info_s *info)
{
	(void)address;
	(void)info;
	return 0;
}

/// zstd, inside curl, declares its tracing hooks weak and calls them when
/// something defines them. The native converter refuses to leave a weak
/// import unresolved, so define them: a zero context means "not tracing".
struct ZSTD_DCtx_s;
unsigned long long ZSTD_trace_decompress_begin(const struct ZSTD_DCtx_s *dctx)
{
	(void)dctx;
	return 0;
}

void ZSTD_trace_decompress_end(unsigned long long ctx, const void *trace)
{
	(void)ctx;
	(void)trace;
}

/// The heap app_heap.c reserves for malloc. ps5-opengl's default, 128 MiB, is
/// sized for its conformance tests. The engine's zone allocator sits on
/// malloc and the PC build has no budget at all, so this is a ceiling chosen
/// to be generous, not a measurement: 2 GiB of direct memory, which
/// app_heap.c halves until the system grants it.
const size_t ps5_opengl_heap_size = (size_t)2 << 30;
