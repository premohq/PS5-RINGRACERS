// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps5/firstboot_install.c
/// \brief First boot: download, check and unpack the game data
///
/// Three steps, each safe to interrupt. The download goes to a .part file
/// and resumes from its end. Only once its SHA-256 matches the pin is it
/// renamed to the archive's own name, so an archive on disk is a good one.
/// Each file is unpacked to a .part of its own and renamed when its CRC
/// matches, data/ first and bios.pk3 last: ps5_paths.cpp takes a folder
/// with bios.pk3 in it as installed, so it never sees half an install.

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <zlib.h>

#include "firstboot.h"

#define FB_PATH_MAX 512
#define FB_CHUNK (256 * 1024)

typedef struct
{
	const fb_callbacks_t* cb;
	char* error;
	size_t error_size;
	/// The archive itself is bad, rather than the network or the disk.
	int damaged;
} fb_ctx_t;

static void fb_log(fb_ctx_t* ctx, const char* fmt, ...)
{
	char line[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);

	if (ctx->cb && ctx->cb->log)
		ctx->cb->log(ctx->cb->user, line);
}

static void fb_progress(fb_ctx_t* ctx, fb_stage_t stage, uint64_t done, uint64_t total, uint64_t rate)
{
	if (ctx->cb && ctx->cb->progress)
		ctx->cb->progress(ctx->cb->user, stage, done, total, rate);
}

/// The player's message: short, and in capitals like the rest of the screen.
/// The last one set stands if the install gives up.
static int fb_fail(fb_ctx_t* ctx, const char* message)
{
	snprintf(ctx->error, ctx->error_size, "%s", message);
	fb_log(ctx, "error: %s", message);
	return 0;
}

static int fb_fail_damaged(fb_ctx_t* ctx)
{
	ctx->damaged = 1;
	return fb_fail(ctx, "THE DOWNLOAD WAS DAMAGED");
}

static int64_t fb_file_size(const char* path)
{
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return -1;
	return (int64_t)st.st_size;
}

static double fb_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/// A write failed: name the likeliest cause the player can do something
/// about.
static int fb_fail_write(fb_ctx_t* ctx, const char* path, int err)
{
	fb_log(ctx, "writing %s: %s", path, strerror(err));
	if (err == ENOSPC)
		return fb_fail(ctx, "THE CONSOLE IS OUT OF SPACE");
	return fb_fail(ctx, "COULD NOT WRITE THE GAME FILES");
}

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)
// ---------------------------------------------------------------------------

typedef struct
{
	uint32_t h[8];
	uint64_t length;
	uint8_t block[64];
	size_t used;
} fb_sha256_t;

