package io.github.go7_lineage.einkshim;

import java.util.Locale;
import java.util.regex.Pattern;

final class Levels {
    private static final int BLACK_MAX = 127;
    private static final int KERNEL_GAMMA_MIN = 25;
    private static final int KERNEL_GAMMA_MAX = 400;
    private static final int WHITE_MIN = 128;
    private static final int WHITE_MAX = 255;
    private static final int SLIDER_GAMMA_MIN = 50;
    private static final int SLIDER_GAMMA_MAX = 200;
    private static final int SLIDER_GAMMA_STEP = 5;
    private static final Pattern NUMBER = Pattern.compile("[0-9]{1,4}");

    static final int BLACK_STEPS = BLACK_MAX;
    static final int GAMMA_STEPS = (SLIDER_GAMMA_MAX - SLIDER_GAMMA_MIN) / SLIDER_GAMMA_STEP;
    static final int WHITE_STEPS = WHITE_MAX - WHITE_MIN;
    static final Levels DEFAULT = new Levels(0, 100, WHITE_MAX);

    final int black;
    final int gamma;
    final int white;

    Levels(int black, int gamma, int white) {
        this.black = black;
        this.gamma = gamma;
        this.white = white;
    }

    static Levels parse(String s) {
        if (s == null) return DEFAULT;
        String[] v = s.trim().split("\\s+");
        if (v.length != 3) return DEFAULT;
        for (String n : v) {
            if (!NUMBER.matcher(n).matches()) return DEFAULT;
        }
        Levels l = new Levels(Integer.parseInt(v[0]), Integer.parseInt(v[1]), Integer.parseInt(v[2]));
        boolean ok = l.black <= BLACK_MAX && l.gamma >= KERNEL_GAMMA_MIN && l.gamma <= KERNEL_GAMMA_MAX
                && l.white >= WHITE_MIN && l.white <= WHITE_MAX;
        return ok ? l : DEFAULT;
    }

    String format() {
        return black + " " + gamma + " " + white;
    }

    Levels withBlack(int v) {
        return new Levels(v, gamma, white);
    }

    Levels withGamma(int v) {
        return new Levels(black, v, white);
    }

    Levels withWhite(int v) {
        return new Levels(black, gamma, v);
    }

    int blackProgress() {
        return black;
    }

    int gammaProgress() {
        int p = Math.round((gamma - SLIDER_GAMMA_MIN) / (float) SLIDER_GAMMA_STEP);
        return Math.max(0, Math.min(GAMMA_STEPS, p));
    }

    int whiteProgress() {
        return white - WHITE_MIN;
    }

    static int blackFromProgress(int p) {
        return p;
    }

    static int gammaFromProgress(int p) {
        return SLIDER_GAMMA_MIN + p * SLIDER_GAMMA_STEP;
    }

    static int whiteFromProgress(int p) {
        return WHITE_MIN + p;
    }

    static String gammaLabel(int gamma) {
        return String.format(Locale.ROOT, "%d.%02d", gamma / 100, gamma % 100);
    }
}
