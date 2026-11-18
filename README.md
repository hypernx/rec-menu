# Rec Menu / Rec Gallery

**English** | [日本語](README.ja.md)

Homebrew tools for recording gameplay on a Nintendo Switch running custom firmware, then watching the recordings on the console.

| Component | Type | What it does |
|---|---|---|
| **Rec Menu** | Tesla overlay (`.ovl`) | Records gameplay video and audio to MP4 and takes screenshots. You open it from Ultrahand / Tesla while a game is running. |
| **Rec Gallery** | Homebrew app (`.nro`) | Lists, plays and deletes your recordings and screenshots, a bit like the Album. You launch it from the Homebrew Menu. |

Everything is saved to the SD card. No PC, Wi‑Fi or phone is needed.

---

## Features

### Rec Menu (overlay)

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

### Rec Gallery (app)

- **Two tabs, Videos and Photos**: each shows thumbnails with the date and time it was taken.
- **Sorting**: newest first, oldest first, largest first, or smallest first.
- **Video player**: pause, skip ±10 s / ±30 s, and a progress bar.
- **Photo viewer**: full screen, step to the previous or next photo.
- **Delete**: asks you to confirm first.

---

## Installation

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

### Requirements

- A Nintendo Switch running custom firmware (Atmosphère)
- **Rec Menu**: Ultrahand (or Tesla)
- **Rec Gallery**: the Homebrew Menu. For smoother playback, launch it through a game (hold **R** while starting a game) rather than from the Album; Album (applet) mode gets less memory and CPU.

---

## Controls

### Rec Menu

Open the overlay with the Ultrahand button combo (default: **L + D‑pad Down + Right Stick press**).

| Item | Action |
|---|---|
| **Record** | **A** starts recording. The menu closes and you go back to the game. Open the menu again and press **A** to stop. |
| **Show timer (top right)** | Turns the recording timer on or off. |
| **Notification display** | Shows whether the timer and notifications are working (`OK`, or which step failed). |
| **Screenshot** | **A** closes the menu and saves a screenshot. |

While recording, **B** only hides the menu. It does not stop the recording.

### Rec Gallery

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

## Limitations

- **Only games can be recorded.** Rec Menu uses the console's built-in game video capture stream (the same video the 30‑second capture button uses), so the HOME Menu and other system screens cannot be recorded. Some games that do not allow video capture cannot be recorded either.
- **Audio is uncompressed PCM** (about 11 MB per minute). The audio plays in VLC on PC and in Rec Gallery, but some phone video players may not play it.
- **Leaving Rec Menu stops the recording.** Hiding the menu is fine, but if you go back to the Ultrahand overlay list or open another overlay, Rec Menu closes and the recording stops (everything up to that point is saved).
- **Cannot be used with SysDVR.** Both use the same video stream. Rec Menu shows an error if SysDVR is running.
- **The timer and notifications are experimental.** Overlays only get a small amount of memory (4 MB on recent firmware). If **Notification display** shows something other than `OK`, the timer and notifications are unavailable, but recording still works.
- **Rec Gallery plays videos in software (CPU only).** Playback may stutter depending on conditions.

---

## Building

You do not need a PC to build. GitHub Actions builds both components on every push.

- Workflow: `.github/workflows/build.yml`
  - **build-ovl**: builds Rec Menu with libtesla from [Tesla-Template](https://github.com/WerWolv/Tesla-Template). A small patch to libtesla adds the HOME button and shutdown hooks and keeps the overlay out of recordings.
  - **gallery**: builds Rec Gallery with devkitPro `switch-ffmpeg`, `switch-sdl2`, `switch-sdl2_ttf` and `switch-sdl2_image`.
- Artifacts:
  - `ovl` (`tpl.ovl` + `tpl.elf`)
  - `nro` (`RecGallery.nro` + `RecGallery.elf`)

  The `.elf` files are debug symbols for reading crash reports.

### Reading a crash report

If the console shows a crash report (`/atmosphere/crash_reports/*.log`), look up the `tpl + 0x…` addresses with `aarch64-none-elf-addr2line -f -C -e tpl.elf 0x…`. Use the `.elf` from the same build that crashed.

---

## Source layout

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

## Credits

- [SysDVR](https://github.com/exelix11/SysDVR) by exelix11: reference for the safe way to use `grc:d` and for the SPS/PPS values
- [libtesla](https://github.com/WerWolv/libtesla) / [Tesla-Template](https://github.com/WerWolv/Tesla-Template) by WerWolv
- [Ultrahand Overlay](https://github.com/ppkantorski/Ultrahand-Overlay) / nx-ovlloader
- [libnx](https://github.com/switchbrew/libnx), [devkitPro](https://devkitpro.org/), FFmpeg, SDL2

Use at your own risk.