static const uint32_t fb_sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define FB_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void fb_sha256_block(fb_sha256_t* s, const uint8_t* p)
{
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
	for (; i < 64; i++)
	{
		const uint32_t s0 = FB_ROR(w[i - 15], 7) ^ FB_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		const uint32_t s1 = FB_ROR(w[i - 2], 17) ^ FB_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
	e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
	for (i = 0; i < 64; i++)
	{
		const uint32_t t1 = h + (FB_ROR(e, 6) ^ FB_ROR(e, 11) ^ FB_ROR(e, 25)) + ((e & f) ^ (~e & g)) + fb_sha256_k[i] + w[i];
		const uint32_t t2 = (FB_ROR(a, 2) ^ FB_ROR(a, 13) ^ FB_ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
	s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void fb_sha256_init(fb_sha256_t* s)
{
	static const uint32_t h0[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};
	memcpy(s->h, h0, sizeof h0);
	s->length = 0;
	s->used = 0;
}

static void fb_sha256_update(fb_sha256_t* s, const uint8_t* p, size_t n)
{
	s->length += n;
	if (s->used)
	{
		const size_t take = n < 64 - s->used ? n : 64 - s->used;
		memcpy(s->block + s->used, p, take);
		s->used += take;
		p += take;
		n -= take;
		if (s->used < 64)
			return;
		fb_sha256_block(s, s->block);
		s->used = 0;
	}
	for (; n >= 64; p += 64, n -= 64)
		fb_sha256_block(s, p);
	memcpy(s->block, p, n);
	s->used = n;
}

static void fb_sha256_hex(fb_sha256_t* s, char out[65])
{
	const uint64_t bits = s->length * 8;
	static const uint8_t pad = 0x80;
	static const uint8_t zero[64];
	uint8_t length[8];
	int i;

	fb_sha256_update(s, &pad, 1);
	fb_sha256_update(s, zero, (s->used <= 56 ? 56 : 120) - s->used);
	for (i = 0; i < 8; i++)
		length[i] = (uint8_t)(bits >> (56 - 8 * i));
	fb_sha256_update(s, length, 8);

	for (i = 0; i < 8; i++)
		snprintf(out + i * 8, 9, "%08x", s->h[i]);
}

/// SHA-256 of a file against the pin, reporting progress as it reads.
static int fb_file_matches(fb_ctx_t* ctx, const char* path)
{
	FILE* f = fopen(path, "rb");
	uint8_t* buffer;
	fb_sha256_t sha;
	char hex[65];
	uint64_t done = 0;
	const int64_t total = fb_file_size(path);
	size_t n;

	if (!f)
		return 0;
	buffer = (uint8_t*)malloc(FB_CHUNK);
	if (!buffer)
	{
		fclose(f);
		return 0;
	}

	fb_sha256_init(&sha);
	while ((n = fread(buffer, 1, FB_CHUNK, f)) > 0)
	{
		fb_sha256_update(&sha, buffer, n);
		done += n;
		fb_progress(ctx, FB_STAGE_VERIFY, done, (uint64_t)total, 0);
	}
	free(buffer);
	fclose(f);

	fb_sha256_hex(&sha, hex);
	fb_log(ctx, "SHA-256 of %s: %s", path, hex);
	return strcmp(hex, FB_DATA_SHA256) == 0;
}

// ---------------------------------------------------------------------------
// Download
// ---------------------------------------------------------------------------

typedef struct
{
	fb_ctx_t* ctx;
	FILE* file;
	int write_errno;
	uint64_t offset;
	double window_start;
	uint64_t window_bytes;
	uint64_t rate;
} fb_download_t;

static size_t fb_write(char* data, size_t size, size_t count, void* user)
{
	fb_download_t* d = (fb_download_t*)user;
	const size_t n = size * count;

	if (fwrite(data, 1, n, d->file) != n)
	{
		d->write_errno = errno ? errno : EIO;
		return 0;
	}
	return n;
}

static int fb_xferinfo(void* user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	fb_download_t* d = (fb_download_t*)user;
	const double now = fb_now();
	const uint64_t done = d->offset + (uint64_t)dlnow;
	const uint64_t total = dltotal > 0 ? d->offset + (uint64_t)dltotal : FB_DATA_SIZE;

	(void)ultotal;
	(void)ulnow;

	// A rate over about a second, steady enough to read.
	if (now - d->window_start >= 1.0)
	{
		d->rate = (uint64_t)((double)(done - d->window_bytes) / (now - d->window_start));
		d->window_start = now;
		d->window_bytes = done;
	}

	fb_progress(d->ctx, FB_STAGE_DOWNLOAD, done, total, d->rate);
	return 0;
}

/// Whatever runtime_shims.c's SO_NBIO stand-in manages, no read or write on
/// the download's sockets blocks for more than a few seconds: curl's close
/// once waited minutes in a read for a TLS goodbye the server never sent.
/// curl treats a timed-out read as "nothing yet" and goes on to its own
/// timeouts, which a blocking socket otherwise keeps it from reaching.
static int fb_sockopt(void* user, curl_socket_t fd, curlsocktype purpose)
{
	struct timeval limit = { 5, 0 };

	(void)user;
	(void)purpose;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof limit);
	return CURL_SOCKOPT_OK;
}

typedef enum
{
	FB_GET_OK,
	FB_GET_RETRY,      // worth trying again: the network
	FB_GET_NO_RANGE,   // the server will not resume from where the file ends
	FB_GET_FAILED,     // reported already
} fb_get_t;

/// One attempt at downloading the rest of part, from its current end.
static fb_get_t fb_get(fb_ctx_t* ctx, const char* part, const char* ca_bundle)
{
	fb_download_t d;
	CURL* curl;
	CURLcode code;
	char curl_error[CURL_ERROR_SIZE] = "";
	long status = 0;
	const int64_t have = fb_file_size(part);

	memset(&d, 0, sizeof d);
	d.ctx = ctx;
	d.offset = have > 0 ? (uint64_t)have : 0;
	d.window_start = fb_now();
	d.window_bytes = d.offset;

	d.file = fopen(part, d.offset ? "ab" : "wb");
	if (!d.file)
	{
		fb_fail_write(ctx, part, errno);
		return FB_GET_FAILED;
	}

	curl = curl_easy_init();
	if (!curl)
	{
		fclose(d.file);
		fb_fail(ctx, "COULD NOT START THE DOWNLOAD");
		return FB_GET_FAILED;
	}

	curl_easy_setopt(curl, CURLOPT_URL, FB_DATA_URL);
	// HTTP/1.1: one file needs nothing more, and it is what has been run on
	// a console.
	curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "RingRacers-PS5-firstboot");
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fb_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &d);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, fb_xferinfo);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &d);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, fb_sockopt);
	// Give up on a connection that has stalled for a minute; the next
	// attempt resumes.
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
	if (d.offset)
		curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)d.offset);
	if (ca_bundle)
		curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle);

	fb_log(ctx, "downloading %s from byte %llu", FB_DATA_URL, (unsigned long long)d.offset);
	code = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);

	if (fclose(d.file) != 0 && code == CURLE_OK)
	{
		fb_fail_write(ctx, part, errno);
		return FB_GET_FAILED;
	}

	if (code == CURLE_OK)
		return FB_GET_OK;

	fb_log(ctx, "download stopped: %s (curl %d, HTTP %ld): %s",
		curl_easy_strerror(code), (int)code, status, curl_error);

	if (code == CURLE_WRITE_ERROR && d.write_errno)
	{
		fb_fail_write(ctx, part, d.write_errno);
		return FB_GET_FAILED;
	}
	if (code == CURLE_RANGE_ERROR || status == 416)
		return FB_GET_NO_RANGE;
	if (code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_CONNECT)
	{
		fb_fail(ctx, "NO INTERNET CONNECTION");
		return FB_GET_RETRY;
	}
	if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CONNECT_ERROR)
	{
		fb_fail(ctx, "COULD NOT REACH THE DOWNLOAD SAFELY");
		return FB_GET_RETRY;
	}
	if (code == CURLE_HTTP_RETURNED_ERROR)
	{
		fb_fail(ctx, "THE DOWNLOAD IS NOT AVAILABLE");
		return FB_GET_RETRY;
	}
	fb_fail(ctx, "THE DOWNLOAD STOPPED");
	return FB_GET_RETRY;
}

