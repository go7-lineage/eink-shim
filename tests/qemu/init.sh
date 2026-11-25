#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs dev /dev 2>/dev/null
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null

fails=0
log() { echo "epd-test: $*"; }
fail() { echo "epd-test: FAIL $*"; fails=$((fails + 1)); }
S=/sys/devices/virtual/sepdc/debug
P=/sys/bus/platform/devices/epd
fake() { cat /sys/bus/platform/drivers/onyx-test-dsi/*/state 2>/dev/null; }
fval() { fake | tr ' ' '\n' | sed -n "/^$1\$/{n;p;}"; }
wait_for() {
	w=0
	while ! eval "$1" && [ $w -lt "$2" ]; do sleep 1; w=$((w + 1)); done
	eval "$1"
}
load() {
	for m in onyx-test-dsi onyx-go7-tcon panel-onyx-go7-eink onyx-epd; do
		insmod /lib/modules/$m.ko || fail "insmod $m"
	done
	wait_for 'grep -q "output ready" $S/status 2>/dev/null' 60 || fail "output not ready: $(head -1 $S/status 2>&1)"
}
unload() {
	for m in onyx_epd panel_onyx_go7_eink onyx_go7_tcon onyx_test_dsi; do
		rmmod $m || fail "rmmod $m"
	done
}
settled() {
	grep -q "powered 0" $S/status && grep -q "engine busy 0, queued 0" $S/status &&
		grep -q "inflight -1 pending -1 filling -1" $S/status
}
pattern() {
	f0=$(fval frames)
	echo "$*" > $P/test_pattern || fail "test_pattern $*"
	if ! wait_for '[ "$(fval frames)" -gt "$f0" ] && settled' 30; then
		fail "pattern $*: frames $f0 -> $(fval frames)"
		cat $S/status
	fi
}

log "start"
load
log "loaded: $(fake)"
grep -q "usercode" /sys/bus/i2c/devices/*-003c/chipid || fail "tcon chipid"
wait_for 'grep -q "temperature 24000 mC from temp_c (qcom_qg: -19), waveform range 20-30 C" $S/status' 5 ||
	fail "temperature: $(grep temperature $S/status)"

pattern white
log "white: $(fake)"
[ "$(fval dcs)" = 2 ] || fail "panel init sent $(fval dcs) DCS packets, want 2"
[ "$(fval dcs_bad)" = 0 ] || fail "DCS packets without LPM: $(fval dcs_bad)"
pattern stripes
pattern checker
pattern diagonal
log "patterns: $(fake)"
echo bogus > $P/test_pattern 2>/dev/null && fail "bogus pattern accepted"

d0=$(fval disables)
wait_for '[ "$(fval disables)" -gt "$d0" ]' 10 || fail "link did not go off: $(fake)"
e0=$(fval enables)
pattern black
[ "$(fval enables)" -gt "$e0" ] || fail "link did not come back: $(fake)"
[ "$(fval dcs)" = 2 ] || fail "panel re-init on link-up: $(fval dcs) DCS packets"
log "link cycle: $(fake)"

echo regal > $S/ui_mode && [ "$(cat $S/ui_mode)" = regal ] || fail "ui_mode regal"
echo bogus > $S/ui_mode 2>/dev/null && fail "ui_mode bogus accepted"
[ "$(cat $S/ui_mode)" = regal ] || fail "ui_mode changed by a bad write"
echo speed > $S/ui_mode
echo 300 > $S/cut_frame_num 2>/dev/null && fail "cut 300 accepted"
echo 2 > $S/cut_frame_num && [ "$(cat $S/cut_frame_num)" = 2 ] || fail "cut 2"

idle() {
	wait_for 'settled && grep -q "link 0," $S/status' 30 || fail "not idle: $(tail -1 $S/status)"
}
quiet() {
	idle
	f0=$(fval frames)
	sleep 2
	wait_for settled 30
	[ "$(fval frames)" = "$f0" ] || fail "$1 redrew: frames $f0 -> $(fval frames)"
}
redraws() {
	idle
	f0=$(fval frames)
	echo "$2" > $S/$1 || fail "$1 $2"
	wait_for '[ "$(fval frames)" -gt "$f0" ] && settled' 30 || fail "$1 $2 did not redraw: $(tail -1 $S/status)"
}
[ "$(cat $S/ui_gamma)" = 0 ] || fail "ui_gamma default $(cat $S/ui_gamma)"
[ "$(cat $S/ui_levels)" = "0 100 255" ] || fail "ui_levels default $(cat $S/ui_levels)"
for v in "0 100 127" "128 100 255" "0 24 255" "0 401 255" "0 100" "x"; do
	echo "$v" > $S/ui_levels 2>/dev/null && fail "ui_levels $v accepted"
done
echo 2 > $S/ui_gamma 2>/dev/null && fail "ui_gamma 2 accepted"
[ "$(cat $S/ui_levels)" = "0 100 255" ] || fail "ui_levels changed by a bad write: $(cat $S/ui_levels)"
echo "16 140 240" > $S/ui_levels && echo 1 > $S/ui_gamma || fail "gamma without a framebuffer"
quiet "gamma without a framebuffer"
echo 0 > $S/ui_gamma
idle
f0=$(fval frames)
kmsfill & kms=$!
wait_for '[ "$(fval frames)" -gt "$f0" ] && settled' 30 || fail "kmsfill frame not shown: $(fake)"
quiet "a still framebuffer"
redraws ui_gamma 1
[ "$(cat $S/ui_gamma)" = 1 ] && [ "$(cat $S/ui_levels)" = "16 140 240" ] || fail "gamma read back"
echo 1 > $S/ui_gamma
quiet "an unchanged ui_gamma"
echo "16 140 240" > $S/ui_levels
quiet "unchanged ui_levels"
redraws ui_levels "16 150 240"
redraws ui_levels "0 100 255"
redraws ui_levels "0 60 255"
redraws ui_gamma 0
echo "0 100 255" > $S/ui_levels
quiet "ui_levels while gamma is off"
echo 2 > $S/ui_flash 2>/dev/null && fail "ui_flash 2 accepted"
redraws ui_flash 1
grep "^ui " $S/status | grep -q "flashes 1 (dropped 0)" || fail "ui_flash count: $(grep "^ui " $S/status)"
kill $kms
wait $kms
log "gamma: $(fake)"

i=0
for m in 2 4 1 5 2 16 15 14 2 4 4 1 2 5 2 3 2; do
	set -- white black stripes checker diagonal
	shift $((i % 5))
	if [ $m = 3 ]; then
		f0=$(fval frames)
		echo "$1 3" > $P/test_pattern
		wait_for settled 10 || fail "mode 3 left work behind"
		[ "$(fval frames)" = "$f0" ] || fail "mode 3 sent frames"
	else
		pattern $1 $m
	fi
	if [ $((i % 6)) = 5 ]; then
		d0=$(fval disables)
		wait_for '[ "$(fval disables)" -gt "$d0" ]' 10 || fail "link stayed on after mode $m: $(tail -1 $S/status)"
	fi
	i=$((i + 1))
done
log "modes: $(fake)"

echo devices > /sys/power/pm_test
echo freeze > /sys/power/state || fail "suspend"
log "after idle suspend: $(fake)"
pattern white
f0=$(fval frames)
for n in white black stripes; do echo "$n 14" > $P/test_pattern; done
echo freeze > /sys/power/state || fail "suspend while busy"
wait_for settled 60 || { fail "not settled after a busy suspend"; cat $S/status; }
[ "$(fval late)" = 0 ] || fail "$(fval late) commits reached the display controller while it was suspended"
[ "$(fval frames)" -gt "$f0" ] || fail "no frames after a busy suspend"
echo none > /sys/power/pm_test
log "after busy suspend: $(fake)"
pattern white

unload
log "unloaded"
load
pattern stripes
log "reloaded: $(fake)"

supplier_cycle() {
	eval "$1" || fail "$1"
	[ -e $S/status ] && fail "onyx-epd still bound after: $1"
	eval "$2" || fail "$2"
	echo epd > /sys/bus/platform/drivers/onyx-epd/bind || fail "bind onyx-epd after: $2"
	wait_for 'grep -q "output ready" $S/status 2>/dev/null' 60 || fail "output not ready after: $2"
	pattern white
	log "$1, $2: $(fake)"
}
supplier_cycle "rmmod panel_onyx_go7_eink" "insmod /lib/modules/panel-onyx-go7-eink.ko"
supplier_cycle "rmmod onyx_go7_tcon" "insmod /lib/modules/onyx-go7-tcon.ko"
unload

n=$(dmesg | grep -c -E 'WARNING:|BUG:|KASAN|Unable to handle|Oops|possible circular|INFO: task|unbalanced')
[ "$n" = 0 ] || { fail "$n kernel complaints"; dmesg | grep -E -A3 'WARNING:|BUG:|KASAN|Unable to handle|Oops|possible circular|unbalanced' | head -40; }
[ $fails = 0 ] && log "RESULT ok" || log "RESULT $fails failures"
poweroff -f
