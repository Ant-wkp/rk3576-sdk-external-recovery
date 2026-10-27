#!/bin/sh
# BOARD TEST ONLY. Restore production startup before invoking a faulted checker.
case "$1" in
start)
    base=/userdata/native-boot-check-fixtures
    read -r tag mode < "$base/pending"
    case "$tag" in *[!a-z0-9-]*|'') exit 2;; esac
    case "$mode" in identity|hang|reboot-fails|metadata-crc) ;; *) exit 2;; esac
    mv "$base/pending" "$base/$tag.consumed"
    cp "$base/S99_bootcontrol.original" /etc/init.d/.S99_bootcontrol.restore
    chmod 755 /etc/init.d/.S99_bootcontrol.restore
    mv /etc/init.d/.S99_bootcontrol.restore /etc/init.d/S99_bootcontrol
    sync
    echo "BOARD_FIXTURE starting $tag mode=$mode; production startup restored"
    UE_BOARD_TEST_MODE="$mode" UE_BOARD_TEST_TAG="$tag" LD_PRELOAD="$base/board_fault.so" \
        /usr/bin/updateEngine --boot-check > "$base/$tag.log" 2>&1 &
    echo $! > /run/updateEngine-boot-check.pid
    ;;
stop) ;;
*) exit 2 ;;
esac
