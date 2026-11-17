# Rec Menu / Rec Gallery

Nintendo Switch homebrew capture project.

> **Project status:** Rec Menu (OVL) and Rec Gallery (NRO) are separate components. The NRO gallery is a separate application and is not included in the OVL. Features described below should be considered planned until they are implemented and tested in a release.

## Components

### 1. Rec Menu (Tesla Overlay — `.ovl`)

Rec Menu is the Tesla overlay component. It is launched from Tesla / UltraHand and is intended for in-game capture controls.

Planned features:
- Start and stop gameplay recording.
- Show a compact recording timer at the top-right of the game screen, including after closing the UltraHand menu (requires a persistent on-screen display implementation).
- Take screenshots.
- Save captured media to the SD card.

### 2. Rec Gallery (Homebrew app — `.nro`)

Rec Gallery is a separate homebrew application, launched from the Homebrew Menu. It is not launched from the Tesla overlay menu.

Planned features:
- Browse saved screenshots and videos.
- Preview supported image files.
- Play supported video files.
- Delete selected media files.

## SD card installation

Copy each file to its own location. Do not rename the file extensions.

```text
SD card/
└── switch/
    ├── .overlays/
    │   └── RecMenu.ovl
    ├── RecGallery/
    │   └── RecGallery.nro
    └── rec-menu/
        └── shot_00000.jpg
```

### Install Rec Menu (`.ovl`)

1. On the SD card, open `/switch/`.
2. Create a folder named `.overlays` if it does not already exist.
3. Copy `RecMenu.ovl` into `/switch/.overlays/`.
4. Open Tesla / UltraHand and select Rec Menu from the overlay list.

### Install Rec Gallery (`.nro`)

1. On the SD card, open `/switch/`.
2. Create a folder named `RecGallery`.
3. Copy `RecGallery.nro` into `/switch/RecGallery/`.
4. Open the Homebrew Menu and launch Rec Gallery.

The NRO is a standalone application. It is separate from the OVL and must be installed separately. The gallery will only be usable once a Rec Gallery NRO release is provided.

## Capture storage

The current screenshot code uses:

`sdmc:/switch/rec-menu/shot_00000.jpg`

New captures use incrementing filenames to avoid overwriting existing screenshots. The gallery and recorder should use this same directory. Video subfolders or alternate paths should only be documented after they are implemented.

No Wi-Fi connection or smartphone transfer is required; media remains on the SD card.

## Requirements

- Nintendo Switch with a compatible custom firmware / homebrew environment.
- Tesla / UltraHand for the OVL component.
- Homebrew Menu for the NRO component.
- SD card.

## 日本語

# Rec Menu / Rec Gallery

Nintendo Switch向けのキャプチャ関連自作ソフトプロジェクトです。

> **開発状況：** Rec Menu（OVL）とRec Gallery（NRO）は別々のコンポーネントです。NRO版ギャラリーはOVLとは別の独立アプリであり、OVLに含まれるものではありません。以下の機能は、リリースで実装・動作確認されるまでは予定機能として扱ってください。

## コンポーネント

### 1. Rec Menu（Teslaオーバーレイ — `.ovl`）

Tesla / UltraHandから起動するオーバーレイです。ゲーム中のキャプチャ操作を担当します。

予定機能：
- ゲーム録画の開始・停止。
- UltraHandのメニューを閉じたあとも、ゲーム画面右上に小さな録画タイマーを表示（常時表示には専用の表示処理が必要です）。
- スクリーンショット撮影。
- キャプチャデータのSDカード保存。

### 2. Rec Gallery（自作アプリ — `.nro`）

Homebrew Menuから起動する独立したアプリです。Teslaオーバーレイから起動するものではありません。

予定機能：
- 保存した画像・動画の一覧表示。
- 対応画像のプレビュー。
- 対応動画の再生。
- 選択したメディアファイルの削除。

## SDカードへの配置方法

OVLとNROは、それぞれ別のフォルダに配置します。拡張子は変更しないでください。

```text
SDカード/
└── switch/
    ├── .overlays/
    │   └── RecMenu.ovl
    ├── RecGallery/
    │   └── RecGallery.nro
    └── rec-menu/
        └── shot_00000.jpg
```

### Rec Menu（.ovl）の導入

1. SDカード内の `/switch/` フォルダを開きます。
2. `.overlays` フォルダがなければ作成します（先頭のドットも含めてください）。
3. `RecMenu.ovl` を `/switch/.overlays/` にコピーします。
4. Tesla / UltraHandを開き、オーバーレイ一覧からRec Menuを選択します。

### Rec Gallery（.nro）の導入

1. SDカード内の `/switch/` フォルダを開きます。
2. `RecGallery` フォルダを作成します。
3. `RecGallery.nro` を `/switch/RecGallery/` にコピーします。
4. Homebrew Menuを開き、Rec Galleryを起動します。

NROはOVLとは別の独立アプリです。別々に配置・起動する必要があります。Rec GalleryのNROがリリースされるまでは、ギャラリー機能は利用できません。

## キャプチャデータの保存先

現在のスクリーンショットコードの保存先：

`sdmc:/switch/rec-menu/shot_00000.jpg`

既存ファイルを上書きしないよう、撮影ごとに連番を使用します。ギャラリーと録画機能も同じフォルダを参照する設計にします。動画用サブフォルダや別の保存先は、実装後にREADMEへ追記します。

Wi-Fi接続やスマートフォンへの転送は必要ありません。メディアはSDカード内に保存します。

## 必要なもの

- 対応するカスタムファームウェア／自作ソフト環境のNintendo Switch本体
- OVL版にはTesla / UltraHand
- NRO版にはHomebrew Menu
- SDカード
