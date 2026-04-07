#!/bin/sh
#
# Don't call this script. It is used internally by the Meson
# build system. Thank you for your cooperation.
#

set -e

bindir="$2"
sbindir="$1"
prefix="${MESON_INSTALL_DESTDIR_PREFIX}"

mkdir -p "${prefix}/${sbindir}"

sshfs_target="$(python3 -c 'import os, sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))' \
   "${prefix}/${bindir}/sshfs" "${prefix}/${sbindir}")"

ln -svf "${sshfs_target}" "${prefix}/${sbindir}/mount.sshfs"

ln -svf "${sshfs_target}" "${prefix}/${sbindir}/mount.fuse.sshfs"
