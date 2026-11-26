# xbased on macOS x86-64: the segment that holds the guest's address space
# (cmake/macos.cmake). One page here, at 0x10000; tools/macos/reserve_guest.py
# widens it after the link. core/vm.c maps over it.
.globl _nc_macos_guest_space
.section __XBGUEST,__space,regular
_nc_macos_guest_space:
.byte 0
