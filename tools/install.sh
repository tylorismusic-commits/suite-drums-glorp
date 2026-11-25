#!/usr/bin/env bash
# User-local install from a source build.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build/GlorpDrums_artefacts/Release"
EDITOR_DIR="${SCRIPT_DIR}/build/GlorpKitEditor_artefacts/Release"
PLUGIN="Suite Drums Glorp"

echo "=== Installing ${PLUGIN} ==="

mkdir -p "$HOME/.vst3" "$HOME/.lv2" "$HOME/.local/bin"
mkdir -p "$HOME/.config/SuiteDrumsGlorp/kits"
mkdir -p "$HOME/.config/SuiteDrumsGlorp/presets"
mkdir -p "$HOME/.local/share/applications"
mkdir -p "$HOME/.local/share/icons/hicolor/256x256/apps"

if [ -d "${BUILD_DIR}/VST3/${PLUGIN}.vst3" ]; then
    rm -rf "$HOME/.vst3/${PLUGIN}.vst3"
    cp -r "${BUILD_DIR}/VST3/${PLUGIN}.vst3" "$HOME/.vst3/"
    echo "  [OK] VST3: $HOME/.vst3/${PLUGIN}.vst3"
else
    echo "  [WARN] VST3 bundle not found; build with -DGLORP_BUILD_VST3=ON"
fi

if [ -d "${BUILD_DIR}/LV2/${PLUGIN}.lv2" ]; then
    rm -rf "$HOME/.lv2/${PLUGIN}.lv2"
    cp -r "${BUILD_DIR}/LV2/${PLUGIN}.lv2" "$HOME/.lv2/"
    echo "  [OK] LV2: $HOME/.lv2/${PLUGIN}.lv2"
else
    echo "  [WARN] LV2 bundle not found; build with -DGLORP_BUILD_LV2=ON"
fi

if [ -f "${EDITOR_DIR}/SuiteKitStudio" ]; then
    install -m 0755 "${EDITOR_DIR}/SuiteKitStudio" "$HOME/.local/bin/SuiteKitStudio"
    echo "  [OK] Suite Kit Studio: $HOME/.local/bin/SuiteKitStudio"
else
    echo "  [WARN] SuiteKitStudio not found; build with -DGLORP_BUILD_KIT_EDITOR=ON"
fi

cp "${SCRIPT_DIR}/assets/suite_drums_icon.png" \
   "$HOME/.local/share/icons/hicolor/256x256/apps/suite_drums_icon.png" 2>/dev/null || true
cp "${SCRIPT_DIR}/assets/suite_kit_studio_icon.png" \
   "$HOME/.local/share/icons/hicolor/256x256/apps/suite_kit_studio_icon.png" 2>/dev/null || true

cat > "$HOME/.local/share/applications/suite-kit-studio.desktop" <<EOF
[Desktop Entry]
Name=Suite Kit Studio
Comment=.glorpkit Drum Kit Creator & Editor
Exec=$HOME/.local/bin/SuiteKitStudio
Icon=suite_kit_studio_icon
Terminal=false
Type=Application
Categories=AudioVideo;Audio;
StartupWMClass=SuiteKitStudio
EOF

if [ -d "${SCRIPT_DIR}/test/kits/GlorpDemoKit" ]; then
    rm -rf "$HOME/.config/SuiteDrumsGlorp/kits/GlorpDemoKit"
    cp -r "${SCRIPT_DIR}/test/kits/GlorpDemoKit" "$HOME/.config/SuiteDrumsGlorp/kits/"
    echo "  [OK] Starter kit: $HOME/.config/SuiteDrumsGlorp/kits/GlorpDemoKit"
fi

echo "=== ${PLUGIN} installed. Rescan plugins in your DAW. ==="
