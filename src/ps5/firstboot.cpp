// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2026 by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps5/firstboot.cpp
/// \brief First boot: the PS5 side of fetching the game data
///
/// i_main.cpp calls this before the game starts, when ps5_paths.cpp found
/// no bios.pk3. The download and unpacking (firstboot_install.c) run on a
/// thread of their own, so that waiting for the display never holds up the
/// network; this thread draws the screen (firstboot_screen.c) into a
/// texture and shows it through the EGL context the game goes on to use,
/// then deletes everything it made in that context. The PS5's launch
/// screen stays up until the first frame is ready, so there is no black
/// gap between the two.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <glad/gl.h>

#include "../doomdef.h"

#include "firstboot.h"
#include "ps5_paths.h"
#include "ps5_sys.h"
#include "ps5_video.h"

namespace
{

/// Written by tools/ps5/firstboot_art.py.
constexpr const char* kArt = "/app0/firstboot.dat";

#ifdef PS5_CA_BUNDLE
constexpr const char* kCaBundle = PS5_CA_BUNDLE;
#else
constexpr const char* kCaBundle = nullptr;
#endif

/// What the installing thread reports and the drawing thread reads.
struct Shared
{
	std::mutex mutex;
	fb_stage_t stage = FB_STAGE_DOWNLOAD;
	uint64_t done = 0;
	uint64_t total = FB_DATA_SIZE;
	uint64_t rate = 0;
	bool finished = false;
	bool ok = false;
	char error[160] = "";
};

void OnProgress(void* user, fb_stage_t stage, uint64_t done, uint64_t total, uint64_t rate)
{
	Shared* shared = static_cast<Shared*>(user);
	std::lock_guard<std::mutex> lock(shared->mutex);
	shared->stage = stage;
	shared->done = done;
	shared->total = total;
	shared->rate = rate;
}

void OnLog(void*, const char* line)
{
	I_OutputMsg("[firstboot] %s\n", line);
}

GLADapiproc LoadGL(const char* name)
{
	return reinterpret_cast<GLADapiproc>(PS5_GLGetProcAddress(name));
}

/// Puts the 640x360 screen on the display, scaled by the largest whole
/// number that fits (3 at 1080p, 4 at 1440p, 6 at 4K) so the pixel art stays
/// sharp. GL 2.1 and GLSL 1.20, as the game's own renderer uses.
class Presenter
{
public:
	bool Init()
	{
		if (!gladLoadGLContext(&gl_, LoadGL))
			return Fail("could not load OpenGL");

		static const char* const vertex =
			"#version 120\n"
			"attribute vec2 a_pos;\n"
			"varying vec2 v_uv;\n"
			"void main() {\n"
			"	v_uv = vec2(a_pos.x * 0.5 + 0.5, 0.5 - a_pos.y * 0.5);\n"
			"	gl_Position = vec4(a_pos, 0.0, 1.0);\n"
			"}\n";
		static const char* const fragment =
			"#version 120\n"
			"uniform sampler2D u_tex;\n"
			"varying vec2 v_uv;\n"
			"void main() {\n"
			"	gl_FragColor = texture2D(u_tex, v_uv);\n"
			"}\n";

		vs_ = Compile(GL_VERTEX_SHADER, vertex);
		fs_ = Compile(GL_FRAGMENT_SHADER, fragment);
		if (!vs_ || !fs_)
			return Fail("could not compile the shaders");

		program_ = gl_.CreateProgram();
		gl_.AttachShader(program_, vs_);
		gl_.AttachShader(program_, fs_);
		gl_.BindAttribLocation(program_, 0, "a_pos");
		gl_.LinkProgram(program_);
		GLint linked = 0;
		gl_.GetProgramiv(program_, GL_LINK_STATUS, &linked);
		if (!linked)
			return Fail("could not link the shaders");

		gl_.UseProgram(program_);
		gl_.Uniform1i(gl_.GetUniformLocation(program_, "u_tex"), 0);

		static const GLfloat quad[] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
		gl_.GenBuffers(1, &buffer_);
		gl_.BindBuffer(GL_ARRAY_BUFFER, buffer_);
		gl_.BufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);

		gl_.GenTextures(1, &texture_);
		gl_.ActiveTexture(GL_TEXTURE0);
		gl_.BindTexture(GL_TEXTURE_2D, texture_);
		gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, FB_SCREEN_W, FB_SCREEN_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

		ready_ = gl_.GetError() == GL_NO_ERROR;
		if (!ready_)
			return Fail("could not set up the screen's texture");
		return true;
	}

	void Show(const uint8_t* rgba)
	{
		int width = 0, height = 0;
		PS5_VideoDrawableSize(&width, &height);
		const int scale = std::max(1, std::min(width / FB_SCREEN_W, height / FB_SCREEN_H));
		const int w = FB_SCREEN_W * scale, h = FB_SCREEN_H * scale;

		gl_.Viewport(0, 0, width, height);
		gl_.ClearColor(0.f, 0.f, 0.f, 1.f);
		gl_.Clear(GL_COLOR_BUFFER_BIT);
		gl_.Viewport((width - w) / 2, (height - h) / 2, w, h);

		gl_.UseProgram(program_);
		gl_.ActiveTexture(GL_TEXTURE0);
		gl_.BindTexture(GL_TEXTURE_2D, texture_);
		gl_.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, FB_SCREEN_W, FB_SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
		gl_.BindBuffer(GL_ARRAY_BUFFER, buffer_);
		gl_.EnableVertexAttribArray(0);
		gl_.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
		gl_.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		PS5_VideoPresent();
	}

