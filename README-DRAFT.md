# Rec Menu & Rec Gallery

> **A Nintendo Switch homebrew capture project.**  
> Capture. Save. Replay.

This document is a draft of the planned project README. Feature descriptions below represent the intended release functionality and should be updated to match what has been implemented and tested before a public release.

## 1. Rec Menu — Tesla Overlay (OVL)

Rec Menu is a Tesla overlay designed to provide convenient gameplay capture controls.

### Planned Features

- Start and stop gameplay recording.
- Display a small recording timer in the top-right corner of the game screen, including after closing UltraHand.
- Take screenshots.
- Toggle the recording timer display.
- Save captured media directly to the SD card.

## 2. Rec Gallery — Standalone Application (NRO)

Rec Gallery is a separate, standalone Nintendo Switch homebrew application for browsing and managing captured media. It is independent of the Tesla overlay.

### Planned Features

- Browse saved screenshots.
- Preview images.
- Browse recorded videos.
- Play supported video files.
- Delete unwanted captures.

## Storage

Both applications use the following SD card directory:

`sdmc:/switch/rec-menu/`

The project is designed for local SD card storage. No Wi-Fi connection or smartphone is required.

## Requirements

- Nintendo Switch running compatible custom firmware.
- Tesla Menu / UltraHand (for Rec Menu).
- SD card.

## Installation

Installation instructions and release file names will be added after the OVL and NRO packages are finalized and tested.

## Project Status

Rec Menu (OVL) and Rec Gallery (NRO) are separate components of the project and are built and distributed separately.

This README describes the intended completed feature set. Features should only be marked as available after successful build and testing on compatible Nintendo Switch hardware.

---

# Rec Menu & Rec Gallery（日本語）

> **Nintendo Switch向けキャプチャ関連自作ソフトプロジェクト。**  
> 録画する。保存する。振り返る。

このファイルは、プロジェクト完成時に使用するREADMEの下書きです。以下の機能は完成時の目標であり、公開前に実装状況と実機での動作を確認して内容を更新してください。

## 1. Rec Menu — Teslaオーバーレイ（OVL）

Rec Menuは、ゲーム中のキャプチャ操作を簡単にするためのTesla用オーバーレイです。

### 予定している機能

- ゲーム録画の開始・停止。
- UltraHandを閉じたあとも、ゲーム画面右上に小さく録画時間を表示。
- スクリーンショット撮影。
- 録画時間表示のON/OFF切り替え。
- キャプチャデータをSDカードへ直接保存。

## 2. Rec Gallery — 独立アプリ（NRO）

Rec Galleryは、保存したキャプチャデータを閲覧・管理するための独立したNintendo Switch向け自作アプリです。Teslaオーバーレイとは別のアプリとして動作します。

### 予定している機能

- 保存したスクリーンショットの一覧表示。
- 画像プレビュー。
- 録画動画の一覧表示。
- 対応動画ファイルの再生。
- 不要なキャプチャデータの削除。

## 保存先

両方のアプリで、SDカード内の以下のフォルダを使用します。

`sdmc:/switch/rec-menu/`

SDカードへのローカル保存を基本とし、Wi-Fi接続やスマートフォンは必要ありません。

## 必要なもの

- 対応するカスタムファームウェアを導入したNintendo Switch。
- Rec Menuを使用する場合はTesla Menu / UltraHand。
- SDカード。

## インストール方法

OVLとNROのパッケージ構成が確定し、実機テストが完了したあとに、ファイル名と具体的なインストール手順を追加します。

## 開発状況

Rec Menu（OVL）とRec Gallery（NRO）は、別々のコンポーネントとして開発・ビルド・配布する予定です。

このREADMEは完成時の機能を想定した下書きです。対応機能として公開する前に、ビルド成功と対応するNintendo Switch実機での動作確認を行います。

---

**Rec Menu & Rec Gallery — Capture. Save. Replay.**
