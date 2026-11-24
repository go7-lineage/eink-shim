package io.github.go7_lineage.einkshim;

public final class LevelsTest {
    private static int checks;
    private static int failures;

    private LevelsTest() {
    }

    private static void check(boolean ok, String what) {
        checks++;
        if (!ok) {
            failures++;
            System.out.println("FAIL " + what);
        }
    }

    private static void eq(Object got, Object want, String what) {
        check(want.equals(got), what + ": got " + got + ", want " + want);
    }

    private static void levels(Levels l, int black, int gamma, int white, String what) {
        eq(l.black, black, what + " black");
        eq(l.gamma, gamma, what + " gamma");
        eq(l.white, white, what + " white");
    }

    private static void parsesBlackGammaWhite() {
        levels(Levels.parse("16 140 240"), 16, 140, 240, "plain");
        levels(Levels.parse(" 0\t100  255\n"), 0, 100, 255, "whitespace");
        levels(Levels.parse("127 400 128"), 127, 400, 128, "upper limits");
        levels(Levels.parse("9 25 255"), 9, 25, 255, "lower gamma limit");
    }

    private static void rejectsWhatTheKernelRejects() {
        String[] bad = {
            null, "", "16 140", "16 140 240 1", "128 100 255", "0 100 127", "0 24 255", "0 401 255", "0 100 256",
            "+1 100 255", "-1 100 255", "a b c", "0 1.4 255", "99999 100 255", "0,100,255",
        };
        for (String s : bad) {
            eq(Levels.parse(s).format(), Levels.DEFAULT.format(), "parse(" + s + ")");
        }
    }

    private static void formatsForTheKernel() {
        eq(Levels.DEFAULT.format(), "0 100 255", "default");
        eq(new Levels(16, 140, 240).format(), "16 140 240", "format");
        eq(Levels.parse(new Levels(3, 77, 201).format()).format(), "3 77 201", "round trip");
    }

    private static void withChangesOneValue() {
        Levels l = new Levels(16, 140, 240);
        levels(l.withBlack(20), 20, 140, 240, "withBlack");
        levels(l.withGamma(150), 16, 150, 240, "withGamma");
        levels(l.withWhite(230), 16, 140, 230, "withWhite");
        levels(l, 16, 140, 240, "unchanged original");
    }

    private static void slidersCoverTheRanges() {
        eq(Levels.BLACK_STEPS, 127, "black steps");
        eq(Levels.blackFromProgress(0), 0, "black 0");
        eq(Levels.blackFromProgress(127), 127, "black max");
        eq(new Levels(16, 100, 255).blackProgress(), 16, "black progress");

        eq(Levels.GAMMA_STEPS, 30, "gamma steps");
        eq(Levels.gammaFromProgress(0), 50, "gamma 0");
        eq(Levels.gammaFromProgress(10), 100, "gamma 1.00");
        eq(Levels.gammaFromProgress(30), 200, "gamma max");
        eq(new Levels(0, 140, 255).gammaProgress(), 18, "gamma progress");
        eq(new Levels(0, 142, 255).gammaProgress(), 18, "gamma progress rounds down");
        eq(new Levels(0, 143, 255).gammaProgress(), 19, "gamma progress rounds up");
        eq(new Levels(0, 25, 255).gammaProgress(), 0, "gamma below the slider");
        eq(new Levels(0, 400, 255).gammaProgress(), 30, "gamma above the slider");

        eq(Levels.WHITE_STEPS, 127, "white steps");
        eq(Levels.whiteFromProgress(0), 128, "white 0");
        eq(Levels.whiteFromProgress(127), 255, "white max");
        eq(new Levels(0, 100, 240).whiteProgress(), 112, "white progress");
    }

    private static void gammaLabelHasTwoDecimals() {
        eq(Levels.gammaLabel(140), "1.40", "1.40");
        eq(Levels.gammaLabel(50), "0.50", "0.50");
        eq(Levels.gammaLabel(105), "1.05", "1.05");
        eq(Levels.gammaLabel(200), "2.00", "2.00");
    }

    public static void main(String[] args) {
        parsesBlackGammaWhite();
        rejectsWhatTheKernelRejects();
        formatsForTheKernel();
        withChangesOneValue();
        slidersCoverTheRanges();
        gammaLabelHasTwoDecimals();
        System.out.println(checks + " checks, " + failures + " failed");
        if (failures > 0) {
            System.exit(1);
        }
    }
}
