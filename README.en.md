# fps_uncap

[日本語](README.md)

> [!NOTE]
> This Mod is for the [Steam version of Way of the Samurai 4](https://store.steampowered.com/app/312780/) only.

Removes the 30 FPS cap of Way of the Samurai 4 so the game runs at 60 FPS.

> [!WARNING]
> **This is a prototype.** It has not been tried in the game yet and is not in a release.

## Features

- The whole game runs at 60 FPS, events and cut-ins included. Characters, the camera, events, menu and icon animations, and the cursor repeat while a key is held are kept at the same speed as at 30 FPS.
- No game file is changed; the Mod works in memory only.

## Limitations

- On a PC that cannot keep 60 FPS, the game runs slower.
- Some motions, such as the camera shake, may look faster than at 30 FPS.
- Some parts of the game measure time by counting frames. Not all of them are corrected yet, so some speeds may differ from 30 FPS.

## Installation (development version)

> [!WARNING]
> Install [wots4-mods (ModLoader)](https://github.com/nagataaaas/wots4-mods) first.

This Mod is a prototype and is not included in Releases yet. Build it following [Build](#build) below, then install it.

1. Close the game.
2. Copy the built `build/game/mods/fps_uncap/` folder into the `mods` folder in the game's installation folder (the folder containing `WayOfTheSamurai4.exe`).
3. Confirm that `{game installation folder}/mods/fps_uncap/mod.dll` exists, then start the game.

## Uninstallation

1. Close the game.
2. Delete the `{game installation folder}/mods/fps_uncap/` folder.

## Build

Usually this Mod is built inside [wots4-mods](https://github.com/nagataaaas/wots4-mods). To build this Mod alone, run CMake with `-DWOTS4_SDK_DIR=<path to wots4-mods>`.
