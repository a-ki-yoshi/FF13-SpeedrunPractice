# FF13-SpeedrunPractice

[![Build](https://github.com/a-ki-yoshi/FF13-SpeedrunPractice/actions/workflows/build.yml/badge.svg)](https://github.com/a-ki-yoshi/FF13-SpeedrunPractice/actions/workflows/build.yml)
[![Twitch Status](https://img.shields.io/twitch/status/a_ki_yoshi)](https://www.twitch.tv/a_ki_yoshi)
[![X (formerly Twitter) URL](https://img.shields.io/twitter/url?url=https%3A%2F%2Fx.com%2FOribeAkiyoshi)](https://x.com/OribeAkiyoshi)

[**日本語のREADMEはこちら/Read in Japanese**](README.ja.md)

A mod for the PC (Steam) version of FINAL FANTASY XIII, with features that make speedrun practice easier.

## Requirements

- Windows
- FINAL FANTASY XIII on Steam (PC)

## How to use

1. Download the zip file

[**Click `FF13-SpeedrunPractice-x.x.x.zip` on this page**](https://github.com/a-ki-yoshi/FF13-SpeedrunPractice/releases).

2. Extract the zip file and place the four files other than `LICENSE` and `README` in the same folder as `ffXiiiimg.exe`

Example folder path: `C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY XIII\white_data\prog\win\bin\`

  Can be used together with [FPSFix](https://github.com/MrTyton/Final-Fantasy-Speedruns/tree/master/Final%20Fantasy%20XIII/FPS%20Fix) and [FF13Fix-4GB](https://github.com/a-ki-yoshi/FF13Fix-4GB).

| File | Description |
| ---- | ---- |
| `dinput8.dll` | The mod itself |
| `ff13-srpractice.ini` | Settings file, editable with Notepad or any text editor. OK to delete if you keep the defaults |
| `ff13-practice.bat` | Batch file that starts the game WITH the mod. Details under "Notes for timed runs" |
| `ff13-run.bat` | Batch file that starts the game WITHOUT the mod. Details under "Notes for timed runs" |

3. Launch the game

4. Use each feature with its hotkey (F1 shows a description of each feature)

Note: when updating (installing a different version), replacing the ini file is usually unnecessary. However, the per-version release notes on the Releases page may mention changes to the ini file, so do check them.

## Notes for timed runs

On Speedrun.com, the rules forbid using mods that are not allowed for the game or category.

While timing a run for submission to Speedrun.com or elsewhere, do not use any feature of this tool (mod).

Be especially careful with the battle overlay: even when it is ON, it only shows during battle.

If no text is shown in the top left and no hotkey has been pressed, the game is in a state equivalent to running without the mod.

### If you want the mod itself unusable while timing

The smoothest way is to start the game by running the two bundled .bat files (batch files).

`ff13-run.bat`: starts FF13 WITHOUT the mod

`ff13-practice.bat`: starts FF13 WITH the mod

- They work by renaming `dinput8.dll` to an inert file name (`dinput8.dll.off`) and back, which switches the mod on and off
- You do not have to use the .bat files: moving or renaming the dll file by hand, or launching from your usual shortcut icon, also starts the game normally
- The .bat files can also be run through shortcuts you create for them
- The .bat file names can be changed, as long as the extension stays

## Hotkeys in short

- Each feature has a hotkey assigned to it
- Pressing F1 shows an in-game list of every feature and its current hotkey
- With the default settings, they only work while the game window is active
- Setting `require_focus = 0` in the ini file makes them work while the window is inactive too

## Features

Every feature stays OFF unless you press its hotkey.

For features of the ON/OFF-toggle kind, the ON/OFF state carries over across game restarts.

- **Hotkey list** — `help`
  - Hotkey: F1
  - Shows the list of every feature and its current hotkey; pressing it again closes the list
  - If you change a hotkey in the ini file, the list shows the changed key

- **Game speed** — `game_speed`
  - Hotkey: F2
  - Changes the game speed. Everything changes with it: field movement, battle, ATB speed and so on
  - Default game speed: 3.0
  - Can be changed in the ini file, 0.1~10.0
  - The ini file also takes a list like `2.0,3.0`; the hotkey then switches through it in order, 2.0x → 3.0x → OFF

- **KILL mode** — `kill_mode`
  - Hotkey: F3
  - The hotkey switches through "instant win at battle start + one-hit kill" → "one-hit kill" → OFF, in that order
    - **Instant win at battle start**
      - Turning it ON mid-battle does not affect a battle already running. It takes effect from the next battle start
      - The battle ends in victory the moment it begins
      - No items drop except guaranteed drops
    - **One-hit kill**
      - An enemy's HP becomes 0 the moment you deal damage to it
      - Item drops work the same as in a normal battle

- **Warp** — `warp`
  - Hotkey: F4
  - Record a spot and return to it instantly
  - Useful for practicing enemy avoidance efficiently
    - e.g. record just before the area with the enemies to avoid → attempt the dodge → warp back on success → run into an enemy to reset the state, and so on ([example video](https://youtu.be/o15w01uMevo?si=1GawKC-p5130JfWZ&t=4))
  - **Hold** the hotkey to record your current position with no confirmation
  - **Tap the hotkey twice** in the field to warp to this zone's recorded point
  - One point can be recorded per zone
  - Records stay valid across game restarts
  - If the destination is far away, maps, objects and the like may not load

- **Suppress enemy spawns on save load** — `enemy_suppress`
  - Hotkey: F5
  - After loading a save finishes, the field's enemy groups are no longer placed
  - Turning it ON after a load has finished does nothing. It takes effect from the next load
  - Moving after the load can load a different map, and enemies can appear there

- **Save anywhere** — `save`
  - Hotkey: F7
  - **Turn cloud sync OFF if you use this feature**
  - Lets you save away from save points
  - Loading such a save starts you from the spot where you saved
  - Only possible while moving in the field
  - In the save list screen, saves made with this feature get a mark after their number
    - Default: *
    - The mark text can be edited via `[save] list_mark` in the ini file
  - Depending on the save spot, objects and nearby enemies do not load
  - Depending on the save spot, you can become unable to progress after loading
  - If you move away or delete the DLL file or the `ff13-mods\ff13-srpractice_slots.dat` file, loading goes to the nearest save point instead
  - If your save data lives in a non-standard path, put that path into `save_dir` in the ini file

- **Camera unstick** — `camera_unstick`
  - Hotkey: F8
  - Puts the camera back into leader-follow mode
  - A remedy for the camera sometimes getting stuck while moving after loading a save made with the save-anywhere feature

- **Battle overlay** — `battle_overlay`
  - Hotkey: F10
  - Shows the following during battle
    - GAME: in-game time
    - REAL: real time
    - HP: remaining HP of the targeted enemy
  - When the game-speed feature is also in use, in-game time moves with it

- **Battle picker** — `battle_picker`
  - Hotkeys
    - Open/confirm/close the menu: F11
    - Move in the menu: ↑ ↓ keys
    - Page through the menu: ← → keys
  - Shows a list of the battle scenes for the current field; select one to enter that battle
  - The hotkey toggles the menu open and closed; holding it confirms
  - Holding it while the menu is closed immediately starts the last selected battle (or the top one if nothing has been selected yet)
  - After the battle, items are not consumed, and TP and Libra information return to what they were
  - This feature has a lot of particularly unstable behavior, so please keep the following in mind
    - **After confirming, avoid inputs as much as possible.** At worst the game soft-locks or crashes
    - If confirming overlaps with the action of examining a save point, the confirm is cancelled
    - **The direction allies face at the start of the battle can depend on the direction they faced in the field.** Whether this can be fixed is unknown
    - Depending on the combination of battle scene and location, the background does not load correctly
    - Loading before and after the battle can take quite a long time
    - After the battle, your position may have moved, the map may not load, or the BGM may differ from normal
    - Some battles include a party-change step
      - The post-change party persists after the battle
      - If you would like a party change added to a battle that has none, please contact me
    - For technical reasons, some battles have a cutscene right before them
      - Battles marked "CS" in the list are the ones
      - Skip it as usual
    - For battles without a cutscene, **the BGM may start at a different timing than the real one**
      - `bgm_delay_ms` in the ini file adjusts the BGM start timing to some extent
      - If you would like the cutscene included for a battle that normally has one right before it, please contact me

## Files created automatically

- The `ff13-mods\` folder and the files inside it
  - Created on first launch
- `{save data directory}\ff13-NN.dat.bak`
  - A backup of the save data, created automatically right before the mod adjusts its checkpoint
  - If a save ever fails to load, renaming the `.bak` back to `.dat` recovers it

## When something goes wrong, or the game crashes

### 1. Copy the log somewhere safe first

Copy `ff13-mods\ff13-srpractice.log` somewhere else -- your desktop, for instance.

> **Careful: starting the game again rewrites this log from scratch.**
> Copy it before you restart or close the game (the log from right after it went wrong is the only
> thing there is to go on).

### 2. If you can reproduce it, reproduce it with `verbose`

Change the `[log]` section of `ff13-srpractice.ini` to `level = verbose`, start the game, and do
the same thing again to bring the problem back. On verbose the log records the game's internals as
well, which makes the cause far quicker to find. Once you have reproduced it, put that log
somewhere safe too.

### 3. Sending the log helps enormously

Get in touch with what happened (what you did, and what the game did) and the log you saved.

## Uninstall

- Uninstalling is just deleting `dinput8.dll` from the install folder
- The automatically created files and directories can be deleted too
- Leaving them as they are has no effect on the game
- Data created with the save-anywhere feature can also be deleted in the game screen as usual

## Safety

- This mod performs no communication whatsoever and makes no network connections
- Among the game's own files, the only ones it rewrites are the files created using the save-anywhere feature

## Disclaimer

This mod is unofficial, and is in no way affiliated with or endorsed by Square Enix.

Use it only with a copy of FINAL FANTASY XIII that you legitimately own.
Use at your own risk.

The author accepts no responsibility for lost save data or any other damage.

## Credits

[WhiteCryptTool](https://github.com/Surihix/WhiteCryptTool) by Surihix (GNU GPL v3.0) — the save-data encryption in this mod is derived from WhiteCryptTool. Since both projects are GPLv3, this reuse is fully license-compatible.

## License

FF13-SpeedrunPractice is free software, licensed under the **GNU General Public License v3.0 or
later** (see the `LICENSE` file). Copyright (C) 2026 Akiyoshi.
