package io.github.go7_lineage.einkshim;

import android.os.SystemProperties;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;
import android.util.Log;

/** QS tile that cycles E-Ink refresh mode */
public class ModeTile extends TileService {
    private static final String TAG = "EinkShimTile";
    private static final String PROP = "persist.vendor.eink_shim.mode";

    // Same names in the shim's src/modes.rs, first is default
    private static final String[] MODES = { "speed", "normal", "regal", "regalplus" };
    private static final String[] NAMES = { "Speed", "Normal", "Regal", "Regal+" };

    @Override
    public void onStartListening() {
        show();
    }

    @Override
    public void onClick() {
        try {
            SystemProperties.set(PROP, MODES[(current() + 1) % MODES.length]);
        } catch (RuntimeException e) {
            Log.e(TAG, "Could not set " + PROP, e);
        }
        show();
    }

    private void show() {
        Tile tile = getQsTile();
        if (tile == null) return;
        tile.setSubtitle(NAMES[current()]);
        tile.setState(Tile.STATE_ACTIVE);
        tile.updateTile();
    }

    private static int current() {
        String mode = SystemProperties.get(PROP, "");
        for (int i = 0; i < MODES.length; i++) {
            if (MODES[i].equals(mode)) return i;
        }
        return 0;
    }
}