	/// Leave the context as the game's renderer expects to find it.
	void Shutdown()
	{
		if (!gl_.UseProgram)
			return;
		gl_.DisableVertexAttribArray(0);
		gl_.BindBuffer(GL_ARRAY_BUFFER, 0);
		gl_.BindTexture(GL_TEXTURE_2D, 0);
		gl_.UseProgram(0);
		if (texture_)
			gl_.DeleteTextures(1, &texture_);
		if (buffer_)
			gl_.DeleteBuffers(1, &buffer_);
		if (program_)
			gl_.DeleteProgram(program_);
		if (vs_)
			gl_.DeleteShader(vs_);
		if (fs_)
			gl_.DeleteShader(fs_);
		texture_ = buffer_ = program_ = vs_ = fs_ = 0;

		int width = 0, height = 0;
		PS5_VideoDrawableSize(&width, &height);
		gl_.Viewport(0, 0, width, height);
		ready_ = false;
	}

	bool ready() const { return ready_; }

private:
	GLuint Compile(GLenum type, const char* source)
	{
		GLuint shader = gl_.CreateShader(type);
		gl_.ShaderSource(shader, 1, &source, nullptr);
		gl_.CompileShader(shader);
		GLint ok = 0;
		gl_.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
		if (!ok)
		{
			char log[512] = "";
			gl_.GetShaderInfoLog(shader, sizeof log, nullptr, log);
			I_OutputMsg("[firstboot] shader: %s\n", log);
			gl_.DeleteShader(shader);
			return 0;
		}
		return shader;
	}

	bool Fail(const char* what)
	{
		I_OutputMsg("[firstboot] %s; installing without a screen\n", what);
		Shutdown();
		return false;
	}

	GladGLContext gl_ = {};
	GLuint vs_ = 0, fs_ = 0, program_ = 0, buffer_ = 0, texture_ = 0;
	bool ready_ = false;
};

} // namespace

void PS5_FirstBoot(void)
{
	if (PS5_DataDir())
		return;

	// Where the data goes: the home directory, which ps5_paths.cpp chose for
	// being writable. That is /data/ringracers, where an FTP client reaches
	// it, when the loader lets the title write there, and otherwise the
	// title's own /download0, which downloadDataSize in param.json makes
	// big enough for the archive and what it unpacks to.
	const char* const install_dir = PS5_HomeDir();

	I_OutputMsg("First boot: no game data found; installing it into %s\n", install_dir);

	if (!FB_LoadArt(kArt))
		I_OutputMsg("[firstboot] %s is missing or damaged; drawing plain shapes\n", kArt);

	Presenter presenter;
	bool display = PS5_VideoStart() && presenter.Init();
	if (!display)
	{
		// The launch screen stays up instead, and the home screen's
		// notifications say what is going on.
		PS5_Notify("Ring Racers: downloading the game files.\nThis only happens on the first boot.");
	}

	Shared shared;

	curl_global_init(CURL_GLOBAL_ALL);
	std::thread worker([&shared, install_dir]() {
		fb_callbacks_t callbacks = {OnProgress, OnLog, &shared};
		char error[sizeof shared.error];
		const int ok = FB_Install(install_dir, kCaBundle, &callbacks, error, sizeof error);

		std::lock_guard<std::mutex> lock(shared.mutex);
		shared.ok = ok != 0;
		snprintf(shared.error, sizeof shared.error, "%s", error);
		shared.finished = true;
	});

	std::vector<uint8_t> screen(static_cast<size_t>(FB_SCREEN_W) * FB_SCREEN_H * 4);
	fb_view_t view = {};
	bool splash_hidden = false;

	auto show = [&]() {
		if (!display)
		{
			sceKernelUsleep(100 * 1000);
			return;
		}
		FB_Render(screen.data(), &view);
		presenter.Show(screen.data());
		if (!splash_hidden)
		{
			sceSystemServiceHideSplashScreen();
			splash_hidden = true;
		}
		view.frame++;
	};

	// Until the install finishes, once a display refresh.
	for (;;)
	{
		{
			std::lock_guard<std::mutex> lock(shared.mutex);
			if (shared.finished)
				break;
			view.stage = shared.stage;
			view.done = shared.done;
			view.total = shared.total;
			view.rate = shared.rate;
		}
		show();
	}

	worker.join();
	curl_global_cleanup();

	if (shared.ok)
	{
		// A second of "ready", then the game's own startup.
		view.stage = FB_STAGE_DONE;
		view.done = view.total = 1;
		for (int i = 0; i < 60; i++)
			show();
		presenter.Shutdown();
		PS5_RescanPaths();
		I_OutputMsg("First boot: done; data directory %s\n", PS5_DataDir() ? PS5_DataDir() : "(still none)");
		return;
	}

	// No way on without the data: say what went wrong and what to do, until
	// the player closes the title. The next start tries again, and resumes.
	I_OutputMsg("First boot: failed: %s\n", shared.error);
	PS5_Notify("Ring Racers could not download its game files.\nClose it and start it again to retry.");
	view.stage = FB_STAGE_FAILED;
	view.error = shared.error;
	for (;;)
		show();
}
