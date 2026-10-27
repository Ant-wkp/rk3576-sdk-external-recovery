#!/bin/sh
# BOARD TEST ONLY. Caller separately activates a known healthy target.
set -eu
target=$1 mode=$2 tag=$3
case "$target" in a|b) ;; *) exit 2;; esac
case "$mode" in identity|hang|reboot-fails|metadata-crc) ;; *) exit 2;; esac
case "$tag" in *[!a-z0-9-]*|'') exit 2;; esac
base=/userdata/native-boot-check-fixtures
mkdir -p "$base"
test ! -e "$base/pending"
test ! -e "$base/$tag.log"
test -s "$base/board_fault.so"
test -s "$base/oneshot-bootcontrol.sh"
current=$(tr ' ' '\n' < /proc/cmdline | sed -n 's/^android_slotsufix=_//p')
case "$current" in a|b) ;; *) exit 2;; esac
mountpoint=/
if test "$target" != "$current"; then
    mountpoint=/mnt/native-boot-check-fixture
    mkdir -p "$mountpoint"
    mount "/dev/block/by-name/system_$target" "$mountpoint"
    trap 'umount /mnt/native-boot-check-fixture' EXIT
fi
script=$mountpoint/etc/init.d/S99_bootcontrol
test -f "$script"
grep -q 'start-stop-daemon' "$script"
if test -e "$base/S99_bootcontrol.original"; then
    cmp "$script" "$base/S99_bootcontrol.original"
else
    cp "$script" "$base/S99_bootcontrol.original"
fi
printf '%s %s\n' "$tag" "$mode" > "$base/pending"
cp "$base/oneshot-bootcontrol.sh" "$script"
chmod 755 "$script"
sync
echo "FAULT_STAGED target=$target tag=$tag mode=$mode"
