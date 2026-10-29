#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Najieb Haswell
#
# Build Debian packages:
#   cligen_<v>_<arch>.deb         CLIgen, from the upstream tag (github.com/clicon/cligen)
#   clixon_<v>_<arch>.deb         Clixon, from the upstream tag (github.com/clicon/clixon)
#   clixon-vppcfg_<v>_<arch>.deb  this project
# All three use the same layout as `make install` (/usr/local), like the upstream Debian packaging
# of CLIgen/Clixon, so they replace a source installation in place.
#
# Usage: packaging/build-debs.sh [--skip-clixon] [OUTDIR]     (default OUTDIR: ./dist)
# Needs: build-essential flex bison libssl-dev libnghttp2-dev git dpkg-dev
set -euo pipefail

CLIXON_VERSION=7.9.0
CLIXON_REV=1
TOP=$(cd "$(dirname "$0")/.." && pwd)
SKIP_CLIXON=0
if [ "${1:-}" = --skip-clixon ]; then
    SKIP_CLIXON=1
    shift
fi
OUT=$(mkdir -p "${1:-$TOP/dist}" && cd "${1:-$TOP/dist}" && pwd)
ARCH=$(dpkg --print-architecture)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
JOBS=$(nproc)

# Version: VERSION file plus the commit count, so every commit sorts higher
VERSION="$(cat "$TOP/VERSION")"
if git -C "$TOP" rev-parse --git-dir >/dev/null 2>&1; then
    VERSION="$VERSION+git$(git -C "$TOP" rev-list --count HEAD).$(git -C "$TOP" rev-parse --short HEAD)"
fi

# Depends: line for the ELF files in a staged tree, from their direct (NEEDED) libraries. Libraries
# inside the given staged trees are skipped (they come from our own packages); the others are
# mapped to installed Debian packages.
shlib_depends() {
    local root=$1
    shift
    local libs pkg ver deps=""
    libs=$(find "$root" -type f \( -name '*.so*' -o -perm -u+x \) | while read -r f; do
        file -b "$f" | grep -q ELF || continue
        for n in $(readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
            ldd "$f" 2>/dev/null | awk -v n="$n" '$1 == n && $3 ~ /^\// {print $1, $3}'
        done
    done | sort -u)
    while read -r name path; do
        [ -n "$name" ] || continue
        for staged in "$root" "$@"; do
            if [ -e "$staged/usr/local/lib/$name" ]; then continue 2; fi
        done
        pkg=$(dpkg -S "$(readlink -f "$path")" 2>/dev/null | head -1 | cut -d: -f1) || true
        [ -n "$pkg" ] || pkg=$(dpkg -S "$path" 2>/dev/null | head -1 | cut -d: -f1) || true
        [ -n "$pkg" ] || continue
        ver=$(dpkg-query -W -f='${Version}' "$pkg" | sed 's/-[^-]*$//')
        case ",$deps," in *",$pkg (>= $ver),"*) ;; *) deps="$deps,$pkg (>= $ver)" ;; esac
    done <<<"$libs"
    echo "${deps#,}" | sed 's/,/, /g'
}

# Build a .deb from a staged tree: build_deb <root> <name> <version> <depends> <description> [extra control]
build_deb() {
    local root=$1 name=$2 ver=$3 depends=$4 desc=$5 extra=${6:-}
    mkdir -p "$root/DEBIAN"
    (cd "$root" && find . -path ./DEBIAN -prune -o \( -path './etc/*' -o -path './usr/local/etc/*' \) -type f -print |
        sed 's|^\.||') \
        >"$root/DEBIAN/conffiles"
    [ -s "$root/DEBIAN/conffiles" ] || rm -f "$root/DEBIAN/conffiles"
    {
        echo "Package: $name"
        echo "Version: $ver"
        echo "Architecture: $ARCH"
        echo "Maintainer: Najieb Haswell <25635833+najiebhaswell@users.noreply.github.com>"
        echo "Installed-Size: $(du -sk --exclude=DEBIAN "$root" | cut -f1)"
        [ -z "$depends" ] || echo "Depends: $depends"
        [ -z "$extra" ] || printf '%s\n' "$extra"
        echo "Section: net"
        echo "Priority: optional"
        printf 'Description: %s\n' "$desc"
    } >"$root/DEBIAN/control"
    find "$root" -path "$root/DEBIAN" -prune -o -type d -exec chmod 755 {} +
    dpkg-deb --root-owner-group -Zxz --build "$root" "$OUT/${name}_${ver}_${ARCH}.deb" >/dev/null
    echo "built $OUT/${name}_${ver}_${ARCH}.deb"
}

