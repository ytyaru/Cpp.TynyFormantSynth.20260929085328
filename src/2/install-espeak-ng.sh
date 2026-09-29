#!/bin/bash
set -e

# ==========================================
# 設定項目の定義
# ==========================================
MAX_JOBS=2

echo "=== [1/7] 依存パッケージおよび MBROLA のインストール ==="
sudo apt-get update
sudo apt-get install -y git make automake libtool g++ pkg-config libpcaudio-dev mbrola

echo "=== [2/7] 日本語MBROLA音声データのインストール (jp1, jp2, jp3) ==="
sudo apt-get install -y mbrola-jp1 mbrola-jp2 mbrola-jp3

echo "=== [3/7] eSpeak-NG 最新安定版タグの動的取得 ==="
echo "GitHubリポジトリから最新の安定版タグを検索中..."

# 1. リモートのタグ一覧を取得
# 2. バージョン形式 (X.Y.Z) のみに絞り込み
# 3. バージョン番号として自然ソート (sort -V) し、一番最後の行（最新）を取得
ESPEAK_VERSION=$(git ls-remote --tags --refs https://github.com/espeak-ng/espeak-ng.git | awk -F/ '{print $3}' | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' | sort -V | tail -n 1)

if [ -z "$ESPEAK_VERSION" ]; then
    echo "エラー: 最新安定版タグの取得に失敗しました。"
    exit 1
fi

echo "-> 検出された最新安定版タグ: ${ESPEAK_VERSION}"

echo "=== [4/7] 指定タグ (${ESPEAK_VERSION}) のクローンとチェックアウト ==="
if [ -d "espeak-ng" ]; then
    rm -rf espeak-ng
fi

git clone --depth 1 --branch ${ESPEAK_VERSION} https://github.com/espeak-ng/espeak-ng.git
cd espeak-ng

echo "=== [5/7] ビルド環境のセットアップ (MBROLA対応有効化) ==="
./autogen.sh
./configure --with-mbrola

echo "=== [6/7] コンパイル実行 (CPUコア数: ${MAX_JOBS}) ==="
make -j${MAX_JOBS}
sudo make install
sudo ldconfig

echo "=== [7/7] MBROLA連携の動作確認テスト ==="
echo "MBROLA (mb-jp1) を使用して音声ファイル(test_mb_jp1.wav)を出力します..."
TXT="こんにちは、いーすぴーくえぬじーで、えむびーろーらをつかった、にほんごおんせいてすとです。"
#espeak-ng -v mb/mb-jp1 -w test_mb_jp1.wav "こんにちは、MBROLA連携による日本語音声テストです。"
espeak-ng -v mb/mb-jp1 -w test_mb_jp1.wav "$TXT"
espeak-ng -v mb/mb-jp1 "$TXT"
espeak-ng jpx/ja-jp1 "$TXT"

if [ -f "test_mb_jp1.wav" ]; then
    echo "=================================================="
    echo " 成功: MBROLA対応 eSpeak-NG (${ESPEAK_VERSION}) のビルドと音声生成が完了しました！"
    echo " 成果物: $(pwd)/test_mb_jp1.wav"
    echo "=================================================="
else
    echo "エラー: MBROLA音声ファイルの生成に失敗しました。"
    exit 1
fi
