#!/bin/bash
# Build the fail-closed MU1316 RetroArch HMI runtime injector.
#
# The jar deliberately does NOT contain replacements for:
#   SystemSMMInitStates, SystemSMMInitTransitions, SystemScreenFactory.
# Stock HMI initialization therefore completes unchanged.  The only OEM class
# shadow is the small MainWizard PlaceholderMenuItem wrapper used as bootstrap.
set -eu

PATCH_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$PATCH_DIR/.." && pwd)"
AUDI2_DIR="$(cd "$PROJECT_DIR/../.." && pwd)"

PATCH_JDK="${JAVA_HOME:-$AUDI2_DIR/Tools/jxe2jar/jvms/zulu8.78.0.19-ca-jdk8.0.412-macosx_aarch64}"
PATCH_JAVAC="$PATCH_JDK/bin/javac"
PATCH_JAR_TOOL="$PATCH_JDK/bin/jar"
PATCH_JAVAP="$PATCH_JDK/bin/javap"

LSD_CLASSES="${LSD_JAR:-$AUDI2_DIR/Tools/jxe2jar/out/MU1316-combined-final.jar}"
DEVICE_JCL="${JCL_JAR:-$AUDI2_DIR/Tools/jxe2jar/libs/jcl/MHI2Q_US_AUG22_P5087_MU1316/jcl.jar}"
OSGI_CLASSES="${OSGI_JAR:-$AUDI2_DIR/Firmwares/HU/MU1367-MHI2_ER_VWG13_K4525/app/eso/hmi/lsd/lib/osgi.jar}"
OUTPUT_JAR="${OUT_JAR:-$PATCH_DIR/ra_mhi2q.jar}"
SOURCE_DIR="$PROJECT_DIR/java_patch/java_src"

[ -x "$PATCH_JAVAC" ] || { echo "ERROR: JDK 8 not found: $PATCH_JDK"; exit 1; }
[ -f "$LSD_CLASSES" ] || { echo "ERROR: MU1316 classes not found: $LSD_CLASSES"; exit 1; }
[ -f "$DEVICE_JCL" ] || { echo "ERROR: MU1316 JCL not found: $DEVICE_JCL"; exit 1; }

BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

BUILD_CP="$LSD_CLASSES"
[ -f "$OSGI_CLASSES" ] && BUILD_CP="$BUILD_CP:$OSGI_CLASSES"
JAVA_SOURCES=$(find "$SOURCE_DIR" -type f -name '*.java' | sort)

echo "== Runtime SystemSMM injector build =="
echo "classes: $LSD_CLASSES"
echo "JCL:     $DEVICE_JCL"
echo "output:  $OUTPUT_JAR"
echo
echo "Compiling Java 1.4 against the exact MU1316 device JCL..."
"$PATCH_JAVAC" -source 1.4 -target 1.4 -nowarn \
    -bootclasspath "$DEVICE_JCL" -extdirs "" \
    -cp "$BUILD_CP" -sourcepath "$SOURCE_DIR" \
    -d "$BUILD_DIR" $JAVA_SOURCES

(cd "$BUILD_DIR" && "$PATCH_JAR_TOOL" cf "$OUTPUT_JAR" .)

echo
echo "Verifying forbidden core shadows are absent..."
for FORBIDDEN_CLASS in \
    de/audi/tghu/system/sm/SystemSMMInitStates.class \
    de/audi/tghu/system/sm/SystemSMMInitTransitions.class \
    de/audi/tghu/system/hmi/evohigh/SystemScreenFactory.class \
    de/audi/audio/init/AudioActivator.class \
    de/audi/audio/services/BaseAudioService.class \
    de/audi/audio/dsi/DSIAudioListenerImpl.class \
    de/audi/atip/audio/HMIAudioService.class \
    org/dsi/ifc/media/DSIMediaRouter.class; do
    if "$PATCH_JAR_TOOL" tf "$OUTPUT_JAR" | grep -q "^$FORBIDDEN_CLASS$"; then
        echo "ERROR: forbidden core shadow in jar: $FORBIDDEN_CLASS"
        exit 1
    fi
