#! /bin/bash
flatpak install com.obsproject.Studio.Plugin.OBSPWVideo
addr=$(flatpak info --show-location com.obsproject.Studio.Plugin.OBSPWVideo)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sudo cp -f "${SCRIPT_DIR}/obs-pwvideo.so" "${addr}/files/lib/obs-plugins/obs-pwvideo.so"
sudo cp -f "${SCRIPT_DIR}/obs-pwvideo/locale/en-US.ini" "${addr}/files/share/obs/obs-plugins/obs-pwvideo/locale/en-US.ini"
