#!/usr/bin/env bash
#
# Builds the Linux release archive for Suite Drums Glorp.
#
# Usage:
#   tools/package_linux.sh [version]
#
# The build must already exist (see README). The version defaults to the
# project version plus the alpha suffix, e.g. 0.1.0-alpha.18.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${1:-0.1.0-alpha.18}"
NAME="suite-drums-glorp-${VERSION}-linux-x86_64"
BUILD="${ROOT}/build"
ARTEFACTS="${BUILD}/GlorpDrums_artefacts/Release"
KIT_EDITOR="${BUILD}/GlorpKitEditor_artefacts/Release/SuiteKitStudio"
STAGE="${BUILD}/stage/${NAME}"
DIST="${BUILD}/dist"

VST3="${ARTEFACTS}/VST3/Suite Drums Glorp.vst3"
LV2="${ARTEFACTS}/LV2/Suite Drums Glorp.lv2"

for required in "${VST3}" "${LV2}" "${KIT_EDITOR}"; do
    if [ ! -e "${required}" ]; then
        echo "error: missing build artefact: ${required}" >&2
        echo "Build first with: cmake --build build -j\"\$(nproc)\"" >&2
        exit 1
    fi
done

rm -rf "${STAGE}"
mkdir -p "${STAGE}/vst3" "${STAGE}/lv2" "${STAGE}/bin" "${STAGE}/kits" "${STAGE}/docs" "${STAGE}/icons"

cp -r "${VST3}" "${STAGE}/vst3/"
cp -r "${LV2}" "${STAGE}/lv2/"
cp "${KIT_EDITOR}" "${STAGE}/bin/SuiteKitStudio"
chmod +x "${STAGE}/bin/SuiteKitStudio"

cp "${ROOT}/README.md" "${ROOT}/LICENSE" "${ROOT}/NOTICE.md" "${STAGE}/"
cp "${ROOT}/docs/screenshot.png" "${ROOT}/docs/mixer_view.png" \
   "${ROOT}/docs/mixer_fx.png" "${ROOT}/docs/edrum_wizard.png" \
   "${ROOT}/docs/kit_editor.png" "${ROOT}/docs/midi_page.png" \
   "${ROOT}/docs/index.html" "${STAGE}/docs/"
cp "${ROOT}/assets/suite_drums_icon.png" "${ROOT}/assets/suite_kit_studio_icon.png" "${STAGE}/icons/"

# Starter kit: real DrumGizmo folder kit, not the LFS pointer archive.
if [ -d "${ROOT}/test/kits/GlorpDemoKit" ]; then
    cp -r "${ROOT}/test/kits/GlorpDemoKit" "${STAGE}/kits/"
    cat > "${STAGE}/kits/README.md" <<'EOF'
# Glorp Demo Kit

This is the optional starter kit bundled with Suite Drums Glorp.

- Kit author / credit: Alexander Holm / GlorpDrums
- Kit metadata license: CC-BY-SA / CC0
- Format: DrumGizmo folder kit (drumkit.xml)

Copy this folder to `~/.config/SuiteDrumsGlorp/kits/` or point Suite Drums
Glorp at it from the FILE menu. Keep this notice with the samples when
redistributing them.
EOF
fi

cat > "${STAGE}/install.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN="Suite Drums Glorp"

mkdir -p "$HOME/.vst3" "$HOME/.lv2" "$HOME/.local/bin"
mkdir -p "$HOME/.config/SuiteDrumsGlorp/kits"
mkdir -p "$HOME/.local/share/applications"
mkdir -p "$HOME/.local/share/icons/hicolor/256x256/apps"

rm -rf "$HOME/.vst3/$PLUGIN.vst3"
cp -r "$HERE/vst3/$PLUGIN.vst3" "$HOME/.vst3/"

rm -rf "$HOME/.lv2/$PLUGIN.lv2"
cp -r "$HERE/lv2/$PLUGIN.lv2" "$HOME/.lv2/"

install -m 0755 "$HERE/bin/SuiteKitStudio" "$HOME/.local/bin/SuiteKitStudio"

if [ -d "$HERE/kits/GlorpDemoKit" ]; then
    rm -rf "$HOME/.config/SuiteDrumsGlorp/kits/GlorpDemoKit"
    cp -r "$HERE/kits/GlorpDemoKit" "$HOME/.config/SuiteDrumsGlorp/kits/"
fi

# Desktop launcher and icons
if [ -d "$HERE/icons" ]; then
    cp "$HERE/icons/"*.png "$HOME/.local/share/icons/hicolor/256x256/apps/"
fi

cat > "$HOME/.local/share/applications/suite-kit-studio.desktop" <<'DESKTOP'
[Desktop Entry]
Name=Suite Kit Studio
Comment=.glorpkit Drum Kit Creator & Editor
Exec=SuiteKitStudio
Icon=suite_kit_studio_icon
Terminal=false
Type=Application
Categories=AudioVideo;Audio;
StartupWMClass=SuiteKitStudio
DESKTOP

echo "Installed Suite Drums Glorp to $HOME/.vst3 and $HOME/.lv2"
echo "Installed Suite Kit Studio to $HOME/.local/bin/SuiteKitStudio"
echo "Rescan plugins in your DAW."
EOF
chmod +x "${STAGE}/install.sh"

mkdir -p "${DIST}"
rm -f "${DIST}/${NAME}.zip" "${DIST}/${NAME}.zip.sha256"
(
    cd "$(dirname "${STAGE}")"
    zip -r -q "${DIST}/${NAME}.zip" "$(basename "${STAGE}")"
)
(
    cd "${DIST}"
    sha256sum "${NAME}.zip" > "${NAME}.zip.sha256"
)
echo "${DIST}/${NAME}.zip"
echo "${DIST}/${NAME}.zip.sha256"
