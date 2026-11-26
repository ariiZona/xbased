#!/usr/bin/env python3
"""xbased on macOS (x86-64): the executable's __XBGUEST segment widened to
the guest's address space, so the kernel reserves it at exec time.

  tools/macos/reserve_guest.py <executable> [end]

The guest's 4 GiB live at host address 0 (native/core/vm.c). libSystem
allocates its first pages (malloc metadata, among others) right above
__PAGEZERO before main runs, so a reservation made at run time comes too
late. The linker gives __XBGUEST (native/core/macos_reserve.s) one page at
0x10000; here its vmsize grows to reach `end` (default 0x200000000: the
guest's 4 GiB and the 4 GiB above them, where __TEXT starts). The page
beyond its file contents are zero-filled, nothing is read from the file.
The executable is signed again (ad hoc) afterwards: the load commands are
part of what the signature covers.
"""
import struct
import subprocess
import sys

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19


def main():
    path = sys.argv[1]
    end = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x200000000
    with open(path, "r+b") as f:
        hdr = f.read(32)
        magic, _, _, _, ncmds, _, _, _ = struct.unpack("<IiiIIIII", hdr)
        if magic != MH_MAGIC_64:
            sys.exit(f"{path}: not a 64-bit Mach-O (thin x86-64 expected)")
        off = 32
        for _ in range(ncmds):
            f.seek(off)
            cmd, size = struct.unpack("<II", f.read(8))
            if cmd == LC_SEGMENT_64:
                f.seek(off + 8)
                name = f.read(16).rstrip(b"\0").decode()
                vmaddr, vmsize = struct.unpack("<QQ", f.read(16))
                if name == "__XBGUEST":
                    if vmaddr >= end:
                        sys.exit(f"{path}: __XBGUEST at {vmaddr:#x}, past {end:#x}")
                    f.seek(off + 32)
                    f.write(struct.pack("<Q", end - vmaddr))
                    break
            off += size
        else:
            sys.exit(f"{path}: no __XBGUEST segment")
    subprocess.run(["codesign", "-s", "-", "-f", path], check=True, capture_output=True)
    print(f"{path}: __XBGUEST {vmaddr:#x}-{end:#x}")


if __name__ == "__main__":
    main()
