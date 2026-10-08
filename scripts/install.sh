#!/usr/bin/env bash
# fcitx5-voiceinput installer: deps -> build -> install -> models
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "==> [1/4] 安装系统依赖（需要 sudo）"
MISSING=()
for pkg in cmake g++ pkg-config libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev; do
    if ! dpkg -s "$pkg" >/dev/null 2>&1; then MISSING+=("$pkg"); fi
done
if [ ${#MISSING[@]} -gt 0 ]; then
    sudo apt-get update
    sudo apt-get install -y "${MISSING[@]}"
else
    echo "    系统依赖已就绪"
fi

echo "==> [2/4] 安装 Python 依赖与构建安装插件"
python3 -c "import sherpa_onnx" 2>/dev/null || pip3 install --user sherpa-onnx
python3 -c "import numpy" 2>/dev/null || pip3 install --user numpy

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
sudo cmake --install build

echo "==> [3/4] 安装中文界面翻译"
if command -v msgfmt >/dev/null 2>&1; then
    sudo mkdir -p /usr/share/locale/zh_CN/LC_MESSAGES
    sudo msgfmt po/zh_CN.po -o /usr/share/locale/zh_CN/LC_MESSAGES/fcitx5-voiceinput.mo
else
    echo "    未找到 msgfmt，跳过（界面将显示英文）"
fi

echo "==> [4/4] 检查依赖并下载模型（约 240 MB）"
python3 backend/voice_backend.py doctor

cat <<'EOF'

安装完成！使用方法：
  1. 重启 fcitx5：        fcitx5 -r  （或注销重新登录）
  2. 在任意输入框按快捷键 Ctrl+Alt+V 开始语音输入，再次按下或停顿后自动上屏
  3. 修改快捷键/识别语言：fcitx5-configtool -> 附加组件 -> 语音输入
  4. 重新自检：            python3 /usr/share/fcitx5/voiceinput/voice_backend.py doctor
EOF
