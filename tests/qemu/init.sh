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
grep -q "^sleep image loaded" $S/status || fail "sleep image: $(grep '^sleep' $S/status)"
grep -q "^power-off image loaded, shutdown 0" $S/status || fail "power-off image: $(grep '^power-off' $S/status)"
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
	wait_for 'settled && grep -q "link 0," $S/status' 30 || fail "not idle: $(grep "^engine busy" $S/status)"
}
frames() { fval frames; }
dframes() { sed -n '1s/.*frames \([0-9]*\),.*/\1/p' $S/status; }
held() {
	sleep 2
	wait_for settled 30
	now=$($1)
	[ -n "$2" ] && [ "$now" = "$2" ] || fail "$3: $1 '$2' -> '$now'"
}
quiet() {
	idle
	f0=$(fval frames)
	held frames "$f0" "$1 redrew"
}
redraws() {
	idle
	f0=$(fval frames)
	echo "$2" > $S/$1 || fail "$1 $2"
	wait_for '[ "$(fval frames)" -gt "$f0" ] && settled' 30 || fail "$1 $2 did not redraw: $(grep "^engine busy" $S/status)"
}
sleepval() { sed -n "/^sleep/s/.*$1 \([0-9]*\).*/\1/p" $S/status; }
still() { held frames "$f0" "$1"; }
unchanged() { held dframes "$d0" "$1"; }
drawn() {
	wait_for '[ "$(fval frames)" -gt "$f0" ] && settled' 30 || fail "$1 not drawn: $(grep "^engine busy" $S/status)"
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
echo night > $S/ui_mode && [ "$(cat $S/ui_mode)" = night ] || fail "ui_mode night"
quiet "ui_mode night without a new frame"
echo "0 60 255" > $S/ui_levels
quiet "ui_levels in night while gamma is off"
redraws ui_gamma 1
redraws ui_gamma 0
echo "0 100 255" > $S/ui_levels
echo speed > $S/ui_mode
quiet "ui_mode speed without a new frame"
f0=$(fval frames)
kill $kms
wait $kms
drawn "the sleep image after kmsfill exits"
[ "$(sleepval shown)" = 1 ] || fail "sleep image shown $(sleepval shown) after kmsfill exits"
log "gamma: $(fake)"
hwc_start() {
	rm -f /hwc /hwc.log
	mkfifo /hwc
	hwcseq < /hwc > /hwc.log 2>&1 &
	hwc_pid=$!
	exec 3> /hwc
	hn=0
}
hwc() {
	hn=$((hn + 1))
	echo "$*" >&3
	wait_for "grep -q '^hwcseq: $hn ' /hwc.log" 10 || { fail "hwcseq '$*' did not answer"; return 2; }
	grep -q "^hwcseq: $hn ok\$" /hwc.log
}
hwc_stop() {
	exec 3>&-
	wait $hwc_pid
}
composer_cycle() {
	if [ "$1" = loaded ]; then per=1; else per=0; fi
	idle
	hwc_start
	s0=$(sleepval shown)
	w0=$(sleepval "wake refreshes")
	f0=$(fval frames)
	hwc off || fail "teardown of a CRTC without a plane: $(tail -1 /hwc.log)"
	still "power-off of an off CRTC"
	idle
	k0=$(sleepval "modesets skipped")
	f0=$(fval frames)
	hwc on || fail "modeset: $(tail -1 /hwc.log)"
	hwc ebc && fail "/dev/ebc drew the composer's modeset buffer"
	still "the composer's modeset buffer"
	hwc "frame 64" || fail "frame: $(tail -1 /hwc.log)"
	drawn "the first composer frame"
	idle
	d0=$(dframes)
	hwc "frame 64" || fail "repeated frame: $(tail -1 /hwc.log)"
	unchanged "an unchanged frame"
	hwc ebc || fail "/dev/ebc while on: $(tail -1 /hwc.log)"
	drawn "a /dev/ebc refresh"
	idle
	f0=$(fval frames)
	hwc off || fail "the composer's power-off commit: $(tail -1 /hwc.log)"
	if [ $per = 1 ]; then drawn "the sleep image"; else still "power-off without a sleep image"; fi
	idle
	f0=$(fval frames)
	hwc ebc && fail "/dev/ebc accepted while off"
	still "/dev/ebc while off"
	hwc on || fail "wake modeset: $(tail -1 /hwc.log)"
	hwc ebc && fail "/dev/ebc drew the wake modeset buffer"
	still "the wake modeset buffer"
	hwc "frame 192" || fail "frame after wake: $(tail -1 /hwc.log)"
	drawn "the frame after wake"
	idle
	d0=$(dframes)
	hwc "frame 192" || fail "repeated frame after wake: $(tail -1 /hwc.log)"
	unchanged "an unchanged frame after wake"
	[ "$(sleepval "wake refreshes")" = $((w0 + 2 * per)) ] ||
		fail "wake refreshes $w0 -> $(sleepval "wake refreshes")"
	[ "$(sleepval "modesets skipped")" = $((k0 + 2)) ] ||
		fail "modesets skipped $k0 -> $(sleepval "modesets skipped")"
	f0=$(fval frames)
	hwc_stop
	if [ $per = 1 ]; then drawn "the sleep image after the composer exits"; else still "composer exit without a sleep image"; fi
	idle
	[ "$(sleepval shown)" = $((s0 + 2 * per)) ] || fail "shown $s0 -> $(sleepval shown)"
	log "composer cycle, image $1: $(grep '^sleep' $S/status)"
}
composer_cycle loaded
shutdown_cycle() {
	echo x > $S/shutdown 2>/dev/null && fail "shutdown x accepted"
	echo 1 > $S/shutdown || fail "shutdown 1"
	grep -q "^power-off image loaded, shutdown 1" $S/status || fail "shutdown flag: $(grep '^power-off' $S/status)"
	s0=$(sleepval shown)
	idle
	hwc_start
	f0=$(fval frames)
	hwc on || fail "modeset during shutdown: $(tail -1 /hwc.log)"
	hwc "frame 64" || fail "frame during shutdown: $(tail -1 /hwc.log)"
	drawn "a composer frame during shutdown"
	idle
	f0=$(fval frames)
	hwc off || fail "power-off commit during shutdown: $(tail -1 /hwc.log)"
	still "the composer's power-off commit during shutdown"
	hwc on || fail "second modeset during shutdown: $(tail -1 /hwc.log)"
	hwc "frame 192" || fail "second frame during shutdown: $(tail -1 /hwc.log)"
	drawn "a second composer frame during shutdown"
	idle
	f0=$(fval frames)
	hwc_stop
	still "the composer's exit during shutdown"
	[ "$(sleepval shown)" = "$s0" ] || fail "sleep image shown during shutdown: $s0 -> $(sleepval shown)"
	echo 0 > $S/shutdown || fail "shutdown 0"
	grep -q "^power-off image loaded, shutdown 0" $S/status || fail "shutdown flag: $(grep '^power-off' $S/status)"
	log "shutdown: $(grep '^power-off' $S/status)"
}
shutdown_cycle
rm /lib/firmware/eink_sleep.gray
unload
load
grep -q "^sleep image missing" $S/status || fail "sleep image after rm: $(grep '^sleep' $S/status)"
composer_cycle missing
pattern black
pattern stripes
h_plain=$(fval hash)
pattern black
echo night > $S/ui_mode
pattern stripes
[ "$(fval hash)" != "$h_plain" ] || fail "night stripes match plain stripes: hash $h_plain"
pattern white
echo speed > $S/ui_mode
pattern white
log "night: $(fake)"

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
		wait_for '[ "$(fval disables)" -gt "$d0" ]' 10 || fail "link stayed on after mode $m: $(grep "^engine busy" $S/status)"
	fi
	i=$((i + 1))
done
log "modes: $(fake)"
grep -q "^pmic night: unavailable$" $S/status || fail "pmic line: $(tail -1 $S/status)"
[ "$(cat /sys/module/onyx_epd/parameters/night_pmic)" = N ] || fail "night_pmic default: $(cat /sys/module/onyx_epd/parameters/night_pmic)"
echo 1 > /sys/module/onyx_epd/parameters/night_pmic || fail "night_pmic 1"
echo night > $S/ui_mode
pattern white
pattern black
echo speed > $S/ui_mode
echo 0 > /sys/module/onyx_epd/parameters/night_pmic || fail "night_pmic 0"
grep -q "^pmic night: unavailable$" $S/status || fail "pmic line after night: $(tail -1 $S/status)"
log "night_pmic: $(fake)"

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
load
pattern stripes
echo 1 > $S/shutdown || fail "shutdown 1 before power-off"
log "power-off with onyx-epd bound: $(grep '^power-off' $S/status)"

n=$(dmesg | grep -c -E 'WARNING:|BUG:|KASAN|Unable to handle|Oops|possible circular|INFO: task|unbalanced')
[ "$n" = 0 ] || { fail "$n kernel complaints"; dmesg | grep -E -A3 'WARNING:|BUG:|KASAN|Unable to handle|Oops|possible circular|unbalanced' | head -40; }
[ $fails = 0 ] && log "RESULT ok" || log "RESULT $fails failures"
poweroff -f
