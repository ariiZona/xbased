# xbased's core on macOS x86-64 (Intel, Apple clang): the guest CPU is
# cpu/cpu_x86.cpp as on Linux and Windows (Zydis), the POSIX layer is
# compat/linux's (nc_winapi.h, seh.c over SIGSEGV/SIGBUS with macOS's
# ucontext). SDL3 is built here for the APU's output and the front end;
# Vulkan is MoltenVK's, which volk loads at run time.
set(NC_DEPS "${CMAKE_CURRENT_LIST_DIR}/../../../deps" CACHE PATH "native core third-party sources")
enable_language(ASM)

set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)
set(ZYCORE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
add_subdirectory("${NC_DEPS}/zydis" zydis-build EXCLUDE_FROM_ALL)
add_library(nc_zydis INTERFACE)
target_link_libraries(nc_zydis INTERFACE Zydis)

set(NC_XXHASH_DIR "${NC_DEPS}/xxhash")

# The guest's 4 GiB live at host address 0 (core/vm.c): pointer arithmetic on
# that null base is everyday business, no optimization may assume otherwise.
add_compile_options(-fno-delete-null-pointer-checks -msse4.1 -mpopcnt -g1)
add_compile_definitions(_DARWIN_C_SOURCE)
# The core's own targets: compat/linux's headers, macOS's shims in front of
# them (compat/macos/include) and nc_macos_posix.h before every source.
set(NC_LINUX_COMPAT_INC "${CMAKE_CURRENT_LIST_DIR}/../compat/linux/include")
set(NC_MACOS_COMPAT_INC "${CMAKE_CURRENT_LIST_DIR}/../compat/macos/include")
function(nc_linux_compat)
  foreach(t ${ARGN})
    if(TARGET ${t})
      target_include_directories(${t} BEFORE PRIVATE "${NC_MACOS_COMPAT_INC}" "${NC_LINUX_COMPAT_INC}")
      target_compile_options(${t} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-include nc_macos_posix.h>")
    endif()
  endforeach()
endfunction()

# The executable that runs titles (xbased): the guest's address
# space is the kernel's to reserve at exec time, before libSystem's first
# allocations land in it. __PAGEZERO shrinks to 64 KiB, the __XBGUEST
# segment (core/macos_reserve.s) starts at 0x10000 and is widened after the
# link to 0x200000000 (tools/macos/reserve_guest.py), where __TEXT starts:
# not position-independent, so nothing slides into the guest's space.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
function(nc_macos_exe)
  foreach(t ${ARGN})
    if(TARGET ${t})
      target_sources(${t} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/core/macos_reserve.s")
      target_link_options(${t} PRIVATE -Wl,-no_pie -Wl,-pagezero_size,0x10000
                          -Wl,-segaddr,__XBGUEST,0x10000 -Wl,-segaddr,__TEXT,0x200000000
                          -Wl,-w)
      add_custom_command(TARGET ${t} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${NC_REPO}/tools/macos/reserve_guest.py" "$<TARGET_FILE:${t}>"
        VERBATIM)
    endif()
  endforeach()
endfunction()

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${NC_DEPS}/SDL3" sdl3 EXCLUDE_FROM_ALL)
