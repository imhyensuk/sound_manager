#!/bin/bash
# Sound Manager AI - Logic Pro installer (double-click in Finder, or run in Terminal).
# Copies the Audio Unit to ~/Library/Audio/Plug-Ins/Components, removes the download
# quarantine, signs it locally (ad-hoc) and makes Logic rescan its Audio Units.
set -e
cd "$(dirname "$0")"

COMP="Sound Manager AI.component"
DEST="$HOME/Library/Audio/Plug-Ins/Components"
DATA="$HOME/Library/Application Support/SoundManagerAI"

if [ ! -d "$COMP" ]; then
    echo "\"$COMP\" was not found next to this script."
    exit 1
fi

mkdir -p "$DEST" "$DATA/models"
rm -rf "$DEST/$COMP"
cp -R "$COMP" "$DEST/"
xattr -dr com.apple.quarantine "$DEST/$COMP" 2>/dev/null || true
codesign --force --deep --sign - "$DEST/$COMP"

if [ -d "Sound Manager AI.vst3" ]; then
    mkdir -p "$HOME/Library/Audio/Plug-Ins/VST3"
    rm -rf "$HOME/Library/Audio/Plug-Ins/VST3/Sound Manager AI.vst3"
    cp -R "Sound Manager AI.vst3" "$HOME/Library/Audio/Plug-Ins/VST3/"
    xattr -dr com.apple.quarantine "$HOME/Library/Audio/Plug-Ins/VST3/Sound Manager AI.vst3" 2>/dev/null || true
    codesign --force --deep --sign - "$HOME/Library/Audio/Plug-Ins/VST3/Sound Manager AI.vst3"
fi

# Make the system (and Logic) see the new Audio Unit.
killall -9 AudioComponentRegistrar 2>/dev/null || true
echo "Validating the Audio Unit (auval)..."
if auval -v aufx Smx1 Smgr > /tmp/smix-auval.txt 2>&1; then
    echo "OK: AU validation passed."
else
    echo "AU validation FAILED - see /tmp/smix-auval.txt"
fi

echo
echo "Installed. Put your model files into:"
echo "  $DATA/models"
echo "Then start Logic Pro (Plug-in Manager: 'Reset & Rescan Selection' if it was open)."
open "$DATA/models" 2>/dev/null || true
