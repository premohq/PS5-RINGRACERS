# CMake toolchain for the PlayStation 5, as a native title (eboot.bin).
#
#   cmake -B build/ps5 -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/ps5/Toolchain-PS5.cmake \
#     -DPS5_PAYLOAD_SDK=/opt/ps5-payload-sdk \
#     -DPS5_OPENGL_PREFIX=/opt/ps5-opengl/sdk
#
# tools/ps5/build.sh does all of this for you, including fetching the SDKs.
#
# The compiler is the host's clang-18 aimed at x86_64-sie-ps5, with the same
# flags the native-app boilerplate's prospero-clang18 wrapper applies. That is
# deliberately not the payload SDK's own prospero-clang: that wrapper adds the
# payload CRT and libkernel_web to every link, and a native title has its own
# startup code (src/ps5/native/app_crt.cpp) and is linked by the rule in
# cmake/ps5/PS5Link.cmake instead.

set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_VERSION 9)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_CROSSCOMPILING ON)
set(PS5 ON)

if(NOT PS5_PAYLOAD_SDK)
	if(DEFINED ENV{PS5_PAYLOAD_SDK})
		set(PS5_PAYLOAD_SDK "$ENV{PS5_PAYLOAD_SDK}")
	else()
		set(PS5_PAYLOAD_SDK "/opt/ps5-payload-sdk")
	endif()
endif()
if(NOT PS5_OPENGL_PREFIX)
	if(DEFINED ENV{PS5_OPENGL_PREFIX})
		set(PS5_OPENGL_PREFIX "$ENV{PS5_OPENGL_PREFIX}")
	endif()
endif()
set(PS5_PAYLOAD_SDK "${PS5_PAYLOAD_SDK}" CACHE PATH "ps5-payload-sdk (v0.42) root")
set(PS5_OPENGL_PREFIX "${PS5_OPENGL_PREFIX}" CACHE PATH "ps5-opengl SDK prefix (the directory holding include/EGL and lib/libPS5OpenGLCore33.a)")
# try_compile runs in a separate project that does not see the cache.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PS5_PAYLOAD_SDK PS5_OPENGL_PREFIX)

if(NOT EXISTS "${PS5_PAYLOAD_SDK}/target/include/stdio.h")
	message(FATAL_ERROR "PS5_PAYLOAD_SDK (${PS5_PAYLOAD_SDK}) is not a ps5-payload-sdk. Run tools/ps5/build.sh, which fetches it.")
endif()

find_program(PS5_CLANG NAMES clang-18 clang REQUIRED)
find_program(PS5_CLANGXX NAMES clang++-18 clang++ REQUIRED)
set(CMAKE_C_COMPILER "${PS5_CLANG}")
set(CMAKE_CXX_COMPILER "${PS5_CLANGXX}")
set(CMAKE_C_COMPILER_TARGET x86_64-sie-ps5)
set(CMAKE_CXX_COMPILER_TARGET x86_64-sie-ps5)
set(CMAKE_LINKER "${PS5_PAYLOAD_SDK}/bin/prospero-lld")
set(CMAKE_AR "${PS5_PAYLOAD_SDK}/bin/llvm-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${PS5_PAYLOAD_SDK}/bin/llvm-ranlib" CACHE FILEPATH "")
set(CMAKE_C_COMPILER_AR "${CMAKE_AR}" CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER_AR "${CMAKE_AR}" CACHE FILEPATH "")
set(CMAKE_C_COMPILER_RANLIB "${CMAKE_RANLIB}" CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER_RANLIB "${CMAKE_RANLIB}" CACHE FILEPATH "")
set(CMAKE_OBJCOPY "${PS5_PAYLOAD_SDK}/bin/llvm-objcopy" CACHE FILEPATH "")

# The flags prospero-clang18 applies. -femulated-tls because the title's TLS
# goes through __emutls like every other PS5 module; -fno-plt and no stack
# protector because there is no __stack_chk_guard to import.
set(_ps5_flags
	"-fvisibility-nodllstorageclass=default"
	"-isysroot ${PS5_PAYLOAD_SDK}"
	"-fno-stack-protector -fno-plt -femulated-tls"
	"-ffunction-sections -fdata-sections")
string(JOIN " " _ps5_flags ${_ps5_flags})
set(CMAKE_C_FLAGS_INIT "${_ps5_flags}")
# The SIE target turns exceptions and RTTI off unless asked. The engine uses
# both: std::runtime_error out of the RHI and the audio decoders, typeid in the
# crash report. libc++abi and libunwind from the payload SDK provide them.
set(CMAKE_CXX_FLAGS_INIT "${_ps5_flags} -fexceptions -fcxx-exceptions -frtti")
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES "${PS5_PAYLOAD_SDK}/target/include")
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES
	"${PS5_PAYLOAD_SDK}/target/include/c++/v1"
	"${PS5_PAYLOAD_SDK}/target/include")

# Nothing links without the title's own CRT, so checks compile only.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Libraries come from the payload SDK and the PacBrew ports installed into it
# (target/user/homebrew). Never from the host.
set(CMAKE_SYSROOT_COMPILE "")
set(CMAKE_FIND_ROOT_PATH "${PS5_PAYLOAD_SDK}/target" "${PS5_PAYLOAD_SDK}/target/user/homebrew")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
