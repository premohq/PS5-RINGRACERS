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
/// beside the save data and a returning main is the player quitting. Added
/// here: a desktop-sized default stack for the engine's threads.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>

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
	// The first boot installs the game data here, and a player who bundled
	// it into the title (--bundle-data) has no reason to have made this
	// folder; without it the log and the saves land in /download0, out of
	// FTP's reach. Where /data is not writable
	// this fails and changes nothing; ps5_paths.cpp probes it again.
	mkdir("/data/ringracers", 0777);

	for (size_t i = 0; i < sizeof ps5_stdout_logs / sizeof ps5_stdout_logs[0]; i++)
	{
		// Truncate the last run's log, then append from both streams. Each
		// stream has its own file offset, so with stdout opened "w" its
		// writes (Mesa's, ps5-opengl's) went over the engine's from the
		// start of the file; with O_APPEND on both, each lands at the end.
		if (freopen(ps5_stdout_logs[i], "w", stdout) == NULL)
			continue;
		if (freopen(ps5_stdout_logs[i], "a", stdout) == NULL)
			continue;
		setvbuf(stdout, NULL, _IONBF, 0);
		if (freopen(ps5_stdout_logs[i], "a", stderr) != NULL)
			setvbuf(stderr, NULL, _IONBF, 0);
		return;
	}
}

/// Every thread created without attributes - std::thread's, the job pool's
/// that run the software renderer, the master server's and the downloader's
/// that do TLS handshakes - gets this much stack. Upstream has only ever run
/// on desktops, whose smallest default for a secondary thread is macOS's
/// 512 KiB; what the console's libkernel gives by default is not documented
/// for titles. 2 MiB is FreeBSD's own default on amd64. If the system will
/// not create a thread this size, it is created as asked, so this can only
/// add stack, never cost a thread. Wired in by --wrap=pthread_create in
/// src/ps5/CMakeLists.txt.
#define PS5_THREAD_STACK_SIZE ((size_t)2 << 20)

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
	void *(*start)(void *), void *arg);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
	void *(*start)(void *), void *arg)
{
	if (attr == NULL)
	{
		pthread_attr_t sized;
		if (pthread_attr_init(&sized) == 0)
		{
			int err = pthread_attr_setstacksize(&sized, PS5_THREAD_STACK_SIZE);
			if (err == 0)
				err = __real_pthread_create(thread, &sized, start, arg);
			pthread_attr_destroy(&sized);
			if (err == 0)
				return 0;
		}
	}
	return __real_pthread_create(thread, attr, start, arg);
}

/// What a title's libkernel refuses on firmware 13.60, and what stands in for
/// it. Each refusal is logged once, for whoever meets the next one.
enum
{
	PS5_SHIM_SETFD,
	PS5_SHIM_SETFL,
	PS5_SHIM_FIONBIO,
	PS5_SHIM_NBIO,
};

static void ps5_shim_log(int what, int err)
{
	static const char *const names[] = {
		"fcntl F_SETFD", "fcntl F_SETFL", "ioctl FIONBIO", "setsockopt SO_NBIO",
	};
	static int logged;
	char line[112];

	if (logged & (1 << what))
		return;
	logged |= 1 << what;
	snprintf(line, sizeof line, "[ps5] %s failed: %s (%d)\n", names[what], strerror(err), err);
	PS5_DebugPrint(line);
}

/// A title's socket only turns non-blocking through Sony's SO_NBIO option:
/// fcntl()'s F_SETFL and ioctl()'s FIONBIO are both refused, and F_GETFL
/// says O_NONBLOCK of a socket that blocks. curl reads F_GETFL first, saw
/// nothing to do, and went on as if its sockets could not block, so its
/// graceful close sat for minutes in a read on a connection the server kept
/// open. So for a socket, F_GETFL's O_NONBLOCK and F_SETFL's are SO_NBIO's.
#define PS5_SO_NBIO 0x1200

/// Which sockets SO_NBIO has made non-blocking, rather than reading SO_NBIO
/// back, which nothing shows the console answers truthfully. Every socket
/// starts out blocking, so the bit is cleared whenever a socket is made (see
/// __wrap_socket and friends) and set only by ps5_set_nbio. curl's own
/// download sockets ask for SOCK_NONBLOCK in socket() and never reach here;
/// firstboot_install.c gives those a receive timeout.
#define PS5_NBIO_FDS 4096
static uint8_t ps5_nbio_fds[PS5_NBIO_FDS / 8];

/// Which descriptors are sockets. getsockopt(SO_TYPE) said curl's were not,
/// so its F_GETFL went to the console's after all; the wrappers below that
/// make sockets know for certain.
static uint8_t ps5_socket_fds[PS5_NBIO_FDS / 8];

static void ps5_note_bit(uint8_t *bits, int fd, int on)
{
	if (fd < 0 || fd >= PS5_NBIO_FDS)
		return;
	if (on)
		__atomic_fetch_or(&bits[fd >> 3], (uint8_t)(1 << (fd & 7)), __ATOMIC_RELAXED);
	else
		__atomic_fetch_and(&bits[fd >> 3], (uint8_t)~(1 << (fd & 7)), __ATOMIC_RELAXED);
}

