package io.github.go7_lineage.einkshim;

import android.os.SystemProperties;
import android.util.Log;

final class Props {
    private static final String TAG = "EinkSettings";
    private static final String GAMMA = "persist.vendor.eink_shim.gamma";
    private static final String LEVELS = "persist.vendor.eink_shim.levels";

    private Props() {
    }

    static boolean gammaOn() {
        return "1".equals(SystemProperties.get(GAMMA, "0"));
    }

    static void setGammaOn(boolean on) {
        set(GAMMA, on ? "1" : "0");
    }

    static Levels levels() {
        return Levels.parse(SystemProperties.get(LEVELS, ""));
    }

    static void setLevels(Levels l) {
        set(LEVELS, l.format());
    }

    private static void set(String key, String value) {
        try {
            SystemProperties.set(key, value);
        } catch (RuntimeException e) {
            Log.e(TAG, "Could not set " + key, e);
        }
    }
}
