package io.github.go7_lineage.einkshim;

import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

public class GammaTile extends TileService {
    @Override
    public void onStartListening() {
        show();
    }

    @Override
    public void onClick() {
        Props.setGammaOn(!Props.gammaOn());
        show();
    }

    private void show() {
        Tile tile = getQsTile();
        if (tile == null) return;
        boolean on = Props.gammaOn();
        tile.setState(on ? Tile.STATE_ACTIVE : Tile.STATE_INACTIVE);
        tile.setSubtitle(getString(on ? R.string.on : R.string.off));
        tile.updateTile();
    }
}
