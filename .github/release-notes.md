**[English](#english)** | **[日本語](#日本語)**

---

## English

Record your Switch gameplay to MP4 and watch it on the console. No PC needed.

- **Rec Menu** (Tesla / Ultrahand overlay): records gameplay video and audio, and takes screenshots
- **Rec Gallery** (Homebrew app): lists, plays and deletes your recordings and screenshots, a bit like the Album

### Download

Each app comes in an **English** and a **Japanese** version. They work the same; only the on-screen text differs.

| File | What it is |
|---|---|
| **`Rec-Menu-and-Gallery-en.zip`** | **English version of both apps — recommended.** Extract it to the root of your SD card. |
| `Rec-Menu-and-Gallery-ja.zip` | Japanese version of both apps. |
| `RecMenu-en.ovl` / `RecMenu-ja.ovl` | Rec Menu only |
| `RecGallery-en.nro` / `RecGallery-ja.nro` | Rec Gallery only |
| `symbols-debug.zip` | Debug symbols (`.elf`) for reading crash reports. You don't need this to use the apps. |

### Install

1. Extract `Rec-Menu-and-Gallery-en.zip` to the root of your SD card. It contains:
   ```text
   switch/.overlays/RecMenu.ovl
   switch/RecGallery.nro
   ```
2. **If you have an older Rec Menu**, delete its old `.ovl` from `switch/.overlays/` first. Otherwise both appear in the Ultrahand list and it's easy to open the old one.
   Likewise, if you put an older Rec Gallery in another folder (for example `switch/RecGallery/`), delete it so only one shows up in the Homebrew Menu.
3. Recordings and screenshots are saved to `switch/rec-menu/`.

**Requirements:** a Switch running Atmosphère, and Ultrahand (or Tesla) for Rec Menu.

### How to use

- **Record:** while a game is running, open the overlay (default: **L + D‑pad Down + Right Stick press**) → **Record** → **A**. The menu closes and recording starts. Open the menu again and press **A** to stop.
- **Watch:** start Rec Gallery from the Homebrew Menu. It also works with the touch screen. For smoother playback, launch it through a game (hold **R** while starting a game) instead of from the Album.

### Good to know

- **Only games can be recorded** (1280×720, 30 fps, with audio). The HOME Menu can't be recorded, because Rec Menu uses the same video as the console's built-in capture. Recording stops automatically when you go to the HOME Menu, put the console to sleep, or quit the game.
- **Audio is uncompressed PCM** (about 11 MB per minute). It plays in VLC and in Rec Gallery, but some phone players may not play the sound.
- **Can't be used together with SysDVR.**
- **The top-right timer and bottom-left notifications are experimental.** Overlays get very little memory (4 MB on recent firmware). If **Notification display** in the menu doesn't say `OK`, they won't appear, but recording still works.
- Use at your own risk.

---

## 日本語

Switchのゲームを録画してMP4で保存し、本体で見られるソフトです。パソコンは必要ありません。

- **Rec Menu**（Tesla / Ultrahandのオーバーレイ）：ゲームの映像と音声の録画、スクリーンショット
- **Rec Gallery**（ホームブリューのアプリ）：録画した動画やスクショを、アルバムのように一覧・再生・削除

### ダウンロード

どちらのアプリも **英語版** と **日本語版** があります。機能は同じで、画面に出る文字だけが違います。

| ファイル | 中身 |
|---|---|
| **`Rec-Menu-and-Gallery-ja.zip`** | **2つのアプリの日本語版。おすすめです。** SDカードの一番上に展開してください。 |
| `Rec-Menu-and-Gallery-en.zip` | 2つのアプリの英語版 |
| `RecMenu-ja.ovl` / `RecMenu-en.ovl` | Rec Menu だけ |
| `RecGallery-ja.nro` / `RecGallery-en.nro` | Rec Gallery だけ |
| `symbols-debug.zip` | クラッシュログを調べるためのデバッグ情報（`.elf`）。使うだけなら不要です。 |

### 入れ方

1. `Rec-Menu-and-Gallery-ja.zip` を、SDカードの一番上（ルート）に展開します。中身は次の2つです。
   ```text
   switch/.overlays/RecMenu.ovl
   switch/RecGallery.nro
   ```
2. **前の版のRec Menuが入っている場合は**、先に `switch/.overlays/` の古い `.ovl` を消してください。残っていると、Ultrahandの一覧に両方並んで、古い方を開いてしまいやすくなります。
   Rec Galleryも、前の版を別の場所（`switch/RecGallery/` など）に入れていた場合は消してください。ホームブリューメニューに2つ並んでしまいます。
3. 録画した動画とスクショは `switch/rec-menu/` に保存されます。

**必要なもの：** Atmosphèreで動いているSwitch。Rec Menuを使うにはUltrahand（またはTesla）も必要です。

### 使い方

- **録画：** ゲーム中にオーバーレイを開き（初期設定は **L＋十字キー下＋右スティック押し込み**）、**録画** で **A** を押します。メニューが閉じて録画が始まります。止めるときは、もう一度メニューを開いて **A** を押します。
- **見る：** ホームブリューメニューからRec Galleryを起動します。タッチでも操作できます。アルバムから起動するより、ゲームのアイコンで **R** を押しながら起動したほうが、再生が滑らかになります。

### 注意点

- **録画できるのはゲームの画面だけです**（1280×720・30fps・音声あり）。本体の撮影機能と同じ映像を使っているので、ホーム画面は録画できません。ホーム画面に移ったとき、スリープしたとき、ゲームを終了したときは、録画が自動で止まります。
- **音声は圧縮していないPCMです**（1分あたり約11MB）。VLCやRec Galleryでは鳴りますが、スマホのプレーヤーによっては音が出ないことがあります。
- **SysDVRとは同時に使えません。**
- **右上のタイマーと左下の通知は試験的な機能です。** オーバーレイで使えるメモリは少なく、最近のファームウェアでは4MBしかありません。メニューの **通知の表示** が `OK` になっていないときは表示されませんが、録画はそのままできます。
- 使用は自己責任でお願いします。
