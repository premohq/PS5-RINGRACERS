# PlayStation 5 platform setup: dependencies and the title link rule.
#
# Included from the top-level CMakeLists.txt when the PS5 toolchain
# (cmake/ps5/Toolchain-PS5.cmake) is in use, in place of the desktop
# find_package() block. Everything here is static: a native title has no
# dynamic loader for third-party code, only for system modules.

# --- Features the console build leaves out ---------------------------------
#
# Discord RPC talks to a desktop client over a local socket; there is none.
# The WebM recorder needs libyuv, which has no PS5 port. Debug links are a
# desktop packaging concern. Everything else the PC build has stays on.
set(SRB2_CONFIG_ENABLE_DISCORDRPC OFF CACHE BOOL "" FORCE)
set(SRB2_CONFIG_ENABLE_WEBM_MOVIES OFF CACHE BOOL "" FORCE)
set(SRB2_CONFIG_DISABLE_DEBUGLINK ON CACHE BOOL "" FORCE)
set(SRB2_CONFIG_SKIP_COMPTIME OFF CACHE BOOL "" FORCE)
set(SRB2_CONFIG_HWRENDER ON CACHE BOOL "" FORCE)

if(NOT PS5_OPENGL_PREFIX OR NOT EXISTS "${PS5_OPENGL_PREFIX}/include/EGL/egl.h")
	message(FATAL_ERROR "PS5_OPENGL_PREFIX must point at the ps5-opengl SDK's sdk/ directory "
		"(the one holding include/EGL and lib/libPS5OpenGLCore33.a). tools/ps5/build.sh fetches it.")
endif()

set(PS5_HOMEBREW "${PS5_PAYLOAD_SDK}/target/user/homebrew")
if(NOT EXISTS "${PS5_HOMEBREW}/lib/libcurl.a")
	message(FATAL_ERROR "The PacBrew ports are not installed into ${PS5_HOMEBREW}. tools/ps5/build.sh installs them.")
endif()

# --- Third-party libraries, as imported targets ------------------------------
#
# Spelled out rather than found, because the find modules would hand back
# pkg-config's -pthread and -l flags, which mean nothing to ld.lld when it is
# called directly, and would happily find the host's copies besides.

function(ps5_import_static name lib)
	cmake_parse_arguments(ARG "" "" "INCLUDES;DEPENDS;DEFINES" ${ARGN})
	if(NOT EXISTS "${PS5_HOMEBREW}/lib/${lib}")
		message(FATAL_ERROR "PacBrew port missing: ${PS5_HOMEBREW}/lib/${lib}")
	endif()
	add_library(${name} STATIC IMPORTED GLOBAL)
	set(includes "${PS5_HOMEBREW}/include")
	foreach(inc IN LISTS ARG_INCLUDES)
		list(APPEND includes "${PS5_HOMEBREW}/include/${inc}")
	endforeach()
	set_target_properties(${name} PROPERTIES
		IMPORTED_LOCATION "${PS5_HOMEBREW}/lib/${lib}"
		INTERFACE_INCLUDE_DIRECTORIES "${includes}"
		INTERFACE_LINK_LIBRARIES "${ARG_DEPENDS}"
		INTERFACE_COMPILE_DEFINITIONS "${ARG_DEFINES}")
endfunction()

ps5_import_static(ps5_zlib libz.a)
ps5_import_static(ps5_zstd libzstd.a)
ps5_import_static(ps5_crypto libcrypto.a)
ps5_import_static(ps5_ssl libssl.a DEPENDS ps5_crypto)
ps5_import_static(ps5_psl libpsl.a)
ps5_import_static(ps5_png libpng16.a INCLUDES libpng16 DEPENDS ps5_zlib)
ps5_import_static(ps5_curl libcurl.a
	DEPENDS "ps5_psl;ps5_ssl;ps5_crypto;ps5_zstd;ps5_zlib"
	DEFINES CURL_STATICLIB)
ps5_import_static(ps5_opus libopus.a INCLUDES opus)

add_library(ZLIB::ZLIB ALIAS ps5_zlib)
add_library(PNG::PNG ALIAS ps5_png)
add_library(CURL::libcurl ALIAS ps5_curl)
add_library(Opus::opus ALIAS ps5_opus)

# pthreads live in libkernel, which every title imports anyway.
add_library(ps5_threads INTERFACE)
add_library(Threads::Threads ALIAS ps5_threads)
set(Threads_FOUND TRUE)
set(ZLIB_FOUND TRUE)
set(PNG_FOUND TRUE)
set(CURL_FOUND TRUE)
set(Opus_FOUND TRUE)

