#!/vendor/bin/sh
# vendor.hwc.drm.device = the onyx-epd card. Card numbers follow probe order (test3: card0 adreno, card1 msm,
# card2 onyx-epd), and the composer must not open the msm card: its DSI link belongs to onyx-epd's in-kernel
# client, which can't commit while userspace holds DRM master there.
hide_msm() {
    for c in /sys/class/drm/card*; do
        case ${c##*/} in *-*) continue ;; esac
        case $(readlink $c/device/driver) in
        */msm_dpu) chmod 000 /dev/dri/${c##*/} || exit 1 ;;
        esac
    done
}

i=0
while [ $i -lt 50 ]; do
    for c in /sys/class/drm/card*; do
        n=${c##*/}
        case $n in *-*) continue ;; esac
        case $(readlink $c/device/driver) in
        */onyx-epd)
            if [ "$(getprop ro.bootmode)" = charger ]; then
                [ -c /dev/dri/$n ] || continue
                hide_msm
            fi
            setprop vendor.hwc.drm.device /dev/dri/$n
            exit 0
            ;;
        esac
    done
    sleep 0.1
    i=$((i + 1))
done
exit 1	# init logs the exit status