/// The archive, downloaded and checked, at zip.
static int fb_download(fb_ctx_t* ctx, const char* zip, const char* part, const char* ca_bundle)
{
	int attempt;
	int restarted = 0;

	for (attempt = 1; attempt <= 4; attempt++)
	{
		const fb_get_t got = fb_get(ctx, part, ca_bundle);

		if (got == FB_GET_FAILED)
			return 0;

		if (got == FB_GET_RETRY)
		{
			if (attempt < 4)
			{
				fb_log(ctx, "trying again in %d seconds", 2 << attempt);
				sleep(2 << attempt);
			}
			continue;
		}

		// FB_GET_NO_RANGE: either the file is already whole, or the server
		// will not resume it. Check it; if it is no good, start again.
		if (got == FB_GET_OK || fb_file_size(part) > 0)
		{
			if (fb_file_matches(ctx, part))
			{
				if (rename(part, zip) != 0)
					return fb_fail_write(ctx, zip, errno);
				return 1;
			}
		}

		fb_log(ctx, "%s does not match the pinned SHA-256", part);
		remove(part);
		if (restarted)
			return fb_fail(ctx, "THE DOWNLOAD WAS DAMAGED");
		restarted = 1;
		attempt = 0;
	}

	// The last attempt's message stands.
	return 0;
}

// ---------------------------------------------------------------------------
// Unpack
// ---------------------------------------------------------------------------

