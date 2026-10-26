#!/bin/sh
# Debian packages with the shared libraries a program needs (for the runtime image, so the list never falls behind
# the build): packaging/docker/runtime-packages.sh build/schneidi-cli
set -eu
paths=$(ldd "$1" | awk '/=> \//{print $3}' | xargs realpath | sort -u)
# merged /usr: a package may list a file under /usr/lib or /lib, so ask for both (one dpkg call: it is slow).
# Lines "package:arch: /path"; diversion notes and "not found" are left out.
{ dpkg -S $paths; dpkg -S $(echo "$paths" | sed 's|^/usr||'); } 2>/dev/null | grep -E '^[^ ]+: /' | cut -d: -f1 | sort -u
