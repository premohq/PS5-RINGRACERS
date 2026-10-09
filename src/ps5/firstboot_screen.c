// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps5/firstboot_screen.c
/// \brief First boot: the screen shown while the game data downloads
///
/// The launch screen's track and logo, with the game's console font for the
/// words, laid out on the game's 320x180 grid and drawn at twice that. Text
/// is spaced as the game spaces this font (v_video.cpp: each glyph advances
/// one pixel less than its width, a space is four), so it reads like the
/// game's own loading screen. The art comes from firstboot.dat, which
/// tools/ps5/firstboot_art.py writes into the title.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "firstboot.h"

/// Layout units: the game's own grid, drawn at twice the size.
#define UNIT 2
#define UNITS_W (FB_SCREEN_W / UNIT)
#define UNITS_H (FB_SCREEN_H / UNIT)

/// The handle in the bottom left corner, where the game puts its own
/// loading step.
#define FB_CREDIT "@premohq"

typedef struct
{
	int w, h, top;
	uint8_t* rgba;
} fb_glyph_t;

static uint8_t* g_background;
static int g_first_char;
static int g_char_count;
static fb_glyph_t* g_glyphs;

static uint16_t rd16(const uint8_t* p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

int FB_LoadArt(const char* path)
{
	FILE* f = fopen(path, "rb");
	uint8_t* data;
	long size;
	size_t at = 6;
	int i;

	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = (uint8_t*)malloc((size_t)size);
	if (!data || size < 10 || fread(data, 1, (size_t)size, f) != (size_t)size || memcmp(data, "RRFB", 4) != 0
		|| rd16(data + 4) != 1)
	{
		fclose(f);
		free(data);
		return 0;
	}
	fclose(f);

	// The data stays loaded for as long as the screen is up; the pointers
	// below are into it.
	if (rd16(data + at) != FB_SCREEN_W || rd16(data + at + 2) != FB_SCREEN_H)
		goto bad;
	at += 4;
	if (at + (size_t)FB_SCREEN_W * FB_SCREEN_H * 4 > (size_t)size)
		goto bad;
	g_background = data + at;
	at += (size_t)FB_SCREEN_W * FB_SCREEN_H * 4;

	if (at + 4 > (size_t)size)
		goto bad;
	g_first_char = rd16(data + at);
	g_char_count = rd16(data + at + 2);
	at += 4;
	g_glyphs = (fb_glyph_t*)calloc((size_t)g_char_count, sizeof *g_glyphs);
	if (!g_glyphs)
		goto bad;
	for (i = 0; i < g_char_count; i++)
	{
		fb_glyph_t* g = &g_glyphs[i];
		if (at + 4 > (size_t)size)
			goto bad;
		g->w = data[at];
		g->h = data[at + 1];
		g->top = (int8_t)data[at + 2];
		at += 4;
		if (at + (size_t)g->w * g->h * 4 > (size_t)size)
			goto bad;
		g->rgba = data + at;
		at += (size_t)g->w * g->h * 4;
	}
	return 1;

bad:
	free(g_glyphs);
	g_glyphs = NULL;
	g_background = NULL;
	g_char_count = 0;
	free(data);
	return 0;
}

// ---------------------------------------------------------------------------
// Drawing, in layout units
// ---------------------------------------------------------------------------

static void blend(uint8_t* px, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	px[0] = (uint8_t)((px[0] * (255 - a) + r * a) / 255);
	px[1] = (uint8_t)((px[1] * (255 - a) + g * a) / 255);
	px[2] = (uint8_t)((px[2] * (255 - a) + b * a) / 255);
	px[3] = 255;
}

static void fill(uint8_t* screen, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	int px, py;

	for (py = y * UNIT; py < (y + h) * UNIT; py++)
	{
		if (py < 0 || py >= FB_SCREEN_H)
			continue;
		for (px = x * UNIT; px < (x + w) * UNIT; px++)
		{
			if (px >= 0 && px < FB_SCREEN_W)
				blend(screen + ((size_t)py * FB_SCREEN_W + px) * 4, r, g, b, a);
		}
	}
}

static const fb_glyph_t* glyph(char c)
{
	const int i = (unsigned char)c - g_first_char;
	if (i < 0 || i >= g_char_count || g_glyphs[i].w == 0)
		return NULL;
	return &g_glyphs[i];
}

static int text_width(const char* s)
{
	int w = 0;
	for (; *s; s++)
	{
		const fb_glyph_t* g = glyph(*s);
		w += g ? (g->w > 1 ? g->w - 1 : 1) : 4;
	}
	return w;
}

/// A glyph, with a shadow one unit down and right so the white letters
/// read over the bright parts of the track. tint multiplies its colour:
/// white for the font as it is.
static void draw_glyph(uint8_t* screen, const fb_glyph_t* g, int x, int y, const uint8_t tint[3])
{
	int pass, gx, gy, sx, sy;

	for (pass = 0; pass < 2; pass++)
	{
		const int ox = pass == 0 ? UNIT : 0;
		for (gy = 0; gy < g->h; gy++)
		{
			for (gx = 0; gx < g->w; gx++)
			{
				const uint8_t* src = g->rgba + ((size_t)gy * g->w + gx) * 4;
				if (src[3] == 0)
					continue;
				for (sy = 0; sy < UNIT; sy++)
				{
					const int py = (y + gy) * UNIT + sy + ox;
					if (py < 0 || py >= FB_SCREEN_H)
						continue;
					for (sx = 0; sx < UNIT; sx++)
					{
						const int px = (x + gx) * UNIT + sx + ox;
						if (px < 0 || px >= FB_SCREEN_W)
							continue;
						if (pass == 0)
							blend(screen + ((size_t)py * FB_SCREEN_W + px) * 4, 0, 0, 0, 160);
						else
							blend(screen + ((size_t)py * FB_SCREEN_W + px) * 4, (uint8_t)(src[0] * tint[0] / 255),
								(uint8_t)(src[1] * tint[1] / 255), (uint8_t)(src[2] * tint[2] / 255), src[3]);
					}
				}
			}
		}
	}
}

static const uint8_t WHITE[3] = {255, 255, 255};
/// The game's yellow text, near enough (V_YELLOWMAP).
static const uint8_t YELLOW[3] = {255, 222, 64};

static void draw_tinted(uint8_t* screen, int x, int y, const char* s, const uint8_t tint[3])
{
	for (; *s; s++)
	{
		const fb_glyph_t* g = glyph(*s);
		if (!g)
		{
			x += 4;
			continue;
		}
		draw_glyph(screen, g, x, y - g->top, tint);
		x += g->w > 1 ? g->w - 1 : 1;
	}
}

static void draw_text(uint8_t* screen, int x, int y, const char* s)
{
	draw_tinted(screen, x, y, s, WHITE);
}

static void draw_centered(uint8_t* screen, int y, const char* s)
{
	draw_text(screen, (UNITS_W - text_width(s)) / 2, y, s);
}

static void draw_right(uint8_t* screen, int right, int y, const char* s)
{
	draw_text(screen, right - text_width(s), y, s);
}

// ---------------------------------------------------------------------------

#define BAR_X 40
#define BAR_W (UNITS_W - 2 * BAR_X)
#define BAR_Y 114
#define BAR_H 9

static void draw_bar(uint8_t* screen, uint64_t done, uint64_t total)
{
	const int inner = BAR_W - 2;
	int filled = total ? (int)((double)done / (double)total * inner) : 0;

	if (filled > inner)
		filled = inner;

	fill(screen, BAR_X + 1, BAR_Y + 1, BAR_W, BAR_H, 0, 0, 0, 160);          // shadow
	fill(screen, BAR_X, BAR_Y, BAR_W, BAR_H, 255, 255, 255, 255);            // frame
	fill(screen, BAR_X + 1, BAR_Y + 1, inner, BAR_H - 2, 20, 12, 60, 255);   // track
	fill(screen, BAR_X + 1, BAR_Y + 1, filled, BAR_H - 2, 255, 255, 255, 255);
}

static void megabytes(char* out, size_t size, uint64_t done, uint64_t total)
{
	snprintf(out, size, "%llu / %llu MB",
		(unsigned long long)(done / 1000000), (unsigned long long)((total + 500000) / 1000000));
}

void FB_Render(uint8_t* screen, const fb_view_t* view)
{
	static const char* const dots[] = {"", ".", "..", "..."};
	const char* heading = "";
	char line[64];
	int heading_x;

	if (g_background)
		memcpy(screen, g_background, (size_t)FB_SCREEN_W * FB_SCREEN_H * 4);
	else
		memset(screen, 0, (size_t)FB_SCREEN_W * FB_SCREEN_H * 4);

	if (!g_glyphs)
	{
		// No art: the bar alone still says that something is happening.
		if (view->stage != FB_STAGE_FAILED)
			draw_bar(screen, view->done, view->total);
		return;
	}

	switch (view->stage)
	{
		case FB_STAGE_DOWNLOAD: heading = "DOWNLOADING THE GAME FILES"; break;
		case FB_STAGE_VERIFY:   heading = "CHECKING THE DOWNLOAD"; break;
		case FB_STAGE_EXTRACT:  heading = "UNPACKING THE GAME FILES"; break;
		case FB_STAGE_DONE:     heading = "READY!"; break;
		case FB_STAGE_FAILED:   heading = "COULD NOT GET THE GAME FILES"; break;
	}

	// Centred without its dots, so it does not shuffle as they come and go.
	heading_x = (UNITS_W - text_width(heading)) / 2;
	draw_text(screen, heading_x, 98, heading);
	if (view->stage < FB_STAGE_DONE)
		draw_text(screen, heading_x + text_width(heading), 98, dots[(view->frame / 20) % 4]);

	if (view->stage == FB_STAGE_FAILED)
	{
		const char* reason = view->error && view->error[0] ? view->error : "SOMETHING WENT WRONG";
		draw_tinted(screen, (UNITS_W - text_width(reason)) / 2, 116, reason, YELLOW);
		draw_centered(screen, 134, "CLOSE THE GAME AND START IT AGAIN,");
		draw_centered(screen, 144, "OR COPY BIOS.PK3 AND DATA/ TO");
		draw_centered(screen, 154, "/DATA/RINGRACERS OVER FTP");
	}
	else
	{
		draw_bar(screen, view->done, view->total);

		if (view->stage == FB_STAGE_DOWNLOAD)
		{
			megabytes(line, sizeof line, view->done, view->total);
			draw_text(screen, BAR_X, 128, line);
			if (view->rate)
			{
				snprintf(line, sizeof line, "%.1f MB/S", (double)view->rate / 1e6);
				draw_right(screen, BAR_X + BAR_W, 128, line);
			}
		}
		else if (view->stage != FB_STAGE_DONE)
		{
			snprintf(line, sizeof line, "%d%%",
				view->total ? (int)(view->done * 100 / view->total) : 0);
			draw_right(screen, BAR_X + BAR_W, 128, line);
		}

		draw_centered(screen, 146, "THIS ONLY HAPPENS ON THE FIRST BOOT");
	}

	draw_text(screen, 4, UNITS_H - 14, FB_CREDIT);
}