CLIGEN_ROOT=$WORK/cligen-root
CLIXON_ROOT=$WORK/clixon-root
if [ $SKIP_CLIXON = 0 ]; then
    # --- CLIgen
    git clone -q --depth 1 -b "$CLIXON_VERSION" https://github.com/clicon/cligen.git "$WORK/cligen"
    (cd "$WORK/cligen" && ./configure -q && make -s -j"$JOBS" && make -s install DESTDIR="$CLIGEN_ROOT")
    build_deb "$CLIGEN_ROOT" cligen "$CLIXON_VERSION-$CLIXON_REV" "$(shlib_depends "$CLIGEN_ROOT")" \
        "CLIgen command-line interface generator (library and headers)
 CLIgen builds interactive command-line interfaces from a syntax specification.
 Upstream: https://github.com/clicon/cligen"

    # --- Clixon, built against the staged CLIgen
    git clone -q --depth 1 -b "$CLIXON_VERSION" https://github.com/clicon/clixon.git "$WORK/clixon"
    (cd "$WORK/clixon" &&
        ./configure -q --with-restconf=native \
            CPPFLAGS="-I$CLIGEN_ROOT/usr/local/include" LDFLAGS="-L$CLIGEN_ROOT/usr/local/lib" &&
        LD_LIBRARY_PATH="$CLIGEN_ROOT/usr/local/lib" make -s -j"$JOBS" &&
        make -s install DESTDIR="$CLIXON_ROOT")
    build_deb "$CLIXON_ROOT" clixon "$CLIXON_VERSION-$CLIXON_REV" \
        "cligen (= $CLIXON_VERSION-$CLIXON_REV), $(shlib_depends "$CLIXON_ROOT" "$CLIGEN_ROOT")" \
        "YANG-based configuration manager (NETCONF, RESTCONF, CLI)
 Clixon backend, CLI, NETCONF and RESTCONF daemons, libraries, headers and YANG models.
 Upstream: https://github.com/clicon/clixon"
    INC="-isystem $CLIGEN_ROOT/usr/local/include -isystem $CLIXON_ROOT/usr/local/include"
    LIB="-L$CLIGEN_ROOT/usr/local/lib -L$CLIXON_ROOT/usr/local/lib"
else
    INC=""
    LIB=""
fi

# --- clixon-vppcfg
ROOT=$WORK/clixon-vppcfg-root
rm -rf "$TOP/build"
make -s -C "$TOP" CFLAGS="-O2 -g -Wall -Wextra -Wno-unused-parameter $INC" LDFLAGS="$LIB"
make -s -C "$TOP" install DESTDIR="$ROOT" UNITDIR=/usr/lib/systemd/system
install -D -m 644 "$TOP/README.md" "$ROOT/usr/local/share/doc/clixon-vppcfg/README.md"
install -m 644 "$TOP/LICENSE" "$TOP/NOTICE" "$ROOT/usr/local/share/doc/clixon-vppcfg/"
mkdir -p "$ROOT/DEBIAN"
install -m 755 "$TOP/packaging/clixon-vppcfg/postinst" "$TOP/packaging/clixon-vppcfg/prerm" \
    "$TOP/packaging/clixon-vppcfg/postrm" "$ROOT/DEBIAN/"
build_deb "$ROOT" clixon-vppcfg "$VERSION" \
    "clixon (>= $CLIXON_VERSION), cligen (>= $CLIXON_VERSION), $(shlib_depends "$ROOT" "$CLIGEN_ROOT" "$CLIXON_ROOT"), python3, python3-yaml, sudo, adduser, iproute2, util-linux, iputils-ping, traceroute, mtr-tiny | mtr, bind9-dnsutils, bird3 | bird2" \
    "Router CLI (Cisco and Huawei VRP style) for VPP and BIRD on Clixon
 Clixon backend and CLI plugins, YANG models and helpers that configure VPP interfaces
 through vppcfg and BGP through BIRD, with candidate/commit, confirmed commit and
 two CLIs: rtr (Cisco style) and vrp (Huawei VRP style). Needs vppcfg in
 /usr/local/sbin/vppcfg (https://github.com/pimvanpelt/vppcfg)." \
    "Recommends: vpp, vpp-plugin-core
Homepage: https://github.com/najiebhaswell/clixon-vppcfg"
rm -rf "$TOP/build"
