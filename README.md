# ESP32 Nightscout SGV Display

ESP32-2432S028 (CYD) 用の PlatformIO プロジェクトです。Nightscout から
CGM の SGV データを取得し、320x240 のディスプレイに表示します。

## 機能

- SGV 値を血糖値に応じた色で表示し、トレンド矢印と経過時間を表示
- 画面下部を1分ごとに切り替え、時計、HbA1c 推定値、温度・湿度を表示
- HbA1c は2週推定値を白、1か月推定値を青で表示
- DHT22 の温度・湿度を表示（GPIO 22）
- 日本標準時を NTP で取得

## 必要なもの

- ESP32-2432S028 / CYD (ILI9341)
- PlatformIO Core または VS Code の PlatformIO 拡張
- Wi-Fi で接続できる Nightscout サーバー
- （任意）DHT22 温湿度センサー

## 設定

書き込み前に [`include/config.h`](include/config.h) を編集してください。
この公開版では Wi-Fi 情報、Nightscout のホスト名、アクセストークンを
空欄にしています。

- `WIFI_SSID` / `WIFI_PASSWORD`: Wi-Fi の接続情報
- `NIGHTSCOUT_HOST`: Nightscout サーバーの IP アドレスまたはホスト名
- `NIGHTSCOUT_PORT`: サーバーのポート（既定値 `1337`）
- `NIGHTSCOUT_USE_HTTPS`: HTTPS を使う場合は `1`
- `NIGHTSCOUT_TOKEN`: サーバーで必要な場合のみ設定
- `DHT_PIN`: DHT22 のデータピン（既定値 `22`）

設定ファイルに実際のパスワードやトークンを入力した状態で、公開リポジトリへ
コミットしないでください。

## 別のCYDモデルを使う場合

CYD はモデルや基板リビジョンによって、液晶コントローラー、解像度、GPIO配線、
バックライト極性などが異なる場合があります。基板の表記とメーカーの配線図を
確認し、以下を合わせてください。別モデルの設定をそのまま流用しないでください。

1. `platformio.ini` の `[env:esp32-2432s028]` で `board` を搭載ESP32に合わせます。
2. `build_flags` の画面ドライバー（現在は `-DILI9341_2_DRIVER=1`）を液晶に
   合わせ、ドライバー指定は1つだけにします。
3. 同じ `build_flags` の `TFT_WIDTH` / `TFT_HEIGHT` と
   `TFT_MISO` / `TFT_MOSI` / `TFT_SCLK` / `TFT_CS` / `TFT_DC` / `TFT_RST` を、
   液晶の仕様と実際の配線に合わせます。バックライトの `TFT_BL` と
   `TFT_BACKLIGHT_ON` も基板に合わせてください。タッチ機能を使う場合は
   `TOUCH_CS` も確認します。
4. `src/main.cpp` の `DISPLAY_INVERT` と `tft.setRotation(1)` を確認します。
   色が反転する場合は反転設定を、上下左右が違う場合は回転値を調整してください。
   色の赤・青が入れ替わる場合は `platformio.ini` のRGB順設定も確認します。
5. 温湿度センサーの配線が異なる場合は `include/config.h` の `DHT_PIN` を変更します。

このアプリの画面レイアウトは横向き320x240を前提にしています。液晶の有効解像度が
異なるモデルでは、`TFT_WIDTH` / `TFT_HEIGHT` だけでなく `src/main.cpp` の描画座標や
画面領域も調整してください。変更後は `pio run` でビルドし、実機で表示・バックライト・
色・向きを確認してください。

## ビルドと書き込み

プロジェクトのルートで実行します。

```sh
pio run
pio run -t upload
```

依存ライブラリは `platformio.ini` に記載してあり、PlatformIO が取得します。
書き込み時は ESP32 を USB で接続してください。
