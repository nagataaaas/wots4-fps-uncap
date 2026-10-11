# fps_uncap

『侍道4』の 30FPS の上限を外す Mod です。

**まだ中身はありません。** 入口だけがあり、Release には入れていません。

## 予定

- ふだんのプレイは 60FPS、イベントの場面は 30FPS にする
- ゲームのファイルは書き換えず、メモリの上だけで変える

## ビルド

ふつうは [wots4-mods](https://github.com/nagataaaas/wots4-mods) の中でビルドします。この Mod だけをビルドするときは、`-DWOTS4_SDK_DIR=<wots4-mods のパス>` を付けて CMake を実行します。
