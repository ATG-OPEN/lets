#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 ATG
#
# xc16-install.sh: makes an XC16 compiler a LETS compiler, so a LETS program includes only standard
# headers (<fcntl.h>, <poll.h>, <sys/stat.h>, <dirent.h>, ...) and <lets-app.h>. docs/xc16-setup.md.
#
#   tools/xc16-install.sh [VERSION]              install (or update) into XC16 VERSION (e.g. 2.10;
#                                                default: the newest installed)
#   tools/xc16-install.sh --check [VERSION]      exit 0 when the compiler has this LETS's headers
#   tools/xc16-install.sh --uninstall [VERSION]  remove what was installed, restore the patched files
#   XC16_DIR=/path/to/xc16/v2.10 tools/xc16-install.sh ...   another place
#
# Into XC16's include/ it copies (new files, listed in include/lets/INSTALLED):
#   fcntl.h poll.h dirent.h sys/stat.h sys/time.h sys/timex.h sys/mount.h sys/ioctl.h sys/statvfs.h
#   sys/types.h sys/reboot.h lets/libc.h lets/posix.h lets-app.h            (from this repository's include/)
# and appends `#include <lets/posix.h>` to XC16's own unistd.h, time.h and signal.h (between marker
# lines; the original is kept as FILE.lets-orig). Nothing else of the compiler is changed: no .gld
# (LETS needs nothing of them, docs/xc16-setup.md). Running it again changes only what differs.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
SRC=$HERE/include
NEW="fcntl.h poll.h dirent.h sys/stat.h sys/time.h sys/timex.h sys/mount.h sys/ioctl.h sys/statvfs.h sys/types.h sys/reboot.h lets/libc.h lets/posix.h lets/task.h lets/node.h lets/lines.h lets/args.h lets-app.h"
PATCH="unistd.h time.h signal.h"
BEGIN='/* LETS (lets/tools/xc16-install.sh): the POSIX calls LETS adds - begin */'
END='/* LETS (lets/tools/xc16-install.sh) - end */'

mode=install
case "${1:-}" in --check) mode=check; shift ;; --uninstall) mode=uninstall; shift ;; -h|--help) sed -n '5,19p' "$0"; exit 0 ;; esac
VER=${1:-}; VER=${VER#v}

find_xc16() {
	[ -n "${XC16_DIR:-}" ] && { echo "$XC16_DIR"; return; }
	local d
	for base in /Applications/microchip/xc16 /opt/microchip/xc16; do
		if [ -n "$VER" ]; then
			[ -d "$base/v$VER" ] && { echo "$base/v$VER"; return; }
		else
			d=$(ls -d "$base"/v* 2>/dev/null | sort -V | tail -1)
			[ -n "$d" ] && { echo "$d"; return; }
		fi
	done
}
XC=$(find_xc16)
[ -n "$XC" ] && [ -d "$XC/include" ] || { echo "xc16-install: no XC16 ${VER:+v$VER }found (set XC16_DIR=)"; exit 1; }
INC=$XC/include
echo "xc16-install: $mode, $XC"

need_write() {
	if [ ! -w "$INC" ]; then
		echo "xc16-install: $INC is not writable; run: sudo $0 $([ $mode = uninstall ] && echo --uninstall) ${VER:+$VER}"
		exit 2
	fi
}

patched() { grep -qF "$BEGIN" "$1" 2>/dev/null; }

case $mode in
check)
	bad=0
	for f in $NEW; do
		if ! cmp -s "$SRC/$f" "$INC/$f"; then echo "  differs or missing: include/$f"; bad=1; fi
	done
	for f in $PATCH; do patched "$INC/$f" || { echo "  not patched: include/$f"; bad=1; }; done
	[ $bad = 0 ] && echo "xc16-install: up to date" || echo "xc16-install: run tools/xc16-install.sh ${VER}"
	exit $bad
	;;
install)
	need_write
	for f in $NEW; do
		if cmp -s "$SRC/$f" "$INC/$f"; then continue; fi
		if [ -e "$INC/$f" ] && ! grep -qx "$f" "$INC/lets/INSTALLED" 2>/dev/null; then
			[ -e "$INC/$f.lets-orig" ] || { cp -p "$INC/$f" "$INC/$f.lets-orig"; echo "  kept the compiler's include/$f as $f.lets-orig"; }
		fi
		mkdir -p "$(dirname "$INC/$f")"
		cp "$SRC/$f" "$INC/$f"
		echo "  copied include/$f"
	done
	mkdir -p "$INC/lets"
	printf '%s\n' $NEW > "$INC/lets/INSTALLED"
	for f in $PATCH; do
		if patched "$INC/$f"; then continue; fi
		[ -e "$INC/$f.lets-orig" ] || cp -p "$INC/$f" "$INC/$f.lets-orig"
		# a new file in its place (the compiler's files may be read-only; the directory is what we write)
		{ cat "$INC/$f.lets-orig"; printf '\n%s\n#include <lets/posix.h>\n%s\n' "$BEGIN" "$END"; } > "$INC/$f.lets-new"
		mv -f "$INC/$f.lets-new" "$INC/$f"
		echo "  patched include/$f (original: $f.lets-orig)"
	done
	echo "xc16-install: done"
	;;
uninstall)
	need_write
	for f in $PATCH; do
		if [ -e "$INC/$f.lets-orig" ]; then mv -f "$INC/$f.lets-orig" "$INC/$f"; echo "  restored include/$f"; fi
	done
	for f in $NEW; do
		if [ -e "$INC/$f.lets-orig" ]; then mv -f "$INC/$f.lets-orig" "$INC/$f"; echo "  restored include/$f"
		elif [ -e "$INC/$f" ]; then rm "$INC/$f"; echo "  removed include/$f"; fi
	done
	rm -f "$INC/lets/INSTALLED"
	rmdir "$INC/lets" "$INC/sys" 2>/dev/null || true
	echo "xc16-install: removed"
	;;
esac
