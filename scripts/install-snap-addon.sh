#!/bin/bash
# Install the prebuilt fcitx5 addon carried by this snap into the system
# fcitx5 directories (classic confinement; requires sudo for the copies).
set -e

if [ -z "${SNAP:-}" ]; then
    echo "本脚本仅用于 snap 安装包。用法：" >&2
    echo "  sudo snap install fcitx5-voiceinput --classic" >&2
    echo "  fcitx5-voiceinput.install-addon" >&2
    exit 1
fi

SO=$(ls "$SNAP"/usr/lib/*/fcitx5/fcitx5-voiceinput.so 2>/dev/null | head -n1)
CONF="$SNAP/usr/share/fcitx5/addon/voiceinput.conf"
BACKEND="$SNAP/usr/share/fcitx5/voiceinput/voice_backend.py"
MO="$SNAP/usr/share/locale/zh_CN/LC_MESSAGES/fcitx5-voiceinput.mo"
for f in "$SO" "$CONF" "$BACKEND"; do
    [ -f "$f" ] || { echo "错误：snap 内缺少 $f" >&2; exit 1; }
done

# Target addon dir: prefer pkg-config (exact, needs the -dev package),
# then glob the multiarch libdirs actually present on this machine.
ADDON_DIR="$(pkg-config --variable=libdir Fcitx5Core 2>/dev/null)/fcitx5"
if [ ! -d "$ADDON_DIR" ]; then
    ADDON_DIR=$(ls -d /usr/lib/*-linux-gnu*/fcitx5 2>/dev/null | head -n1 \
        || true)
fi
if [ -z "$ADDON_DIR" ] || [ ! -d "$ADDON_DIR" ]; then
    echo "错误：未找到 fcitx5 插件目录，请先安装 fcitx5" >&2
    exit 1
fi

echo "fcitx5 插件目录: $ADDON_DIR"
sudo install -D -m 644 "$SO" "$ADDON_DIR/fcitx5-voiceinput.so"
sudo install -D -m 644 "$CONF" /usr/share/fcitx5/addon/voiceinput.conf
sudo install -D -m 755 "$BACKEND" \
    /usr/share/fcitx5/voiceinput/voice_backend.py
if [ -f "$MO" ]; then
    sudo install -D -m 644 "$MO" \
        /usr/share/locale/zh_CN/LC_MESSAGES/fcitx5-voiceinput.mo
fi

echo
echo "安装完成。请重启 fcitx5 生效："
echo "  fcitx5 -r"
