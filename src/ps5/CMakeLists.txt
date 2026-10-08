# PlayStation 5 platform layer, in place of src/sdl.
#
# Built only with cmake/ps5/Toolchain-PS5.cmake; see tools/ps5/build.sh.

target_sources(SRB2SDL2 PRIVATE
	i_main.cpp
	i_system.cpp
	i_video.cpp
	i_input.cpp
	i_keyboard.cpp
	i_sound.cpp
	i_threads.c
	i_net.c
	ogl_ps5.c
	ps5_paths.cpp
	rhi_gl2_platform.cpp

	# The title's C runtime: startup, the malloc heap, and what libc lacks.
	native/app_crt.cpp
	native/app_heap.c
	native/runtime_shims.c
)

target_compile_definitions(SRB2SDL2 PRIVATE -DSRB2_PS5
	# HTTPS certificates for the master server and add-on downloads; the
	# packager copies them into the title folder.
	"PS5_CA_BUNDLE=\"/app0/ca-bundle.crt\"")
target_link_libraries(SRB2SDL2 PRIVATE PS5::OpenGL)

# The link rule itself is in cmake/ps5/PS5Platform.cmake; these are its
# inputs that belong to this target.
target_link_options(SRB2SDL2 PRIVATE
	-T "${PS5_LINKER_SCRIPT}"
	--version-script "${PS5_SYMBOL_MAP}"
	-e _start
	--gc-sections
	# libc++ asks for "pthread" by name; the payload SDK's libpthread.a.
	-L "${PS5_PAYLOAD_SDK}/target/lib"
	# ps5-opengl's AGC runtime backend is reached only through this symbol.
	-u ps5_agc_gate2_run
	# malloc and friends go to native/app_heap.c's direct-memory heap.
	--wrap=malloc --wrap=calloc --wrap=realloc --wrap=free
	--wrap=posix_memalign --wrap=malloc_usable_size
	# Threads made without attributes get a desktop-sized stack; see
	# native/runtime_shims.c.
	--wrap=pthread_create
	# Which archive member supplied what, for when an import goes wrong.
	-Map "${CMAKE_BINARY_DIR}/ringracers.map"
)
set_property(TARGET SRB2SDL2 APPEND PROPERTY LINK_DEPENDS "${PS5_LINKER_SCRIPT}" "${PS5_SYMBOL_MAP}")

# After every library CMake knows about: the OpenGL stack, the C++ runtime
# and the system module stubs.
target_link_libraries(SRB2SDL2 PRIVATE ${PS5_LINK_TAIL})