static int ps5_bit(const uint8_t *bits, int fd)
{
	if (fd < 0 || fd >= PS5_NBIO_FDS)
		return 0;
	return (__atomic_load_n(&bits[fd >> 3], __ATOMIC_RELAXED) >> (fd & 7)) & 1;
}

static void ps5_note_nbio(int fd, int on)
{
	ps5_note_bit(ps5_nbio_fds, fd, on);
}

/// A new socket: known to be one, and blocking, whatever the last
/// descriptor with its number was.
static void ps5_note_socket(int fd, int on)
{
	ps5_note_bit(ps5_socket_fds, fd, on);
	ps5_note_bit(ps5_nbio_fds, fd, 0);
}

static int ps5_is_socket(int fd)
{
	int type;
	socklen_t len = sizeof type;
	if (ps5_bit(ps5_socket_fds, fd))
		return 1;
	return getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0;
}

static int ps5_set_nbio(int fd, int on)
{
	if (setsockopt(fd, SOL_SOCKET, PS5_SO_NBIO, &on, sizeof on) == 0)
	{
		ps5_note_nbio(fd, on);
		return 0;
	}
	ps5_shim_log(PS5_SHIM_NBIO, errno);
	return -1;
}

/// Whether ps5_set_nbio made this socket non-blocking. A socket out of the
/// table's range reads as blocking, so a caller that wants it otherwise
/// asks to change it, which is harmless if it already is.
static int ps5_get_nbio(int fd)
{
	return ps5_bit(ps5_nbio_fds, fd);
}

/// Sockets as they are made and closed. Wired in by --wrap=socket
/// --wrap=accept --wrap=socketpair --wrap=close in src/ps5/CMakeLists.txt.
int __real_socket(int domain, int type, int protocol);
int __real_accept(int fd, struct sockaddr *addr, socklen_t *len);
int __real_socketpair(int domain, int type, int protocol, int fds[2]);
int __real_close(int fd);

int __wrap_socket(int domain, int type, int protocol)
{
	int fd = __real_socket(domain, type, protocol);
	ps5_note_socket(fd, 1);
	return fd;
}

int __wrap_accept(int fd, struct sockaddr *addr, socklen_t *len)
{
	int ret = __real_accept(fd, addr, len);
	ps5_note_socket(ret, 1);
	return ret;
}

int __wrap_socketpair(int domain, int type, int protocol, int fds[2])
{
	int ret = __real_socketpair(domain, type, protocol, fds);
	if (ret == 0)
	{
		ps5_note_socket(fds[0], 1);
		ps5_note_socket(fds[1], 1);
	}
	return ret;
}

/// The number may next be a file, which F_SETFL must reach as a file.
int __wrap_close(int fd)
{
	ps5_note_socket(fd, 0);
	return __real_close(fd);
}

/// curl marks each socket it opens close-on-exec, and gives up on one it
/// cannot ("fcntl set CLOEXEC"). A title's fcntl() refuses F_SETFD with
/// EINVAL. A title never calls exec, so close-on-exec can be granted without
/// being set. Wired in by --wrap=fcntl in src/ps5/CMakeLists.txt.
int __real_fcntl(int fd, int cmd, ...);

int __wrap_fcntl(int fd, int cmd, ...)
{
	va_list ap;
	intptr_t arg;
	int ret;

	// Every command curl and the engine use takes an int or a pointer, or
	// nothing, in which case this reads a register no one looks at.
	va_start(ap, cmd);
	arg = va_arg(ap, intptr_t);
	va_end(ap);

	if ((cmd == F_GETFL || cmd == F_SETFL) && ps5_is_socket(fd))
	{
		if (cmd == F_SETFL)
			return ps5_set_nbio(fd, (arg & O_NONBLOCK) != 0);
		ret = __real_fcntl(fd, F_GETFL, 0);
		if (ret == -1)
			ret = O_RDWR;
		return ps5_get_nbio(fd) ? (ret | O_NONBLOCK) : (ret & ~O_NONBLOCK);
	}

	ret = __real_fcntl(fd, cmd, arg);
	if (ret != -1)
		return ret;

	switch (cmd)
	{
		case F_GETFD:
			return 0;
		case F_SETFD:
			ps5_shim_log(PS5_SHIM_SETFD, errno);
			return 0;
		case F_GETFL:
			return O_RDWR;
		case F_SETFL:
			ps5_shim_log(PS5_SHIM_SETFL, errno);
			return -1;
		default:
			return ret;
	}
}

/// FIONBIO on a socket goes to SO_NBIO too, for code that asks that way.
/// Wired in by --wrap=ioctl in src/ps5/CMakeLists.txt.
int __real_ioctl(int fd, unsigned long request, ...);

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;
	int ret;

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);

	if (request == FIONBIO && arg && ps5_is_socket(fd))
		return ps5_set_nbio(fd, *(const int *)arg != 0);
	ret = __real_ioctl(fd, request, arg);
	if (ret == -1 && request == FIONBIO)
		ps5_shim_log(PS5_SHIM_FIONBIO, errno);
	return ret;
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