static uint16_t fb_u16(const uint8_t* p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t fb_u32(const uint8_t* p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

typedef struct
{
	char name[128];
	uint16_t method;
	uint32_t crc;
	uint32_t packed;
	uint32_t size;
	uint32_t local;
} fb_entry_t;

/// Only what the game reads: bios.pk3 and the files directly in data/.
/// Anything else in the archive, or any name that could climb out of the
/// install directory, is left alone.
static int fb_wanted(const char* name)
{
	if (strcmp(name, "bios.pk3") == 0)
		return 1;
	if (strncmp(name, "data/", 5) != 0 || name[5] == '\0')
		return 0;
	return strchr(name + 5, '/') == NULL && strchr(name + 5, '\\') == NULL && strstr(name, "..") == NULL;
}

/// The archive's directory: the entries to unpack, data/ before bios.pk3.
static int fb_read_directory(fb_ctx_t* ctx, FILE* f, fb_entry_t** out, size_t* count)
{
	uint8_t tail[65536 + 22];
	uint8_t* cd = NULL;
	uint8_t* p;
	fb_entry_t* entries = NULL;
	fb_entry_t bios;
	int have_bios = 0;
	size_t n = 0, i, entries_total;
	long size, tail_size, eocd = -1, k;
	uint32_t cd_size, cd_offset;

	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 22)
		return 0;
	tail_size = size < (long)sizeof tail ? size : (long)sizeof tail;
	if (fseek(f, size - tail_size, SEEK_SET) != 0 || fread(tail, 1, (size_t)tail_size, f) != (size_t)tail_size)
		return 0;
	for (k = tail_size - 22; k >= 0; k--)
	{
		if (fb_u32(tail + k) == 0x06054b50)
		{
			eocd = k;
			break;
		}
	}
	if (eocd < 0)
		return 0;

	entries_total = fb_u16(tail + eocd + 10);
	cd_size = fb_u32(tail + eocd + 12);
	cd_offset = fb_u32(tail + eocd + 16);
	if (cd_offset == 0xFFFFFFFF || (long)cd_offset + (long)cd_size > size)
		return 0;

	cd = (uint8_t*)malloc(cd_size);
	entries = (fb_entry_t*)calloc(entries_total + 1, sizeof *entries);
	if (!cd || !entries || fseek(f, cd_offset, SEEK_SET) != 0 || fread(cd, 1, cd_size, f) != cd_size)
	{
		free(cd);
		free(entries);
		return 0;
	}

	p = cd;
	for (i = 0; i < entries_total; i++)
	{
		fb_entry_t e;
		uint16_t name_len, extra_len, comment_len;

		if (p + 46 > cd + cd_size || fb_u32(p) != 0x02014b50)
			break;
		name_len = fb_u16(p + 28);
		extra_len = fb_u16(p + 30);
		comment_len = fb_u16(p + 32);
		if (p + 46 + name_len > cd + cd_size)
			break;

		memset(&e, 0, sizeof e);
		e.method = fb_u16(p + 10);
		e.crc = fb_u32(p + 16);
		e.packed = fb_u32(p + 20);
		e.size = fb_u32(p + 24);
		e.local = fb_u32(p + 42);
		if (name_len < sizeof e.name)
			memcpy(e.name, p + 46, name_len);

		if (name_len < sizeof e.name && fb_wanted(e.name))
		{
			if (strcmp(e.name, "bios.pk3") == 0)
			{
				bios = e;
				have_bios = 1;
			}
			else
				entries[n++] = e;
		}
		p += 46 + name_len + extra_len + comment_len;
	}
	free(cd);

	if (!have_bios)
	{
		free(entries);
		fb_log(ctx, "the archive holds no bios.pk3");
		return 0;
	}
	entries[n++] = bios;
	*out = entries;
	*count = n;
	return 1;
}

/// One file out of the archive into path, through path.part.
static int fb_unpack(fb_ctx_t* ctx, FILE* zip, const fb_entry_t* e, const char* path,
	uint64_t* done, uint64_t total)
{
	char part[FB_PATH_MAX + 8];
	uint8_t header[30];
	uint8_t* in = NULL;
	uint8_t* out = NULL;
	FILE* f = NULL;
	z_stream z;
	int z_open = 0;
	uint32_t crc = crc32(0L, Z_NULL, 0);
	uint64_t left = e->packed, written = 0;
	int ok = 0;

	snprintf(part, sizeof part, "%s.part", path);

	if (e->method != 0 && e->method != 8)
	{
		fb_log(ctx, "%s: compression method %u", e->name, e->method);
		return fb_fail_damaged(ctx);
	}
	if (fseek(zip, e->local, SEEK_SET) != 0 || fread(header, 1, 30, zip) != 30 || fb_u32(header) != 0x04034b50
		|| fseek(zip, e->local + 30 + fb_u16(header + 26) + fb_u16(header + 28), SEEK_SET) != 0)
	{
		fb_log(ctx, "%s: bad local header", e->name);
		return fb_fail_damaged(ctx);
	}

	in = (uint8_t*)malloc(FB_CHUNK);
	out = (uint8_t*)malloc(FB_CHUNK);
	f = fopen(part, "wb");
	if (!in || !out)
	{
		fb_fail(ctx, "THE CONSOLE IS OUT OF MEMORY");
		goto done;
	}
	if (!f)
	{
		fb_fail_write(ctx, part, errno);
		goto done;
	}

	memset(&z, 0, sizeof z);
	if (e->method == 8)
	{
		if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
		{
			fb_fail(ctx, "THE CONSOLE IS OUT OF MEMORY");
			goto done;
		}
		z_open = 1;
	}

	while (left > 0)
	{
		const size_t want = left < FB_CHUNK ? (size_t)left : FB_CHUNK;
		const size_t got = fread(in, 1, want, zip);

		if (got != want)
		{
			fb_log(ctx, "%s: archive ends early", e->name);
			fb_fail_damaged(ctx);
			goto done;
		}
		left -= got;
		*done += got;

		if (e->method == 0)
		{
			crc = crc32(crc, in, (uInt)got);
			if (fwrite(in, 1, got, f) != got)
			{
				fb_fail_write(ctx, part, errno);
				goto done;
			}
			written += got;
		}
		else
		{
			int r;

			z.next_in = in;
			z.avail_in = (uInt)got;
			do
			{
				size_t produced;

				z.next_out = out;
				z.avail_out = FB_CHUNK;
				r = inflate(&z, Z_NO_FLUSH);
				if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR)
				{
					fb_log(ctx, "%s: inflate %d", e->name, r);
					fb_fail_damaged(ctx);
					goto done;
				}
				produced = FB_CHUNK - z.avail_out;
				crc = crc32(crc, out, (uInt)produced);
				if (produced && fwrite(out, 1, produced, f) != produced)
				{
					fb_fail_write(ctx, part, errno);
					goto done;
				}
				written += produced;
			} while (z.avail_out == 0 && r != Z_STREAM_END);
		}

		fb_progress(ctx, FB_STAGE_EXTRACT, *done, total, 0);
	}

	if (written != e->size || crc != e->crc)
	{
		fb_log(ctx, "%s: %llu bytes, CRC %08x; expected %u, %08x", e->name,
			(unsigned long long)written, crc, e->size, e->crc);
		fb_fail_damaged(ctx);
		goto done;
	}

	if (fclose(f) != 0)
	{
		f = NULL;
		fb_fail_write(ctx, part, errno);
		goto done;
	}
	f = NULL;
	if (rename(part, path) != 0)
	{
		fb_fail_write(ctx, path, errno);
		goto done;
	}
	ok = 1;

done:
	if (z_open)
		inflateEnd(&z);
	if (f)
		fclose(f);
	if (!ok)
		remove(part);
	free(in);
	free(out);
	return ok;
}

