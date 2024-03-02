#!/bin/sh
# Copyright 2024 Simon McVittie
# SPDX-License-Identifier: LGPL-2.1-or-later

# Reproducer for <https://bugs.debian.org/1065022>.
# Needs to be run as root in an expendable amd64 chroot, container or VM
# that initially has apt sources that can install some version of
# libglib2.0-0, for example:
# podman run --rm -it -v $(pwd):$(pwd):ro -w $(pwd) debian:bookworm-slim debian/tests/manual/1065022.sh
# podman run --rm -it -v $(pwd):$(pwd):ro -w $(pwd) debian:sid-20240110-slim debian/tests/manual/1065022.sh

set -eux

export DEBIAN_FRONTEND=noninteractive
n=0
failed=0
this_tuple=x86_64-linux-gnu
other_arch=i386
other_tuple=i386-linux-gnu

assert () {
    n=$(( n + 1 ))

    if "$@"; then
        echo "ok $n - $*"
    else
        echo "not ok $n - $* exit status $?"
        failed=1
    fi
}

# Preconditions: install libglib2.0-0, libglib2.0-0t64, at least one
# GSettings schema and at least one GIO module.
dpkg --add-architecture "$other_arch"
apt-get -y update
apt-get -y install libglib2.0-0 "libglib2.0-0:$other_arch"
apt-get -y install gsettings-desktop-schemas
apt-get -y install dconf-gsettings-backend "dconf-gsettings-backend:$other_arch"
test -e /usr/share/glib-2.0/schemas/org.gnome.desktop.interface.gschema.xml
test -s /usr/share/glib-2.0/schemas/gschemas.compiled

for tuple in "$this_tuple" "$other_tuple"; do
    f="/usr/lib/$tuple/gio/modules/libdconfsettings.so"
    test -e "$f"
    test -s "$f"
done

for tuple in "$this_tuple" "$other_tuple"; do
    f="/usr/lib/$tuple/gio/modules/giomodule.cache"
    test -e "$f"
    test -s "$f"
done

# Remove but do not purge the other architecture's packages
apt-get -y remove "libglib2.0-0:$other_arch" "dconf-gsettings-backend:$other_arch"
apt-get -y autoremove

# Upgrade to current unstable, with libglib2.0-0t64
cat > /etc/apt/sources.list.d/debian.sources <<EOF
Types: deb
URIs: http://deb.debian.org/debian
Suites: sid
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
EOF

# Add a deb822-formatted apt source at this location if you are testing a
# locally-built glib2.0 before upload
if [ -e "debian/tests/manual/local-1065022.sources" ]; then
    install -m644 "debian/tests/manual/local-1065022.sources" /etc/apt/sources.list.d/
fi

# Reproducer (1): Upgrade to libglib2.0-0t64. This runs the postrm from
# libglib2.0-0, which deletes necessary files.
apt-get -y update
apt-get -y install --purge libglib2.0-0t64

assert test -e /usr/share/glib-2.0/schemas/org.gnome.desktop.interface.gschema.xml
assert test -s /usr/share/glib-2.0/schemas/gschemas.compiled

assert test -e "/usr/lib/$this_tuple/gio/modules/libdconfsettings.so"
assert test -s "/usr/lib/$this_tuple/gio/modules/libdconfsettings.so"

assert test -e "/usr/lib/$this_tuple/gio/modules/giomodule.cache"
assert test -s "/usr/lib/$this_tuple/gio/modules/giomodule.cache"

# Workaround: Trigger the postinst of libglib2.0-0t64, which will regenerate
# the generated files.
apt-get -y install --reinstall libglib2.0-0t64

assert test -e /usr/share/glib-2.0/schemas/org.gnome.desktop.interface.gschema.xml
assert test -s /usr/share/glib-2.0/schemas/gschemas.compiled

assert test -e "/usr/lib/$this_tuple/gio/modules/libdconfsettings.so"
assert test -s "/usr/lib/$this_tuple/gio/modules/libdconfsettings.so"

assert test -e "/usr/lib/$this_tuple/gio/modules/giomodule.cache"
assert test -s "/usr/lib/$this_tuple/gio/modules/giomodule.cache"

# Reproducer (2): Purge the other multiarch instance of libglib2.0-0.
# Again this runs the postrm from libglib2.0-0, which deletes necessary files.
dpkg --purge "libglib2.0-0:$other_arch"

assert test -e /usr/share/glib-2.0/schemas/org.gnome.desktop.interface.gschema.xml
assert test -s /usr/share/glib-2.0/schemas/gschemas.compiled

assert test -e "/usr/lib/$this_tuple/gio/modules/libdconfsettings.so"
assert test -s "/usr/lib/$this_tuple/gio/modules/libdconfsettings.so"

assert test -e "/usr/lib/$this_tuple/gio/modules/giomodule.cache"
assert test -s "/usr/lib/$this_tuple/gio/modules/giomodule.cache"

echo "1..$n"
exit "$failed"

# vim:set sw=4 sts=4 et:
