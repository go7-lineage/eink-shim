package io.github.go7_lineage.einkshim;

import android.app.Activity;
import android.os.Bundle;
import android.widget.CompoundButton;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

public class SettingsActivity extends Activity
        implements CompoundButton.OnCheckedChangeListener, SeekBar.OnSeekBarChangeListener {
    private Switch gamma;
    private SeekBar black;
    private SeekBar curve;
    private SeekBar white;
    private TextView blackLabel;
    private TextView curveLabel;
    private TextView whiteLabel;
    private Levels levels = Levels.DEFAULT;
    private boolean tracking;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        setContentView(R.layout.settings);
        gamma = findViewById(R.id.gamma);
        black = findViewById(R.id.black);
        curve = findViewById(R.id.curve);
        white = findViewById(R.id.white);
        blackLabel = findViewById(R.id.black_label);
        curveLabel = findViewById(R.id.curve_label);
        whiteLabel = findViewById(R.id.white_label);
        black.setMax(Levels.BLACK_STEPS);
        curve.setMax(Levels.GAMMA_STEPS);
        white.setMax(Levels.WHITE_STEPS);
    }

    @Override
    protected void onResume() {
        super.onResume();
        load();
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus && !tracking) load();
    }

    private void load() {
        gamma.setOnCheckedChangeListener(null);
        black.setOnSeekBarChangeListener(null);
        curve.setOnSeekBarChangeListener(null);
        white.setOnSeekBarChangeListener(null);
        boolean on = Props.gammaOn();
        levels = Props.levels();
        gamma.setChecked(on);
        black.setProgress(levels.blackProgress());
        curve.setProgress(levels.gammaProgress());
        white.setProgress(levels.whiteProgress());
        showLevels();
        setSlidersEnabled(on);
        gamma.setOnCheckedChangeListener(this);
        black.setOnSeekBarChangeListener(this);
        curve.setOnSeekBarChangeListener(this);
        white.setOnSeekBarChangeListener(this);
    }

    @Override
    public void onCheckedChanged(CompoundButton button, boolean on) {
        Props.setGammaOn(on);
        setSlidersEnabled(on);
    }

    @Override
    public void onProgressChanged(SeekBar bar, int progress, boolean fromUser) {
        if (!fromUser) return;
        if (bar == black) {
            levels = levels.withBlack(Levels.blackFromProgress(progress));
        } else if (bar == curve) {
            levels = levels.withGamma(Levels.gammaFromProgress(progress));
        } else {
            levels = levels.withWhite(Levels.whiteFromProgress(progress));
        }
        showLevels();
        if (!tracking) Props.setLevels(levels);
    }

    @Override
    public void onStartTrackingTouch(SeekBar bar) {
        tracking = true;
    }

    @Override
    public void onStopTrackingTouch(SeekBar bar) {
        tracking = false;
        Props.setLevels(levels);
    }

    private void showLevels() {
        blackLabel.setText(getString(R.string.black_point, levels.black));
        curveLabel.setText(getString(R.string.gamma_value, Levels.gammaLabel(levels.gamma)));
        whiteLabel.setText(getString(R.string.white_point, levels.white));
    }

    private void setSlidersEnabled(boolean on) {
        black.setEnabled(on);
        curve.setEnabled(on);
        white.setEnabled(on);
    }
}
