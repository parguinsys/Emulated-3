# EIII Rack plugin, CMake build: the CLAP and, through clap-wrapper, a VST3 from the same sources and flags as
# `make plugin` in standalone/ (which stays the build for the standalone and the regression tests).
#
#   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --target eiii-rack_all
#   -> build/plugins/EIII Rack.clap, build/plugins/EIII Rack.vst3
#
# clap-wrapper and the VST3 SDK are downloaded at configure time (versions pinned below); -DVST3_SDK_ROOT=<path> uses a
# local VST3 SDK instead.  -DEIII_VST3=OFF builds the CLAP alone, with no download.
cmake_minimum_required(VERSION 3.21)
project(eiii-rack VERSION 0.1.0 LANGUAGES C CXX)

option(EIII_VST3 "Build the VST3 (clap-wrapper)" ON)
set(EIII_CLAP_WRAPPER_TAG v0.16.0 CACHE STRING "clap-wrapper release")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	# no fused multiply-add contraction, so the analog model is bit-identical on x86-64 and ARM64 (as in the Makefiles)
	add_compile_options(-ffp-contract=off)
	set(EIII_WARN -Wall -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable)
endif()

set(MAME ${CMAKE_CURRENT_SOURCE_DIR}/standalone/third_party/mame)

# ---- Berkeley SoftFloat 3e, MAME's configuration (compiled as C++ like MAME does)
file(STRINGS ${MAME}/softfloat3_files.txt sf_files)
list(TRANSFORM sf_files PREPEND ${MAME}/)
set_source_files_properties(${sf_files} PROPERTIES LANGUAGE CXX)
add_library(eiii_softfloat3 STATIC ${sf_files})
set_target_properties(eiii_softfloat3 PROPERTIES CXX_STANDARD 17)
target_compile_definitions(eiii_softfloat3 PRIVATE SOFTFLOAT_ROUND_ODD INLINE_LEVEL=5 SOFTFLOAT_FAST_DIV32TO16
	SOFTFLOAT_FAST_DIV64TO32 SOFTFLOAT_FAST_INT64 LITTLEENDIAN=1)
target_include_directories(eiii_softfloat3 PRIVATE ${MAME}/src/osd ${MAME}/3rdparty/softfloat3/build/MAME
	${MAME}/3rdparty/softfloat3/source ${MAME}/3rdparty/softfloat3/source/include ${MAME}/3rdparty/softfloat3/source/8086
	${MAME}/3rdparty/softfloat3/bochs_ext)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(eiii_softfloat3 PRIVATE -w)
endif()

# ---- runtime: one emulated EIII (MAME devices + shim, motherboard, eiii-core voice hardware, panel model)
set(mame_src
	devices/cpu/ns32000/ns32000.cpp devices/cpu/ns32000/ns32000d.cpp devices/machine/ns32081.cpp
	devices/machine/nscsi_bus.cpp devices/machine/ncr5380.cpp devices/machine/nscsi_hle.cpp devices/bus/nscsi/hd.cpp
	devices/bus/nscsi/cd.cpp devices/machine/z80scc.cpp devices/machine/z80daisy.cpp lib/util/strformat.cpp
	lib/util/coreutil.cpp lib/util/disasmintf.cpp)
