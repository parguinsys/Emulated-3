#!/bin/sh
# Fetches the ARM64 sysroot used to cross-build the Raspberry Pi (Pi 400 / Pi 4 / Pi 5, 64-bit OS) binaries on an x86-64
# Linux machine: Ubuntu 22.04 (glibc 2.35, libstdc++ 11) arm64 headers and libraries for ALSA and X11, so the result
# runs on Raspberry Pi OS bookworm (glibc 2.36) and anything newer.  Needs debootstrap and root (sudo) once.
#   tools/pi_sysroot.sh [DIR]     (default ~/.cache/eiii/pi-sysroot)
set -e
dir=${1:-$HOME/.cache/eiii/pi-sysroot}
[ -f "$dir/.complete" ] && exit 0
command -v debootstrap >/dev/null || { echo "pi_sysroot: install debootstrap (sudo apt install debootstrap)" >&2; exit 1; }
sudo=; [ "$(id -u)" = 0 ] || sudo=sudo
rm -rf "$dir.tmp"; mkdir -p "$dir.tmp"
$sudo debootstrap --foreign --arch=arm64 --variant=minbase \
	--include=libc6-dev,libasound2-dev,libx11-dev,libxext-dev,libstdc++-11-dev,libgcc-11-dev \
	jammy "$dir.tmp" http://ports.ubuntu.com/ubuntu-ports
# --foreign only unpacks the base system: unpack every downloaded package (headers, static and shared libraries)
for deb in "$dir.tmp"/var/cache/apt/archives/*.deb; do $sudo dpkg -x "$deb" "$dir.tmp"; done
$sudo chown -R "$(id -u):$(id -g)" "$dir.tmp"
rm -rf "$dir"; mv "$dir.tmp" "$dir"; touch "$dir/.complete"
echo "pi_sysroot: $dir"
