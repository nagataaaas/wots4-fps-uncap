# fps_uncap

[日本語](README.md) | [Changelog](CHANGELOG.en.md)

> [!NOTE]
> This Mod is for the [Steam version of Way of the Samurai 4](https://store.steampowered.com/app/312780/) only.

Removes the 30 FPS cap of Way of the Samurai 4 so the game runs at 60, 90 or 120 FPS.

## Features

- The whole game runs at the chosen frame rate, events and cut-ins included. Characters, the camera, events, menu and icon animations, and the cursor repeat while a key is held are kept at the same speed as at 30 FPS.
- The frame rate is chosen in the options and changes as soon as it is chosen.
- No game file is changed; the Mod works in memory only.

## Limitations

- On a PC that cannot keep the chosen frame rate, the game runs slower.
- Some motions, such as the camera shake, may look faster than at 30 FPS.
- Some parts of the game measure time by counting frames. Not all of them are corrected yet, so some speeds may differ from 30 FPS.

## Installation

> [!WARNING]
> Install [wots4-mods (ModLoader)](https://github.com/nagataaaas/wots4-mods) first.

1. Close the game.
2. Download `wots4-mod-fps-uncap-<version>.zip` from [Releases](../../releases) and extract it.
3. Copy the extracted `mods` folder into the game's installation folder (the folder containing `WayOfTheSamurai4.exe`). If a `mods` folder already exists, merge the contents into it.
4. Make sure `{game installation folder}/mods/fps_uncap/mod.dll` exists, then start the game.

## Uninstallation

1. Close the game.
2. Delete the `{game installation folder}/mods/fps_uncap/` folder. This also deletes its settings (`config.ini`).

## Settings

Choose the frame rate on the "Frame Rate" page (「フレームレート」 in the Japanese game) added to the options on the title screen and in the pause menu's settings. The chosen frame rate takes effect at once.

| Item | Choices | Default |
| --- | --- | --- |
| Frame Rate | 30 FPS / 60 FPS / 90 FPS / 120 FPS | 60 FPS |

30 FPS is the game's own speed.

The setting is saved to `{game installation folder}/mods/fps_uncap/config.ini` when the game saves its options, and kept for the next start. It can also be changed by writing 30, 60, 90 or 120 to `fps` under `[frame_rate]` in `config.ini` (takes effect when the game is restarted).