/// The game asks for its archives by lower-case names (d_main.cpp), and some
/// in Kart Krew's archive are not (data/textures_General.pk3). A desktop's
/// file system does not mind; the console's does.
static void fb_lower(char* s)
{
	for (; *s; s++)
		*s = (char)tolower((unsigned char)*s);
}

static int fb_extract(fb_ctx_t* ctx, const char* zip_path, const char* dir)
{
	FILE* zip = fopen(zip_path, "rb");
	fb_entry_t* entries = NULL;
	size_t count = 0, i;
	uint64_t total = 0, done = 0;
	char path[FB_PATH_MAX];

	if (!zip)
		return fb_fail_damaged(ctx);
	if (!fb_read_directory(ctx, zip, &entries, &count))
	{
		fclose(zip);
		fb_log(ctx, "%s: no usable zip directory", zip_path);
		return fb_fail_damaged(ctx);
	}

	snprintf(path, sizeof path, "%s/data", dir);
	mkdir(path, 0777);

	for (i = 0; i < count; i++)
		total += entries[i].packed;

	for (i = 0; i < count; i++)
	{
		snprintf(path, sizeof path, "%s/%s", dir, entries[i].name);
		fb_lower(path + strlen(dir) + 1);
		fb_log(ctx, "unpacking %s (%u bytes)", entries[i].name, entries[i].size);
		if (!fb_unpack(ctx, zip, &entries[i], path, &done, total))
		{
			free(entries);
			fclose(zip);
			return 0;
		}
	}

	free(entries);
	fclose(zip);
	return 1;
}

// ---------------------------------------------------------------------------

int FB_Install(const char* dir, const char* ca_bundle, const fb_callbacks_t* cb, char* error, size_t error_size)
{
	fb_ctx_t ctx;
	char zip[FB_PATH_MAX];
	char part[FB_PATH_MAX + 8];

	ctx.cb = cb;
	ctx.error = error;
	ctx.error_size = error_size;
	ctx.damaged = 0;
	if (error_size)
		error[0] = '\0';

	snprintf(zip, sizeof zip, "%s/%s", dir, FB_DATA_ZIP);
	snprintf(part, sizeof part, "%s/%s.part", dir, FB_DATA_ZIP);

	// An archive under its own name has been checked already.
	if (fb_file_size(zip) < 0 && !fb_download(&ctx, zip, part, ca_bundle))
		return 0;

	if (!fb_extract(&ctx, zip, dir))
	{
		// An archive that will not unpack is no use to the next attempt
		// either, so that one downloads it again. One that failed for want
		// of space is kept.
		if (ctx.damaged)
			remove(zip);
		return 0;
	}

	remove(zip);
	fb_progress(&ctx, FB_STAGE_DONE, 1, 1, 0);
	fb_log(&ctx, "game data installed in %s", dir);
	return 1;
}