done

for REQUIRED_CLASS in \
    'de/esolutions/hmi/widgets/audi/evo/widgets/AbstractPlaceholderMenuController$1.class' \
    de/audi/tghu/system/hmi/evohigh/RaScreen.class \
    de/luka/ra/inject/audio/AudioFocusBridge.class \
    de/luka/ra/inject/audio/DsiReflection.class \
    de/luka/ra/inject/sm/RuntimeSmmInjector.class; do
    if ! "$PATCH_JAR_TOOL" tf "$OUTPUT_JAR" | grep -q "^$REQUIRED_CLASS$"; then
        echo "ERROR: required class missing: $REQUIRED_CLASS"
        exit 1
    fi
done

echo "Checking bytecode compatibility..."
for CHECK_CLASS in \
    de.luka.ra.inject.sm.RuntimeSmmInjector \
    de.luka.ra.inject.items.RetroArchHook \
    de.luka.ra.inject.audio.AudioFocusBridge \
    de.luka.ra.inject.audio.DsiReflection \
    de.audi.tghu.system.hmi.evohigh.RaScreen; do
    MAJOR=$("$PATCH_JAVAP" -classpath "$OUTPUT_JAR:$LSD_CLASSES" -verbose "$CHECK_CLASS" \
        | awk '/major version:/{print $3; exit}')
    [ "$MAJOR" = "48" ] || { echo "ERROR: $CHECK_CLASS major=$MAJOR, expected 48"; exit 1; }
    echo "  major=$MAJOR $CHECK_CLASS"
done

if "$PATCH_JAVAP" -classpath "$OUTPUT_JAR:$LSD_CLASSES" -p -c \
        de.luka.ra.inject.sm.RuntimeSmmInjector \
        de.luka.ra.inject.items.RetroArchHook \
        | grep -q 'java/lang/Integer.valueOf:(I)'; then
    echo "ERROR: unsupported Integer.valueOf(int) found"
    exit 1
fi

INJECTOR_BYTECODE=$("$PATCH_JAVAP" -classpath "$OUTPUT_JAR:$LSD_CLASSES" -p -c \
    de.luka.ra.inject.sm.RuntimeSmmInjector)
echo "$INJECTOR_BYTECODE" | grep -q 'reinitActiveStateStack' || {
    echo "ERROR: live SMI active-state refresh missing"
    exit 1
}
echo "  live SMI active-state refresh present"

AUDIO_BYTECODE=$("$PATCH_JAVAP" -classpath "$OUTPUT_JAR:$LSD_CLASSES" -p -c \
    de.luka.ra.inject.audio.AudioFocusBridge)
for REQUIRED_CALL in \
    HMIAudioService.requestAndFadeToConnection \
    HMIAudioService.releaseConnection \
    DSIMediaRouter.registerClient \
    DSIMediaRouter.setAudioRoutes \
    DSIMediaRouter.startStreaming \
    IAudioFocusManager.setActiveAudioApp; do
    echo "$AUDIO_BYTECODE" | grep -q "$REQUIRED_CALL" || {
        echo "ERROR: direct stock audio call missing: $REQUIRED_CALL"
        exit 1
    }
done
if unzip -p "$OUTPUT_JAR" | grep -q 'retroarch-agent.fragment'; then
    echo "ERROR: obsolete native DSI framework fragment reference in jar"
    exit 1
fi
echo "  stock MU1316 audio calls present; no OEM audio shadows"

echo
echo "Jar contents:"
"$PATCH_JAR_TOOL" tf "$OUTPUT_JAR" | sort
echo
echo "PASS: built $OUTPUT_JAR without SystemSMM/factory core shadows"
echo "runtime constants: RA_SCREEN=250 DRUM_STATE=89 EV_ENTER=9990001 EV_EXIT=9990002"
