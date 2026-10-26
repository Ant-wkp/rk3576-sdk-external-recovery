# Native updateEngine boot checking

## Purpose and installation

`updateEngine --boot-check` is a finite boot-time operation, not an updater
invocation or a permanent watchdog daemon. `S99_bootcontrol` starts it in the
background. Buildroot must enable Recovery, No UI, updateEngine,
`SUCCESSFUL_BOOT`, and `BOOTCONTROL`, without the Recovery executable.

The board overlay generates `/etc/rk-ota-release` from matching U-Boot and
kernel build artifacts. Its six fields are `release`, `kernel`, `uboot`, `dtb`,
`layout`, and `spl_abi`. The file is data, never shell-sourced. The current
board integration uses the existing `100ask,*` chosen properties and
`/etc/ab-test-version`. These checks establish coherence, not cryptographic
authenticity. They do not hash the mutable mounted ext4 against its release image.

Do not install the old `S99ab-watchdog` or provision a separate feeder in userdata.
The BSP watchdog must permit stopping (`NOWAYOUT=n`) and have a working reset.
This integration targets the existing eMMC A/B layout, not MTD or network roots.

## Runtime policy

The checker owns `/dev/watchdog0` and the same nonblocking process lock as A/B
updates, activation, and manual confirmation. New updates require a confirmed
current slot. Never combine `--boot-check` with another CLI option.

The checker validates slot/root identity, exact release metadata, root ext4 rw,
userdata mount/read/write/fsync, A/B metadata, and successful watchdog adoption.
The overall limit is 120 monotonic seconds; sample interval is five seconds;
30 continuous healthy seconds are required. There are no business probes.

Healthy boots are confirmed using the native function and reread, then the
watchdog is explicitly disabled. The kernel stop callback verifies hardware EN
cleared. Only then does the process exit. The watchdog provides **no runtime
hang protection after this point**. Already-confirmed slots need no metadata
rewrite. A current last trial with zero remaining attempts may still confirm.

An explicit new-slot failure disables only that unconfirmed slot, preserving
the confirmed fallback and `last_boot`, durably verifies the sector, then
immediately requests normal `/sbin/reboot`. It does not wait for seven failures,
and does not call `setSlotActivity()`, which would clear fallback confirmation.
An already-confirmed slot failure requests reboot without demotion.

Watchdog reset is only the fallback for a hung checker, failed reboot (15-second
grace), unknown actual root, invalid metadata, absent confirmed fallback, or
uncertain persistence. Such failures never report successful rollback. The
error path never disables the watchdog or sends magic close. An uncertain
confirmation is not blindly replayed. A stop error after confirmation is logged;
normal reboot is requested without demotion and feeding lasts at most another
15 seconds. This request is necessary because a reset-deassert error can leave
the hardware stopped even though the stop ioctl failed; watchdog fallback alone
cannot guarantee recovery in that state. A hardware reset is awaited if normal
reboot fails and the watchdog remains running.

## Diagnostics and testing

`AB_CHECK` messages identify each outcome. `/run/updateEngine-boot-check.state`
records this boot ID and result. A `complete` record prevents restarting the
watchdog by duplicate successful invocations in the same boot. A single bounded
last-result is saved to `/userdata/.updateEngine-boot-result` only after its
mount/write checks pass. State files are diagnostic, not an alternative to
on-disk metadata verification.

`--misc=now` remains an explicit administrative bypass of automatic health
checking, with corrected metadata handling. Do not put it back into init.

Run `python3 tests/boot_check/run.py` for real-source metadata/state/CLI tests.
Run `python3 tests/boot_check/test_integration.py` from the SDK for actual BSP
stop-function and build-identity tests. Device I/O and reboot substitutes exist
only at test link time; production has no fault-injection or simulation option.

Physical acceptance must separately prove immediate fallback and watchdog
stopping for longer than the actual hardware period. Host tests are not board
acceptance. This feature does not certify native delta transactions, arbitrary
power-loss safety, signatures, MTD, or old standalone OTA behavior.
