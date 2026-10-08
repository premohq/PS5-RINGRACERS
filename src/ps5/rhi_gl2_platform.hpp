// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Ronald "Eidolon" Kinard
// Copyright (C) 2026 by Kart Krew
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------

#ifndef SRB2_PS5_RHI_GL2_PLATFORM_HPP
#define SRB2_PS5_RHI_GL2_PLATFORM_HPP

#include <tuple>

#include "../core/string.h"
#include "../core/vector.hpp"
#include "../rhi/gl2/gl2_rhi.hpp"
#include "../rhi/rhi.hpp"

namespace srb2::rhi
{

/// The GL2 backend's window-system half: the same as SdlGl2Platform, with
/// EGL behind it instead of SDL's GL context.
struct Ps5Gl2Platform final : public Gl2Platform
{
	virtual ~Ps5Gl2Platform();

	virtual void present() override;
	virtual std::tuple<Vector<srb2::String>, Vector<srb2::String>>
	find_shader_sources(const char* name) override;
	virtual Rect get_default_framebuffer_dimensions() override;
};

} // namespace srb2::rhi

#endif // SRB2_PS5_RHI_GL2_PLATFORM_HPP