list(TRANSFORM mame_src PREPEND ${MAME}/src/)
# upstream MAME files and eiii_scsi.cpp (extracted verbatim from emu3.cpp) are compiled unmodified: no warnings
set_source_files_properties(${mame_src} standalone/eiii_scsi.cpp PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang>:-w>")
add_library(eiii_runtime STATIC
	standalone/shim/emu_shim.cpp standalone/eiii_scsi.cpp standalone/diserial_shim.cpp
	fchip/eiii_fchip.cpp cem/eiii_cem3387.cpp cem/eiii_cem_calibration.cpp cem/eiii_cem_cv_conditioning.cpp
	cem/eiii_cem3387_v2.cpp host/eiii_backend.cpp host/eiii_board_link.cpp runtime/eiii_runtime.cpp runtime/eiii_engine.cpp runtime/eiii_bank_import.cpp
	panel/eiii_lcd.cpp panel/eiii_panel_model.cpp panel/eiii_panel_adapter.cpp
	${mame_src})
target_include_directories(eiii_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
	PRIVATE standalone/shim ${MAME} ${MAME}/src/lib ${MAME}/src/emu ${MAME}/src/osd ${MAME}/src/lib/util ${MAME}/src/devices
	${MAME}/src/devices/cpu/ns32000 ${MAME}/3rdparty)
target_compile_options(eiii_runtime PRIVATE ${EIII_WARN})
find_package(Threads REQUIRED)
target_link_libraries(eiii_runtime PUBLIC eiii_softfloat3 Threads::Threads)

# ---- UI: software-rendered rack panel, panel fonts, native window
set(fonts_cpp ${CMAKE_CURRENT_BINARY_DIR}/generated/eiii_fonts.cpp)
file(GLOB font_files third_party/fonts/*.ttf)
add_custom_command(OUTPUT ${fonts_cpp}
	COMMAND ${CMAKE_COMMAND} -DFONT_DIR=${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts -DOUT=${fonts_cpp}
	        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_fonts.cmake
	DEPENDS cmake/embed_fonts.cmake ${font_files}
	COMMENT "Embedding panel fonts")
add_library(eiii_ui STATIC ui/raster.cpp ui/panel_view.cpp ui/panel_menu.cpp ui/panel_window.cpp ui/x11_window.cpp ${fonts_cpp})
target_compile_options(eiii_ui PRIVATE ${EIII_WARN})
find_package(X11 REQUIRED)
if(NOT X11_Xext_FOUND)
	message(FATAL_ERROR "libXext (development files) not found")
endif()
target_link_libraries(eiii_ui PUBLIC eiii_runtime X11::X11 X11::Xext)

# ---- the plugin: CLAP SDK (vendored), the plugin code as a static library, clap_entry in its own file
add_library(clap INTERFACE)
target_include_directories(clap INTERFACE third_party/clap/include)
add_library(eiii_rack_impl STATIC plugin/eiii_clap.cpp)
target_compile_options(eiii_rack_impl PRIVATE ${EIII_WARN})
target_link_libraries(eiii_rack_impl PUBLIC clap eiii_ui)

set(plugin_dir ${CMAKE_CURRENT_BINARY_DIR}/plugins)
if(EIII_VST3)
	include(FetchContent)
	# patch: hosts may pass fewer audio buses than declared (Qtractor passes no Sample In); v0.16.0 read past them
	find_package(Git REQUIRED)
	FetchContent_Declare(clap-wrapper GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
		GIT_TAG ${EIII_CLAP_WRAPPER_TAG} GIT_SHALLOW TRUE UPDATE_DISCONNECTED TRUE
		PATCH_COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clap-wrapper-missing-buses.patch)
	set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES TRUE CACHE BOOL "")
	FetchContent_MakeAvailable(clap-wrapper)
	# VST3 class ID: clap-wrapper derives it from the CLAP id (SHA-1 name UUID of "org.eiii-core.eiii-rack"), so it is
	# fixed as long as that id never changes; saved projects find the plugin by it.
	make_clapfirst_plugins(
		TARGET_NAME eiii-rack
		IMPL_TARGET eiii_rack_impl
		OUTPUT_NAME "EIII Rack"
		ENTRY_SOURCE plugin/eiii_clap_entry.cpp
		BUNDLE_IDENTIFIER org.eiii-core.eiii-rack
		BUNDLE_VERSION ${PROJECT_VERSION}
		COPY_AFTER_BUILD FALSE
		PLUGIN_FORMATS CLAP VST3
		ASSET_OUTPUT_DIRECTORY ${plugin_dir})
else()
	add_library(eiii-rack_clap MODULE plugin/eiii_clap_entry.cpp)
	target_link_libraries(eiii-rack_clap PRIVATE eiii_rack_impl)
	set_target_properties(eiii-rack_clap PROPERTIES OUTPUT_NAME "EIII Rack" PREFIX "" SUFFIX ".clap"
		LIBRARY_OUTPUT_DIRECTORY ${plugin_dir})
	add_custom_target(eiii-rack_all DEPENDS eiii-rack_clap)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	# report missing symbols at link time, not when a host loads the plugin
	foreach(t eiii-rack_clap eiii-rack_vst3)
		if(TARGET ${t})
			target_link_options(${t} PRIVATE -Wl,-z,defs)
		endif()
	endforeach()
	# the CLAP exports clap_entry only, as with `make plugin`.  The VST3 keeps clap-wrapper's default exports: hiding
	# the libstdc++ GNU-unique symbols it carries lets hosts unload the module, and the Steinberg validator then crashes
	# at exit.
	target_link_options(eiii-rack_clap PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/plugin/exports.map)
	set_property(TARGET eiii-rack_clap APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/plugin/exports.map)
endif()

# tools/vst3_host_test: loads the VST3 and drives it the way a DAW does (cmake --build build --target vst3_host_test)
if(EIII_VST3 AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
	set(vst3_sdk ${VST3_SDK_ROOT})
	if(NOT vst3_sdk)
		set(vst3_sdk ${CMAKE_BINARY_DIR}/cpm/vst3sdk)
	endif()
	set(hosting ${vst3_sdk}/public.sdk/source/vst/hosting)
	add_executable(vst3_host_test EXCLUDE_FROM_ALL tools/vst3_host_test.cpp
		${hosting}/module.cpp ${hosting}/module_linux.cpp ${hosting}/hostclasses.cpp ${hosting}/eventlist.cpp
		${hosting}/parameterchanges.cpp ${hosting}/pluginterfacesupport.cpp ${vst3_sdk}/public.sdk/source/vst/utility/stringconvert.cpp)
	target_link_libraries(vst3_host_test PRIVATE base-sdk-vst3 ${CMAKE_DL_LIBS} Threads::Threads)
	add_dependencies(vst3_host_test eiii-rack_vst3)
endif()
