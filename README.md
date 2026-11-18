# Rec Menu / Rec Gallery

**[English](#english)** | **[日本語](#日本語)**

---

## English

Homebrew tools for recording gameplay on a Nintendo Switch running custom firmware, then watching the recordings on the console.

| Component | Type | What it does |
|---|---|---|
| **Rec Menu** | Tesla overlay (`.ovl`) | Records gameplay video and audio to MP4 and takes screenshots. You open it from Ultrahand / Tesla while a game is running. |
| **Rec Gallery** | Homebrew app (`.nro`) | Lists, plays and deletes your recordings and screenshots, a bit like the Album. You launch it from the Homebrew Menu. |

Everything is saved to the SD card. No PC, Wi‑Fi or phone is needed.

---

### Features

#### Rec Menu (overlay)

- **Records gameplay to MP4**: 1280×720 / 30 fps H.264 video with 48 kHz stereo audio, saved straight to the SD card.
  - The file is written in small fragments, so if the console crashes or loses power, everything up to that point still plays.
- **Timer while recording**: "● REC 01:23" appears at the top-right of the screen. It stays there after you close the menu, and you can keep playing normally.
- **Notifications** at the bottom-left of the screen when:
  - recording starts or stops
  - a screenshot is saved
  - an error occurs
- **Stops recording automatically** and shows a notification when you:
  - press HOME (go to the HOME Menu)
  - put the console to sleep or turn it off
  - quit the game (the game video stops)
- **Takes screenshots**: the menu closes before the screenshot is taken, so the menu does not appear in the image.
- The timer, the notifications and the Tesla menu are kept out of the recorded video and the screenshots.

#### Rec Gallery (app)

- **Two tabs, Videos and Photos**: each shows thumbnails with the date and time it was taken.
- **Sorting**: newest first, oldest first, largest first, or smallest first.
- **Video player**: pause, skip ±10 s / ±30 s, and a progress bar.
- **Photo viewer**: full screen, step to the previous or next photo.
- **Delete**: asks you to confirm first.

---

### Installation

Copy the files to these locations on the SD card. Keep the file extensions as they are.

```text
SD card/
└── switch/
    ├── .overlays/
    │   └── RecMenu-vX.Y.Z.ovl      ← Rec Menu (the file name can be anything)
    ├── RecGallery/
    │   └── RecGallery.nro          ← Rec Gallery
    └── rec-menu/                   ← created automatically; recordings and screenshots go here
        ├── rec_00000.mp4
        └── shot_00000.jpg
```

1. Create `/switch/.overlays/` if it does not exist (the name starts with a dot), and copy the `.ovl` file into it.
2. Create `/switch/RecGallery/` and copy `RecGallery.nro` into it.
3. **When you update Rec Menu, delete the old `.ovl` first.** If old and new copies are both in `.overlays`, they show up side by side in the Ultrahand list and it is easy to open the old one. Check the version number shown in the Ultrahand list and in the top-right of the menu.

#### Requirements

- A Nintendo Switch running custom firmware (Atmosphère)
- **Rec Menu**: Ultrahand (or Tesla)
- **Rec Gallery**: the Homebrew Menu. For smoother playback, launch it through a game (hold **R** while starting a game) rather than from the Album; Album (applet) mode gets less memory and CPU.

---

### Controls

#### Rec Menu

Open the overlay with the Ultrahand button combo (default: **L + D‑pad Down + Right Stick press**).

| Item | Action |
|---|---|
| **Record** | **A** starts recording. The menu closes and you go back to the game. Open the menu again and press **A** to stop. |
| **Show timer (top right)** | Turns the recording timer on or off. |
| **Notification display** | Shows whether the timer and notifications are working (`OK`, or which step failed). |
| **Screenshot** | **A** closes the menu and saves a screenshot. |

While recording, **B** only hides the menu. It does not stop the recording.

#### Rec Gallery

| Screen | Button | Action |
|---|---|---|
| List | D‑pad / Left Stick | Select an item |
| | **A** | Play the video / open the photo |
| | **X** | Delete (asks to confirm) |
| | **L / R** | Switch between Videos and Photos |
| | **−** | Change the sort order |
| | **Y** | Reload the list |
| | **+ / B** | Exit |
| Video | **A** | Pause / resume |
| | **← / →** | Back / forward 10 seconds |
| | **L / R** | Back / forward 30 seconds |
| | **B** | Back to the list |
| Photo | **← / →** (or L / R) | Previous / next photo |
| | **X** | Delete |
| | **B** | Back to the list |

---

### Limitations

- **Only games can be recorded.** Rec Menu uses the console's built-in game video capture stream (the same video the 30‑second capture button uses), so the HOME Menu and other system screens cannot be recorded. Some games that do not allow video capture cannot be recorded either.
- **Audio is uncompressed PCM** (about 11 MB per minute). The audio plays in VLC on PC and in Rec Gallery, but some phone video players may not play it.
- **Leaving Rec Menu stops the recording.** Hiding the menu is fine, but if you go back to the Ultrahand overlay list or open another overlay, Rec Menu closes and the recording stops (everything up to that point is saved).
- **Cannot be used with SysDVR.** Both use the same video stream. Rec Menu shows an error if SysDVR is running.
- **The timer and notifications are experimental.** Overlays only get a small amount of memory (4 MB on recent firmware). If **Notification display** shows something other than `OK`, the timer and notifications are unavailable, but recording still works.
- **Rec Gallery plays videos in software (CPU only).** Playback may stutter depending on conditions.

---

### Building

You do not need a PC to build. GitHub Actions builds both components on every push.

- Workflow: `.github/workflows/build.yml`
  - **build-ovl**: builds Rec Menu with libtesla from [Tesla-Template](https://github.com/WerWolv/Tesla-Template). A small patch to libtesla adds the HOME button and shutdown hooks and keeps the overlay out of recordings.
  - **gallery**: builds Rec Gallery with devkitPro `switch-ffmpeg`, `switch-sdl2`, `switch-sdl2_ttf` and `switch-sdl2_image`.
- Artifacts:
  - `ovl` (`tpl.ovl` + `tpl.elf`)
  - `nro` (`RecGallery.nro` + `RecGallery.elf`)

  The `.elf` files are debug symbols for reading crash reports.

#### Reading a crash report

If the console shows a crash report (`/atmosphere/crash_reports/*.log`), look up the `tpl + 0x…` addresses with `aarch64-none-elf-addr2line -f -C -e tpl.elf 0x…`. Use the `.elf` from the same build that crashed.

---

### Source layout

```text
source/            Rec Menu (overlay)
  main.cpp           Menu UI
  recorder.hpp       Recording (grc:d), screenshots, stopping on sleep/HOME, monitor thread
  mp4.hpp            Fragmented MP4 writer (H.264 + PCM)
  osd.hpp            Timer and notification display (separate layer from Tesla)
gallery/           Rec Gallery (NRO)
  source/main.cpp    List, video playback, photo viewer, delete
  icon.jpg           App icon
```

### Credits

- [SysDVR](https://github.com/exelix11/SysDVR) by exelix11: reference for the safe way to use `grc:d` and for the SPS/PPS values
- [libtesla](https://github.com/WerWolv/libtesla) / [Tesla-Template](https://github.com/WerWolv/Tesla-Template) by WerWolv
- [Ultrahand Overlay](https://github.com/ppkantorski/Ultrahand-Overlay) / nx-ovlloader
- [libnx](https://github.com/switchbrew/libnx), [devkitPro](https://devkitpro.org/), FFmpeg, SDL2

Use at your own risk.

---

## 日本語

カスタムファームウェアで動いているNintendo Switchで、ゲームを録画して、その動画を本体で見るための自作ソフトです。

| ソフト | 種類 | できること |
|---|---|---|
| **Rec Menu** | Teslaオーバーレイ（`.ovl`） | ゲームの映像と音声をMP4で録画したり、スクリーンショットを撮ったりします。ゲーム中にUltrahand / Teslaから開きます。 |
| **Rec Gallery** | 自作アプリ（`.nro`） | 録画した動画やスクリーンショットを、アルバムのように一覧・再生・削除します。ホームブリューメニューから起動します。 |

保存先はすべてSDカードです。パソコン、Wi‑Fi、スマホは必要ありません。

---

### 機能

#### Rec Menu（オーバーレイ）

- **ゲームをMP4で録画**：映像は1280×720・30fpsのH.264、音声は48kHzのステレオで、SDカードに直接保存します。
  - 少しずつ区切って書き込んでいるので、途中で本体が落ちたり電源が切れたりしても、そこまでの分は再生できます。
- **録画中のタイマー**：画面右上に「● REC 01:23」と出ます。メニューを閉じても出たままで、ゲームはそのまま操作できます。
- **通知**：次のときに、画面左下に通知が出ます。
  - 録画を開始・停止したとき
  - スクショを保存したとき
  - エラーが起きたとき
- **自動で録画を停止**：次のときは録画を自動で止めて、通知を出します。
  - HOMEボタンを押してホーム画面に移ったとき
  - スリープしたとき、電源を切ったとき
  - ゲームを終了したとき（ゲームの映像が止まったとき）
- **スクリーンショット**：メニューを閉じてから撮るので、メニューは写りません。
- タイマー・通知・Teslaのメニューは、録画した動画にもスクショにも写りません。

#### Rec Gallery（アプリ）

- **動画と写真のタブ**：それぞれサムネイルと撮影日時が一覧で出ます。
- **並び替え**：新しい順・古い順・大きい順・小さい順を切り替えられます。
- **動画の再生**：一時停止、10秒・30秒の早送りと巻き戻し、再生位置のバーがあります。
- **写真の表示**：全画面で表示して、前後の写真に移動できます。
- **削除**：削除する前に確認が出ます。

---

### 入れ方

SDカードの次の場所にファイルを置いてください。拡張子は変えないでください。

```text
SDカード/
└── switch/
    ├── .overlays/
    │   └── RecMenu-vX.Y.Z.ovl      ← Rec Menu（ファイル名は自由です）
    ├── RecGallery/
    │   └── RecGallery.nro          ← Rec Gallery
    └── rec-menu/                   ← 自動で作られます。動画とスクショはここに保存されます
        ├── rec_00000.mp4
        └── shot_00000.jpg
```

1. `/switch/.overlays/` フォルダがなければ作ってください（名前の先頭にドットが付きます）。そこに `.ovl` ファイルを入れます。
2. `/switch/RecGallery/` フォルダを作って、`RecGallery.nro` を入れます。
3. **Rec Menuを新しい版にするときは、先に古い `.ovl` を消してください。** 古いファイルと新しいファイルが両方 `.overlays` にあると、Ultrahandの一覧に並んで表示され、古い方を開いてしまいやすくなります。Ultrahandの一覧とメニューの右上にバージョンが出るので、それで確認してください。

#### 必要なもの

- カスタムファームウェア（Atmosphère）で動いているNintendo Switch本体
- **Rec Menu**：Ultrahand（またはTesla）
- **Rec Gallery**：ホームブリューメニュー。アルバムから起動するより、ゲームのアイコンで **R** を押しながら起動するのがおすすめです。アルバムから起動すると使えるメモリとCPUが少なくなり、再生がカクつきやすくなります。

---

### 操作方法

#### Rec Menu

Ultrahandを開くボタン（初期設定は **L＋十字キー下＋右スティック押し込み**）でメニューを開きます。

| 項目 | 操作 |
|---|---|
| **Record** | **A** で録画を開始します。メニューが閉じてゲームに戻ります。止めるときは、もう一度メニューを開いて **A** を押します。 |
| **Show timer (top right)** | 録画中のタイマーを表示するかどうかを切り替えます。 |
| **Notification display** | タイマーと通知が使えるかを表示します（`OK` か、どこで失敗したか）。 |
| **Screenshot** | **A** を押すとメニューが閉じて、スクショを保存します。 |

録画中に **B** を押しても、メニューが隠れるだけで録画は止まりません。

#### Rec Gallery

| 画面 | ボタン | 操作 |
|---|---|---|
| 一覧 | 十字キー / 左スティック | 選ぶ |
| | **A** | 動画を再生する / 写真を表示する |
| | **X** | 削除する（確認が出ます） |
| | **L / R** | 動画と写真を切り替える |
| | **−** | 並び順を変える |
| | **Y** | 一覧を読み直す |
| | **＋ / B** | 終了する |
| 動画 | **A** | 一時停止 / 再開 |
| | **← / →** | 10秒戻る / 進む |
| | **L / R** | 30秒戻る / 進む |
| | **B** | 一覧に戻る |
| 写真 | **← / →**（または L / R） | 前の写真 / 次の写真 |
| | **X** | 削除する |
| | **B** | 一覧に戻る |

---

### できないこと・注意点

- **録画できるのはゲームの画面だけです。** 本体の動画撮影機能（キャプチャボタン長押しの30秒動画）と同じ映像を使っているので、ホーム画面やシステムの画面は録画できません。動画撮影ができないゲームも録画できません。
- **音声は圧縮していないPCMです**（1分あたり約11MB）。パソコンのVLCやRec Galleryでは鳴りますが、スマホの動画プレーヤーによっては音が出ないことがあります。
- **Rec Menuを終了すると録画も止まります。** メニューを隠すだけなら録画は続きます。ただし、Ultrahandのオーバーレイ一覧に戻ったり、別のオーバーレイを開いたりするとRec Menu自体が終了するので、録画もそこで止まります（そこまでの分は保存されます）。
- **SysDVRとは同時に使えません。** 同じ映像を使うためです。SysDVRが動いているときは、録画を始めようとするとエラーが出ます。
- **タイマーと通知は試験的な機能です。** オーバーレイで使えるメモリは少なく、最近のファームウェアでは4MBしかありません。メニューの **Notification display** が `OK` 以外になっているときは、タイマーと通知は出ません。録画はそのままできます。
- **Rec GalleryはCPUだけで動画を再生しています。** 状況によっては再生がカクつくことがあります。

---

### ビルド方法

パソコンがなくてもビルドできます。GitHubにpushするたびに、GitHub Actionsが2つとも自動でビルドします。

- ワークフロー：`.github/workflows/build.yml`
  - **build-ovl**：[Tesla-Template](https://github.com/WerWolv/Tesla-Template) のlibteslaを使ってRec Menuをビルドします。その際、libteslaに小さなパッチを当てています（HOMEボタンと終了処理のフックを追加し、オーバーレイが録画に写らないようにする）。
  - **gallery**：devkitProの `switch-ffmpeg`、`switch-sdl2`、`switch-sdl2_ttf`、`switch-sdl2_image` を使ってRec Galleryをビルドします。
- 成果物：
  - `ovl`（`tpl.ovl` と `tpl.elf`）
  - `nro`（`RecGallery.nro` と `RecGallery.elf`）

  `.elf` はクラッシュログを調べるためのデバッグ情報です。

#### クラッシュログの調べ方

本体が落ちてクラッシュログ（`/atmosphere/crash_reports/*.log`）が出たときは、その中の `tpl + 0x…` という番号を `aarch64-none-elf-addr2line -f -C -e tpl.elf 0x…` で調べると、落ちた場所がわかります。落ちたときと同じビルドの `.elf` を使ってください。

---

### ソースの構成

```text
source/            Rec Menu（オーバーレイ）
  main.cpp           メニュー画面
  recorder.hpp       録画（grc:d）、スクショ、スリープ・HOMEでの停止、監視スレッド
  mp4.hpp            MP4（fragmented MP4）の書き出し（H.264＋PCM）
  osd.hpp            タイマーと通知の表示（Teslaとは別のレイヤー）
gallery/           Rec Gallery（NRO）
  source/main.cpp    一覧、動画再生、写真表示、削除
  icon.jpg           アプリのアイコン
```

### 参考にしたもの・使っているもの

- exelix11氏の [SysDVR](https://github.com/exelix11/SysDVR)：`grc:d` を安全に使う方法と、SPS/PPSの値を参考にしました
- WerWolv氏の [libtesla](https://github.com/WerWolv/libtesla) / [Tesla-Template](https://github.com/WerWolv/Tesla-Template)
- [Ultrahand Overlay](https://github.com/ppkantorski/Ultrahand-Overlay) / nx-ovlloader
- [libnx](https://github.com/switchbrew/libnx)、[devkitPro](https://devkitpro.org/)、FFmpeg、SDL2

使用は自己責任でお願いします。
