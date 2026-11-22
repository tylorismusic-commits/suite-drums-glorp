#!/usr/bin/env bash
# Reinstall just the compiled plugins into the current user's plugin folders.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_DIR/build/GlorpDrums_artefacts/Release"
PLUGIN="Suite Drums Glorp"

mkdir -p "$HOME/.vst3" "$HOME/.lv2"

if [ -d "$BUILD_DIR/VST3/$PLUGIN.vst3" ]; then
    rm -rf "$HOME/.vst3/$PLUGIN.vst3"
    cp -r "$BUILD_DIR/VST3/$PLUGIN.vst3" "$HOME/.vst3/"
    echo "Updated $HOME/.vst3/$PLUGIN.vst3"
else
    echo "VST3 not built" >&2
fi

if [ -d "$BUILD_DIR/LV2/$PLUGIN.lv2" ]; then
    rm -rf "$HOME/.lv2/$PLUGIN.lv2"
    cp -r "$BUILD_DIR/LV2/$PLUGIN.lv2" "$HOME/.lv2/"
    echo "Updated $HOME/.lv2/$PLUGIN.lv2"
else
    echo "LV2 not built" >&2
fi

echo "Rescan plugins in your DAW."
