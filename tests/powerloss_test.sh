#!/usr/bin/env bash
#
# Power-loss durability test using dm-flakey (Linux, requires root/sudo).
#
# This is a PRIVILEGED MANUAL test — it is NOT part of ctest because it needs
# a loop device, a device-mapper flakey target, a mounted filesystem, and
# drop_caches. It proves the durability claim that a standard ctest cannot:
# that an auto-commit which returned SDB_OK survives a real power loss, because
# its fsync genuinely pushed the WAL frame to stable storage.
#
# How it models power loss:
#   1. Build an ext4 filesystem on a dm-flakey device backed by a loop file,
#      running in "always up" (normal) mode.
#   2. The writer helper auto-commits N encrypted records (each commit fsyncs)
#      and then blocks WITHOUT closing (a clean close would checkpoint+fsync
#      everything and mask per-commit durability).
#   3. Flip dm-flakey to drop_writes: every subsequent write to the device is
#      silently discarded — exactly what a power cut does to writes still in
#      flight / not yet on the platter.
#   4. SIGKILL the writer, drop the page cache (RAM is gone after power loss),
#      lazily unmount (its dirty metadata writes are dropped), then restore the
#      device to normal and remount — ext4 recovers from the backing image,
#      which contains only what was truly persisted before the cut.
#   5. The verifier reopens the DB (WAL recovery) and asserts EVERY acked
#      commit is present with its exact value and verify() is clean. If any
#      acked commit is missing, ShibaDB acked before its data was durable — a
#      real fsync-durability bug.
#
# Usage: sudo bash tests/powerloss_test.sh [N]   (N = commits, default 5000)

set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LIB="${SDB_POWERLOSS_LIBRARY:-$REPO/build-gcc/libshibadb.so}"
LIB_DIR="$(cd "$(dirname "$LIB")" 2>/dev/null && pwd)" \
    || { echo "POWERLOSS TEST: FAIL — library directory not found: $LIB"; exit 1; }
HELPER="/tmp/sdb_powerloss_helper"
WORK="/tmp/sdb_powerloss"
BACKING="$WORK/backing.img"
MNT="$WORK/mnt"
DM="sdb_flakey_pl"
N="${1:-5000}"
LOOP=""

cleanup() {
    set +e
    sudo umount -l "$MNT" 2>/dev/null
    sudo dmsetup remove "$DM" 2>/dev/null
    [ -n "$LOOP" ] && sudo losetup -d "$LOOP" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

fail() { echo "POWERLOSS TEST: FAIL — $1"; exit 1; }

# Compile the helper against the freshly-built shared library.
[ -f "$LIB" ] || fail "shared library not found: $LIB"
gcc -I "$REPO/include" "$REPO/tests/powerloss_helper.c" -o "$HELPER" \
    "$LIB" -Wl,-rpath,"$LIB_DIR" || fail "helper compile"

rm -rf "$WORK"; mkdir -p "$WORK" "$MNT"
fallocate -l 400M "$BACKING" || dd if=/dev/zero of="$BACKING" bs=1M count=400 status=none || fail "backing"
LOOP="$(sudo losetup -f --show "$BACKING")" || fail "losetup"
SECTORS="$(sudo blockdev --getsz "$LOOP")" || fail "getsz"

# Normal mode: up=60s, down=0 -> always up (writes pass through to backing).
sudo dmsetup create "$DM" --table "0 $SECTORS flakey $LOOP 0 60 0" || fail "dmsetup create"
sudo mkfs.ext4 -q -F "/dev/mapper/$DM" || fail "mkfs"
sudo mount "/dev/mapper/$DM" "$MNT" || fail "mount"
sudo chmod 777 "$MNT"

DB="$MNT/pl.sdb"

# Writer: commit N (fsync each), announce READY, block without closing.
"$HELPER" write "$N" "$DB" > "$WORK/writer.out" 2>"$WORK/writer.err" &
WRITER_PID=$!

# Wait for READY (all N commits fsync'd).
for _ in $(seq 1 120); do
    grep -q "READY" "$WORK/writer.out" 2>/dev/null && break
    sleep 0.5
done
grep -q "READY" "$WORK/writer.out" 2>/dev/null || { kill -9 $WRITER_PID 2>/dev/null; fail "writer never reached READY (see $WORK/writer.err)"; }

# ---- POWER LOSS ----
# From here every write to the device is dropped (never reaches the backing
# image), just like writes lost to a power cut.
sudo dmsetup suspend "$DM"       || fail "suspend"
sudo dmsetup reload  "$DM" --table "0 $SECTORS flakey $LOOP 0 0 1 1 drop_writes" || fail "reload drop_writes"
sudo dmsetup resume  "$DM"       || fail "resume drop_writes"

# Anti-vacuous guard: a write issued AFTER drop_writes is engaged MUST NOT
# survive the reboot. If it does, drop_writes is not actually dropping and the
# whole test would be meaningless (data survives only because nothing was cut).
sudo sh -c "echo canary > '$MNT/sentinel_dropped'" 2>/dev/null
sudo sync 2>/dev/null

# RAM is gone after a power loss: discard the page cache so nothing unpersisted
# can be read back, and kill the writer without a clean close.
kill -9 "$WRITER_PID" 2>/dev/null
wait "$WRITER_PID" 2>/dev/null
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches' || fail "drop_caches"

# ---- REBOOT ----
sudo umount -l "$MNT" 2>/dev/null
sudo dmsetup suspend "$DM"       || fail "suspend2"
sudo dmsetup reload  "$DM" --table "0 $SECTORS flakey $LOOP 0 60 0" || fail "reload normal"
sudo dmsetup resume  "$DM"       || fail "resume normal"
sudo mount "/dev/mapper/$DM" "$MNT" || fail "remount"
sudo chmod 777 "$MNT"

# Anti-vacuous: the post-power-loss sentinel MUST be gone. If it survived,
# drop_writes never dropped anything and this test proves nothing.
if [ -e "$MNT/sentinel_dropped" ]; then
    fail "drop_writes NOT effective (sentinel survived) — test would be vacuous"
fi

# ---- VERIFY ----
if "$HELPER" verify "$N" "$DB" > "$WORK/verify.out" 2>&1; then
    echo "POWERLOSS TEST: PASS — $(cat "$WORK/verify.out")"
    exit 0
else
    echo "POWERLOSS TEST: FAIL — verifier reported:"
    cat "$WORK/verify.out"
    exit 1
fi
