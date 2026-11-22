# xbased native core (QEMU-free): its own x86 translator + reimplemented Xbox
# kernel, for Windows x64 and Linux x86-64.
#
# xbased_native is the core library, the xbased executable runs it
# (cmake/app_win.cmake).
cmake_minimum_required(VERSION 3.20)
# macOS: the oldest system the build runs on, MoltenVK's own (before
# project(), which sets the compilers up with it)
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "the oldest macOS the build runs on" FORCE)
endif()
project(xbased_native C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ThinLTO for Release builds: the decoder and the renderer ~2% lighter. The
# cache keeps relinking after a change to the files changed.
option(XBASED_LTO "ThinLTO in Release builds" ON)
# The dev tools' control of a run (app/win_devctl.c, hw/xbox/hle/vk/devcap.h;
# a local tool drives it): off, and never in a release.
option(XBASED_DEVCTL "the dev tools' control of a run (not for releases)" OFF)
if(XBASED_DEVCTL)
  add_compile_definitions(XBASED_DEVCTL=1)
endif()
if(XBASED_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT nc_ipo LANGUAGES C CXX)
  if(nc_ipo)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    if(APPLE)
      add_link_options("-Wl,-cache_path_lto,${CMAKE_BINARY_DIR}/lto-cache")
    else()
      add_link_options("-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/lto-cache")
    endif()
  endif()
endif()

get_filename_component(NC_REPO "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
# Linux on x86-64: the same translator as Windows (cmake/linux.cmake)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(NC_LINUX_X64 ON)
endif()
# macOS on Intel: the same translator and POSIX code as Linux
# (cmake/macos.cmake); NC_POSIX_X64 is either of the two. Built on an Intel
# Mac, or for one on Apple silicon (CMAKE_OSX_ARCHITECTURES=x86_64): the
# translator emits x86-64 code, which an arm64 build can't run.
if(APPLE AND (CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64" OR
              (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")))
  set(NC_MACOS_X64 ON)
elseif(APPLE)
  message(FATAL_ERROR "xbased on macOS is for Intel Macs (x86-64); on Apple silicon, configure with "
                      "-DCMAKE_OSX_ARCHITECTURES=x86_64")
endif()
if(NC_LINUX_X64 OR NC_MACOS_X64)
  set(NC_POSIX_X64 ON)
endif()
if(NC_LINUX_X64)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/linux.cmake")
elseif(NC_MACOS_X64)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos.cmake")
elseif(WIN32)
  # Windows x64: the guest CPU is the core's own x86-32 -> x86-64 translator
  # (cpu/cpu_x86.cpp, on Zydis); cmake/win.cmake.
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/win.cmake")
else()
  message(FATAL_ERROR "xbased builds for Windows x64 and Linux x86-64")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/hle.cmake")

# libslirp (NC_DEPS/libslirp, 4.7.0, unmodified): the NAT backend's TCP/IP
# stack (net/nat.c), on the GLib subset plus net/slirp/glib.h. Its public
# header is copied beside the generated version header, so the core's
# include path gets only those two.
set(NC_SLIRP "${NC_DEPS}/libslirp/src")
set(SLIRP_MAJOR_VERSION 4)
set(SLIRP_MINOR_VERSION 7)
set(SLIRP_MICRO_VERSION 0)
set(SLIRP_VERSION_STRING "\"4.7.0\"")
configure_file("${NC_SLIRP}/libslirp-version.h.in" "${CMAKE_BINARY_DIR}/slirp/libslirp-version.h" @ONLY)
if(WIN32)
  # linked statically: no dllimport/dllexport on its functions
  file(READ "${NC_SLIRP}/libslirp.h" nc_slirp_h)
  string(REPLACE "__declspec(dllexport)" "" nc_slirp_h "${nc_slirp_h}")
  string(REPLACE "__declspec(dllimport)" "" nc_slirp_h "${nc_slirp_h}")
  file(WRITE "${CMAKE_BINARY_DIR}/slirp/libslirp.h" "${nc_slirp_h}")
else()
  configure_file("${NC_SLIRP}/libslirp.h" "${CMAKE_BINARY_DIR}/slirp/libslirp.h" COPYONLY)
endif()
add_library(nc_slirp STATIC
  net/slirp/glib.c
  "${NC_SLIRP}/arp_table.c" "${NC_SLIRP}/bootp.c" "${NC_SLIRP}/cksum.c" "${NC_SLIRP}/dhcpv6.c"
  "${NC_SLIRP}/dnssearch.c" "${NC_SLIRP}/if.c" "${NC_SLIRP}/ip6_icmp.c" "${NC_SLIRP}/ip6_input.c"
  "${NC_SLIRP}/ip6_output.c" "${NC_SLIRP}/ip_icmp.c" "${NC_SLIRP}/ip_input.c" "${NC_SLIRP}/ip_output.c"
  "${NC_SLIRP}/mbuf.c" "${NC_SLIRP}/misc.c" "${NC_SLIRP}/ncsi.c" "${NC_SLIRP}/ndp_table.c"
  "${NC_SLIRP}/sbuf.c" "${NC_SLIRP}/slirp.c" "${NC_SLIRP}/socket.c" "${NC_SLIRP}/state.c"
  "${NC_SLIRP}/stream.c" "${NC_SLIRP}/tcp_input.c" "${NC_SLIRP}/tcp_output.c" "${NC_SLIRP}/tcp_subr.c"
  "${NC_SLIRP}/tcp_timer.c" "${NC_SLIRP}/tftp.c" "${NC_SLIRP}/udp.c" "${NC_SLIRP}/udp6.c"
  "${NC_SLIRP}/util.c" "${NC_SLIRP}/version.c" "${NC_SLIRP}/vmstate.c")
target_include_directories(nc_slirp BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/net/slirp" "${NC_REPO}")
target_include_directories(nc_slirp PUBLIC "${CMAKE_BINARY_DIR}/slirp")
target_link_libraries(nc_slirp PRIVATE nc_glib)
target_compile_options(nc_slirp PRIVATE -w)
if(NC_POSIX_X64)
  target_compile_options(nc_slirp PRIVATE "SHELL:-include signal.h")   # misc.c's
endif()

if(NC_LINUX_X64)
  set(NC_PLATFORM_SOURCES core/vm.c core/fault_win.c core/jmp_x64.c cpu/cpu_x86.cpp cpu/x64emu.cpp
      compat/linux/seh.c compat/linux/linux.c)
  set(NC_CPU_LIB nc_zydis)
  set(NC_SYS_LIBS pthread dl m)
elseif(NC_MACOS_X64)
  # compat/linux's POSIX layer serves macOS too; macos_reserve.s: the guest's
  # address space kept free from exec on (cmake/macos.cmake)
  set(NC_PLATFORM_SOURCES core/vm.c core/fault_win.c core/jmp_x64.c cpu/cpu_x86.cpp cpu/x64emu.cpp
      compat/linux/seh.c compat/linux/linux.c)
  set(NC_CPU_LIB nc_zydis)
  set(NC_SYS_LIBS pthread m resolv)   # resolv: libslirp's DNS servers
else()
  set(NC_PLATFORM_SOURCES core/vm.c core/vm_win.cpp core/fault_win.c core/jmp_x64.c cpu/cpu_x86.cpp cpu/x64emu.cpp compat/win/win.c)
  set(NC_CPU_LIB nc_zydis)
  set(NC_SYS_LIBS ws2_32 iphlpapi winmm bcrypt dwmapi synchronization pthread)
endif()

add_library(xbased_native STATIC
  ${NC_PLATFORM_SOURCES}
  core/log.c
  core/xdvdfs.c
  core/xbe_load.c
  core/mods.c
  core/options.c
  core/props.c
  core/cheats.c
  core/xcheat.c
  core/memscan.c
  core/core.c
  core/mmio.c
  core/irq.c
  dev/nv2a.c
  dev/ports.c
  dev/usb.c
  dev/usb_hub.c
  dev/xid.c
  dev/communicator.c
  dev/ohci.c
  dev/aci.c
  dev/nvnet.c
  net/net.c
  net/lan.c
  net/kai.c
  net/nat.c
  kernel/kernel.c
  kernel/pool.c
  kernel/ob.c
  kernel/ps.c
  kernel/ke.c
  kernel/mm.c
  kernel/paging.c
  kernel/ex.c
  kernel/rtl.c
  kernel/seh.c
  kernel/fixups.c
  kernel/x86mmio.c
  kernel/timer.c
  kernel/intr.c
  kernel/apc.c
  kernel/io.c
  kernel/xc.c
  kernel/xc_crypto.c
  kernel/kstate.c
  state/state.c
  state/vm_state.c
  state/save.c
  state/cache.c
  "${NC_REPO}/hw/xbox/hle/core/xbe.c"
)
target_include_directories(xbased_native PUBLIC "${NC_REPO}")
# xxhash (single header): the savestates' blob store (state/cache.c)
target_include_directories(xbased_native PRIVATE "${NC_XXHASH_DIR}")
# hw/xbox/hle/core/xbe.c here as in the host tests (hle_pure.h: the C library alone)
target_compile_definitions(xbased_native PRIVATE HLE_HOST_TEST)
target_link_libraries(xbased_native PUBLIC ${NC_CPU_LIB} xbased_hle xbased_apu nc_slirp ${NC_SYS_LIBS} z)
target_compile_options(xbased_native PRIVATE -Wall -Wno-unused-parameter)
target_sources(xbased_native PRIVATE core/chd_image.c core/fold_image.c core/nbd_image.c)

# libchdr (NC_DEPS/libchdr, BSD-3, unmodified): CHD disc images
# (core/chd_image.c), with its own LZMA, Zstandard and FLAC decoders and the
# system's zlib. Without it, CHD files aren't read.
if(EXISTS "${NC_DEPS}/libchdr/CMakeLists.txt")
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(WITH_SYSTEM_ZLIB ON CACHE BOOL "" FORCE)
  set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
  set(INSTALL_STATIC_LIBS OFF CACHE BOOL "" FORCE)
  add_subdirectory("${NC_DEPS}/libchdr" "${CMAKE_BINARY_DIR}/libchdr" EXCLUDE_FROM_ALL)
  target_compile_options(chdr-static PRIVATE -w)
  target_link_libraries(xbased_native PRIVATE chdr-static)
  target_compile_definitions(xbased_native PRIVATE NC_HAVE_CHD)
endif()

target_link_libraries(xbased_hle PUBLIC xbased_native)
target_link_libraries(nc_slirp PRIVATE xbased_native)   # nc_log

# The xbased executable (native/app/win_main.c, cmake/app_win.cmake).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/app_win.cmake")

# Host tests of the PC's own code (not part of the default build):
#   cputest: the translator (cpu/cpu_x86.cpp) against the host processor,
#   32-bit code run in compatibility mode and translated, results compared
#   (tests/native/cpu/cputest.cpp)
if(WIN32)
  add_executable(cputest EXCLUDE_FROM_ALL "${NC_REPO}/tests/native/cpu/cputest.cpp" cpu/cpu_x86.cpp)
  target_include_directories(cputest PRIVATE "${NC_REPO}")
  target_link_libraries(cputest PRIVATE nc_zydis)
  target_compile_options(cputest PRIVATE -Wall -Wno-unused-parameter)
  target_compile_definitions(cputest PRIVATE NC_CPU_DIAG=1)
  nc_win_compat(cputest)
endif()
# The translator's diagnostics in the app itself (NC_SMC_CHECK): off, and
# never in a release; -DXBASED_CPU_DIAG=ON for a build to investigate a title.
option(XBASED_CPU_DIAG "the translator's diagnostics in the app (not for releases)" OFF)
if(XBASED_CPU_DIAG)
  target_compile_definitions(xbased_native PRIVATE NC_CPU_DIAG=1)
endif()
# The statistics a run keeps and logs as it goes (the kernel's and the
# decoder's counts and their lines every few seconds, the stutter monitor,
# the GPU event trace, the debug properties' polls): off, and never in a
# release; -DXBASED_DIAG=ON for a build to investigate a title. What a
# report needs stays in every build: the log, a fault's last kernel calls
# and threads, the frame rate's line.
option(XBASED_DIAG "the run's statistics and traces (not for releases)" OFF)
if(XBASED_DIAG)
  add_compile_definitions(XBASED_DIAG=1)
else()
  add_compile_definitions(XBASED_DIAG=0)
endif()

if(WIN32)
  nc_win_compat(xbased_native xbased_hle xbased_apu nc_glib nc_slirp)
  if(TARGET xbased)
    nc_win_compat(xbased)
  endif()
elseif(NC_POSIX_X64)
  nc_linux_compat(xbased_native xbased_hle xbased_apu nc_glib nc_slirp xbased)
endif()
if(NC_MACOS_X64)
  nc_macos_exe(xbased)
endif()