# --- The OpenGL implementation -----------------------------------------------
#
# ps5-opengl: Mesa's Gallium state tracker over the console's own graphics
# driver, with EGL. libPS5OpenGLCore33.a is a linker GROUP script over the
# whole static stack; the two .so files are import stubs for the system's
# AGC modules, which every title can load.
add_library(ps5_opengl INTERFACE)
target_include_directories(ps5_opengl BEFORE INTERFACE "${PS5_OPENGL_PREFIX}/include")
add_library(PS5::OpenGL ALIAS ps5_opengl)

# --- The title link rule ------------------------------------------------------
#
# The same link the native-app boilerplate performs (its tools/build.sh), with
# the additions ps5-opengl's own native apps make: the malloc family wrapped
# onto src/ps5/native/app_heap.c, and the __eh_frame bounds libunwind needs to
# find unwind tables without a dynamic loader to ask. The ELF this produces is
# then converted and signed by tools/ps5/package.sh; it is not runnable as is.
set(PS5_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/src/ps5/native/ps5-pie.ld")
set(PS5_SYMBOL_MAP "${CMAKE_SOURCE_DIR}/src/ps5/native/app-symbols.map")

execute_process(COMMAND "${CMAKE_C_COMPILER}" --print-resource-dir
	OUTPUT_VARIABLE _ps5_resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE)
set(PS5_COMPILER_RT "${_ps5_resource_dir}/lib/linux/libclang_rt.builtins-x86_64.a"
	CACHE FILEPATH "compiler-rt builtins for x86_64 (Debian/Ubuntu: libclang-rt-18-dev)")
if(NOT EXISTS "${PS5_COMPILER_RT}")
	message(FATAL_ERROR "compiler-rt builtins not found at ${PS5_COMPILER_RT}. "
		"On Debian or Ubuntu: apt install libclang-rt-18-dev")
endif()

# The system modules a title may import: the payload SDK's stubs, less one,
# plus ps5-opengl's two for the graphics driver.
#
# The one left out is libScePosixForWebKit, a module that exists for the
# browser and is where the stubs put getaddrinfo, strcasestr and
# arc4random_buf. Whether the loader hands it to a game has not been
# established, and every network feature needs getaddrinfo, so those come
# from the payload SDK's libc.a instead (below), whose getaddrinfo sits on
# libSceNet's resolver.
file(GLOB PS5_SYSTEM_STUBS "${PS5_PAYLOAD_SDK}/target/lib/*.so")
list(FILTER PS5_SYSTEM_STUBS EXCLUDE REGEX "/libSceAgc(Driver)?\\.so$")
list(FILTER PS5_SYSTEM_STUBS EXCLUDE REGEX "/libScePosixForWebKit\\.so$")
list(APPEND PS5_SYSTEM_STUBS
	"${PS5_OPENGL_PREFIX}/lib/libSceAgc.so"
	"${PS5_OPENGL_PREFIX}/lib/libSceAgcDriver.so")

set(PS5_LINK_TAIL
	--start-group
	"${PS5_OPENGL_PREFIX}/lib/libPS5OpenGLCore33.a"
	"${PS5_PAYLOAD_SDK}/target/lib/libunwind.a"
	"${PS5_PAYLOAD_SDK}/target/lib/libc++abi.a"
	"${PS5_PAYLOAD_SDK}/target/lib/libc++.a"
	"${PS5_COMPILER_RT}"
	--end-group
	--as-needed
	${PS5_SYSTEM_STUBS}
	# The payload SDK's supplement to the system libc: the locale (_l)
	# variants libc++ calls, setjmp, getaddrinfo and the rest the system
	# modules do not export. It goes after the stubs on purpose. It also
	# carries payload versions of mmap and mprotect that call into the
	# exploit's kernel primitives, which a title does not have; placed
	# first, those replace the system's own. Placed last, it is only
	# consulted for what no system module provides.
	"${PS5_PAYLOAD_SDK}/target/lib/libc.a")

# A title has no run-time search path, and lld called directly does not
# understand the -Wl, form CMake would write one in.
set(CMAKE_SKIP_RPATH ON)

# prospero-lld adds -pie, --eh-frame-hdr, the 16 KiB page size and emulated
# TLS; -T replaces its payload linker script with the title layout.
set(CMAKE_CXX_LINK_EXECUTABLE
	"<CMAKE_LINKER> <LINK_FLAGS> <CMAKE_CXX_LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>")
set(CMAKE_C_LINK_EXECUTABLE "${CMAKE_CXX_LINK_EXECUTABLE}")
