package com.luka.retroarch.inject.ids;

/**
 * Main Wizard icon/widget IDs used by stock MU1316 LSD.
 *
 * Notes:
 * - On MainWizard, widgetID also acts as the icon selector. Don't auto-generate
 *   new values unless you know the platform has an icon for it.
 * - Not all IDs are always visible; some appear only when features are present.
 */
public final class MainWizardWidgetIds {
    private MainWizardWidgetIds() {
    }

    /**
     * All widget IDs observed in stock MU1316 LSD (SystemScreenBag2.mAINWIZARD).
     *
     * These IDs are "icon selectors" for Main Wizard. Some are feature-gated and may be hidden
     * on your setup (Android Auto / CarPlay / etc). Do not reuse these IDs for custom items.
     *
     * Names:
     * - For the core Main Wizard (0..9) we use the semantic MW_* names from
     *   de.audi.atip.statemachine.SMEventSystem (MW_CAR, MW_TUNER, MW_MEDIA, ...).
     * - For smartphone integration items (16..19), names are based on typical MHI2 behavior
     *   and the observed on-screen icons (AA / CarPlay / CarLife / Terminal Mode).
     */
    // Core MainWizard (values match ChoiceModel#138 values and icon selector IDs).
    public static final int CAR = 0;        // "Vehicle"
    public static final int TUNER = 1;      // "Radio"
    public static final int MEDIA = 2;      // "Media"
    public static final int TEL = 3;        // "Telephone" (labelId 247 in SystemScreenBag2)
    public static final int NAV_DEST = 4;   // "Navigation" / destinations (labelId 248 in SystemScreenBag2)
    public static final int NAV_MAP = 5;    // "Map" (labelId 222 in SystemScreenBag2)
    public static final int ONLINE = 6;     // "Audi connect" / online (labelId 0 in SystemScreenBag2)
    public static final int OFFICE = 7;     // MW_OFFICE event=1666 (labelId 192 in SystemScreenBag2)
    public static final int TONE = 8;       // "Sound" (labelId 250 in SystemScreenBag2)
    public static final int SETTINGS = 9;   // MW_SETTINGS event=1534

    // Optional / feature-gated MainWizard items (still "icon selector" widget IDs).
    public static final int BCALL = 13;        // MW_BCALL event=1977 ("Audi call"/assist)
    public static final int TPEG = 15;         // MW_TPEG event=1991
    public static final int BAIDU_CARLIFE = 16; // "Baidu CarLife" (icon observed on some units)
    public static final int CARPLAY = 17;      // "Apple CarPlay"
    public static final int ANDROID_AUTO = 18; // "Android Auto"
    public static final int TERMINALMODE = 19; // MW_TERMINALMODE event=1959 ("Smartphone connection"/terminal mode)

    // Backward-compatible aliases (older code used ID_* or wrong MAP name).
    public static final int ID_0 = CAR;
    public static final int ID_1 = TUNER;
    public static final int ID_2 = MEDIA;
    public static final int ID_3 = TEL;
    public static final int ID_4 = NAV_DEST;
    public static final int ID_5 = NAV_MAP;
    public static final int ID_6 = ONLINE;
    public static final int ID_7 = OFFICE;
    public static final int ID_8 = TONE;
    public static final int ID_9 = SETTINGS;
    public static final int ID_13 = BCALL;
    public static final int ID_15 = TPEG;
    public static final int ID_16 = BAIDU_CARLIFE;
    public static final int ID_17 = CARPLAY;
    public static final int ID_18 = ANDROID_AUTO;
    public static final int ID_19 = TERMINALMODE;

    public static final int[] ALL = new int[] {
            CAR,
            TUNER,
            MEDIA,
            TEL,
            NAV_DEST,
            NAV_MAP,
            ONLINE,
            OFFICE,
            TONE,
            SETTINGS,
            BCALL,
            TPEG,
            BAIDU_CARLIFE,
            CARPLAY,
            ANDROID_AUTO,
            TERMINALMODE
    };
}
