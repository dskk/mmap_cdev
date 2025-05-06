#!/bin/bash

# 引数が1つ以上あるか確認
if [ $# -lt 1 ]; then
  echo "使い方: $0 <実行ファイル名> [引数...]"
  exit 1
fi

# 最初の引数を実行ファイル名として取得
executable="$1"

# ファイルが存在するか確認
if [ ! -f "$executable" ]; then
  echo "エラー: '$executable' は存在しません。"
  exit 1
fi

# 実行可能でない場合にエラーメッセージを表示
if [ ! -x "$executable" ]; then
  echo "エラー: '$executable' は実行可能なファイルではありません。"
  exit 1
fi

insmod drv/dynmmapdev.ko
"$executable" "$@"
rmmod dynmmapdev
