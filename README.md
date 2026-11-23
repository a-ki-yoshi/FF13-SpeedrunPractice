# FF13-SpeedrunPractice

[![Build](https://github.com/a-ki-yoshi/FF13-SpeedrunPractice/actions/workflows/build.yml/badge.svg)](https://github.com/a-ki-yoshi/FF13-SpeedrunPractice/actions/workflows/build.yml)
[![Twitch Status](https://img.shields.io/twitch/status/a_ki_yoshi)](https://www.twitch.tv/a_ki_yoshi)
[![X (formerly Twitter) URL](https://img.shields.io/twitter/url?url=https%3A%2F%2Fx.com%2FOribeAkiyoshi)](https://x.com/OribeAkiyoshi)

[**日本語のREADMEはこちら/Read in Japanese**](README.ja.md)

A mod for the PC (Steam) version of FINAL FANTASY XIII, with features that make speedrun practice easier.

## Requirements

- Windows
- Linux (Steam / Proton)
  - It has been seen to start, but this is not a full check. Reports are welcome.
- FINAL FANTASY XIII on Steam (PC)
  - Any region should work, though this is unverified.  
    (The Japan-region and international releases share the same executable, and this mod touches only the executable in memory and the save files.)  
    The mod speaks Japanese when the game runs in Japanese, English for every other language.  
    Developed and tested on the Japan-region install; reports from other regions are welcome.

## How to use

1. Download the zip file

[**Click `FF13-SpeedrunPractice-x.x.x.zip` on this page**](https://github.com/a-ki-yoshi/FF13-SpeedrunPractice/releases).  
On Linux (Steam / Proton), download `FF13-SpeedrunPractice-x.x.x-linux.zip` from the same page instead (it has .sh files in place of the .bat files; details under "On Linux (Steam / Proton)").

2. Extract the zip file and place the three files other than `LICENSE` and `README` in the same folder as `ffXiiiimg.exe`

Example folder path: `C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY XIII\white_data\prog\win\bin\`

| File | Description |
| ---- | ---- |
| `dinput8.dll` | The mod itself |
| `ff13-practice.bat` | Batch file that starts the game WITH the mod. Details under "Notes for timed runs" |
| `ff13-run.bat` | Batch file that starts the game WITHOUT the mod. Details under "Notes for timed runs" |

3. Launch the game

4. Use each feature with its hotkey (F1 shows every hotkey and setting, with a description of each, and lets you change them)

Note: no settings file (`ff13-srpractice.ini`) is included. It is created in the same folder as `ffXiiiimg.exe` the first time you change a setting on the hotkeys-and-settings screen.  
Note: if F1 does not work for some reason, type the two lines below in Notepad or similar, save them as `ff13-srpractice.ini`, and place it next to the dll (write the key you want to use instead of F1 in place of `Ctrl+1`).

```ini
[hotkeys]
help = Ctrl+1
```

Note: to update (install another version), place the contents of the new zip the same way, overwriting the old files.

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
- Running one twice in a row by mistake does no harm: nothing changes
- They cannot be used while the game is running. Quit it first
- You do not have to use the .bat files: moving or renaming the dll file by hand, or launching from your usual shortcut icon, also starts the game normally
- The .bat files can also be run through shortcuts you create for them
- The .bat file names can be changed, as long as the extension stays

### On Linux (Steam / Proton)

The Linux zip (`FF13-SpeedrunPractice-x.x.x-linux.zip`) has two .sh files (shell scripts) in place of the two .bat files, doing the same job.  
`ff13-run.sh`: starts FF13 WITHOUT the mod  
`ff13-practice.sh`: starts FF13 WITH the mod  
- Place them with `dinput8.dll`, in the same folder as `ffXiiiimg.exe` (example folder path: `~/.local/share/Steam/steamapps/common/FINAL FANTASY XIII/white_data/prog/win/bin/`)
- Double-click one in your file manager to run it. If it opens in a text editor instead, use "Run" or "Run as a Program" from its right-click menu, or run it in a terminal as `./ff13-run.sh`
- If it will not run at all, allow executing it in the file's properties, or run `chmod +x ff13-*.sh` in a terminal
- As with the .bat files, running either one twice in a row changes nothing, and they cannot be used while the game is running
- You can also switch without the .sh files, in one of these ways:
  - Rename `dinput8.dll` to `dinput8.dll.off` and back by hand (Proton loads a `dinput8.dll` in the game folder with no extra setup)
  - In the game's Properties in Steam, set the launch options to `WINEDLLOVERRIDES="dinput8=b" %command%` to start without the mod, and clear them to start with it

## Using it with other mods

- Can be used together with [FF13Fix-4GB](https://github.com/a-ki-yoshi/FF13Fix-4GB)
- For [FPSFix](https://github.com/MrTyton/Final-Fantasy-Speedruns/tree/master/Final%20Fantasy%20XIII/FPS%20Fix), rename its `dinput8.dll` to `d3d9.dll`. FF13Fix-4GB, FPSFix and this mod cannot all be used at the same time.
- The included bat files stop without renaming anything when the `dinput8.dll` in place is not this mod's

## A request when sharing

When you share footage or images of anything that does not appear in normal play, please make it clear that a mod was used.  
This is so it is not mistaken for a bug in normal play.

## Hotkeys in short

- Each feature has a hotkey assigned to it
- F1 shows the hotkeys-and-settings screen in the game. It lists every feature with its current hotkey, and every setting, and you can change them there
- With the default settings, they only work while the game window is active
- Turning on "Hotkeys while another window is active" on the hotkeys-and-settings screen makes them work while the window is inactive too

## Features

Every feature stays OFF unless you press its hotkey.

For features of the ON/OFF-toggle kind, the ON/OFF state carries over across game restarts.

- **\[F1\] Hotkeys and settings**
  - Shows or hides the hotkeys-and-settings screen at the top of the screen: every hotkey and setting, with the one under the cursor explained at the bottom
  - The picker's keys drive it: up and down pick a row, left and right change its value, confirm (`menu_ok`) starts a key assignment or switches a value, `cancel` or this hotkey closes it
  - To reassign a hotkey, confirm on its row and press the new key (with Ctrl, Shift or Alt if you like). Pressing the key it already has unbinds it; a key another function already uses is refused, and the screen says which function has it
  - The eight picker/menu keys share one row, where left and right pick which one to reassign
  - A change takes effect at once and is written back into `ff13-srpractice.ini`: only that line changes
  - Whether the screen is open is not kept across launches
  - Settings you can change
    - Hotkeys while another window is active (default: OFF)
      - When OFF, hotkeys are accepted only while the game window is active
      - When ON, they work while it is not active too
    - GAME SPEED factors (default: 3.0)
      - Sets the factors that "Game speed" below steps through
      - The row lists candidate factors; confirm puts the one under the cursor in or out of the hotkey's cycle (one to eight of them, stepped through in ascending order)
      - The candidates are 0.5/1.5/2/3/4/5/6/8/10
      - Editing the ini file directly allows any value from 0.1 to 10
    - Battle picker music start timing (default: 1200 ms)
      - 0 to 8000 ms. 0 brings the music in at once
    - Text added to save-anywhere saves (default: *)
      - Applies to saves made from then on. Details under "Save anywhere" below
    - Keep a save's note when saving over it (default: OFF)
      - When ON, a note stays on a save you save over. When OFF, saving over it clears the note
    - Save folder
      - Display only. To change it, edit `save_dir` in the ini file (empty = auto-detect)
    - Overlay starts this far down (default: 34 px)
      - 0 to 400 px in steps of 8. Move it down when an FPS counter or another overlay covers the top lines
    - Log level (default: normal)
      - Left and right change it (quiet / normal / verbose)
      - The log starts fresh on every launch, and only the previous launch's is kept, as `prev.log`

- **\[F2\] Game speed**
  - Changes the game speed. Everything changes with it: field movement, battle, ATB speed and so on
  - Default game speed: 3.0
  - The values can be changed on the settings screen or in the ini file

- **\[F3\] KILL mode**
  - The hotkey switches through "instant win at battle start + one-hit kill" → "one-hit kill" → OFF, in that order
    - **Instant win at battle start**
      - Turning it ON mid-battle does not affect a battle already running. It takes effect from the next battle start
      - The battle ends in victory the moment it begins
      - No items drop except guaranteed drops
    - **One-hit kill**
      - An enemy's HP becomes 0 the moment you deal damage to it
      - Item drops work the same as in a normal battle
      - **Do not summon an Eidolon while this is ON** — it has only been tested against ordinary enemies. Turn it OFF before summoning

- **\[F4\] Warp**
  - Record a spot and return to it instantly
  - Useful for practicing enemy avoidance efficiently
    - e.g. record just before the area with the enemies to avoid → attempt the dodge → warp back on success → run into an enemy to reset the state, and so on ([example video](https://youtu.be/o15w01uMevo?si=1GawKC-p5130JfWZ&t=4))
  - **Hold** the hotkey to record your current position with no confirmation
  - **Tap the hotkey twice** in the field to warp to this zone's recorded point
  - One point can be recorded per zone
  - Records stay valid across game restarts
  - If the destination is far away, maps, objects and the like may not load

- **\[F5\] Suppress enemy spawns on save load**
  - After loading a save finishes, the field's enemy groups are no longer placed
  - Turning it ON after a load has finished does nothing. It takes effect from the next load
  - Moving after the load can load a different map, and enemies can appear there

- **\[F7\] Save anywhere**
  - **Turn cloud sync OFF if you use this feature. Leaving it ON is untested**
  - Lets you save away from save points
  - Loading such a save starts you from the spot where you saved
  - Only possible while moving in the field
  - In the save list screen, saves made with this feature get a mark after their number
    - Default: *
    - The mark text can be edited on the settings screen or via `[save] list_mark` in the ini file (max 8 characters, any but digits)
      - A character the game's font does not have is drawn as a blank, so check how yours looks in the list
  - Depending on the save spot, objects and nearby enemies do not load
  - Depending on the save spot, you can become unable to progress after loading
  - If you move away or delete the DLL file or the `ff13-mods\ff13-srpractice_slots.dat` file, loading goes to the nearest save point instead

- **\[F8\] Camera unstick**
  - Puts the camera back into leader-follow mode
  - A remedy for the camera sometimes getting stuck while moving after loading a save made with the save-anywhere feature

- **\[F9\] Tidy the save data**
  - Controls
    - Run the selected entry: Enter (`menu_ok`)
      - While this menu is up the keyboard is kept from the game's own controls, so sharing the game's confirm key is safe
    - Choose an entry: ↑ ↓ keys
    - Choose which save the menu acts on, over the save list: A D (`list_prev` / `list_next`)
      - While this menu is up the keyboard is kept from the game's own controls, but the keys set here still move its cursor
    - Type text: the keys themselves
    - Delete a character: Backspace
  - **Usable only while this game's Steam cloud sync is OFF.**
  - The menu only appears on the title screen (the attract movie included) and on the save list
  - Each of those screens shows a different menu
  - As a rule, confirm (Enter) to run what is selected
  - What the title screen's menu can do
    - Which saves the list shows
      - A chapter filter for the load list
      - Only that chapter's saves are shown in the game's save list
      - Confirming the row resets the filter and shows every chapter again
      - No save is moved or renamed
      - It has no effect on save screens opened during play (at a save point or with save anywhere)
      - The setting is remembered, so the next start opens on the same chapter (if that chapter has no save left, the whole list comes back)
    - Manage categories
      - Swaps the whole set of saves in and out, a folder at a time
        - The category in use sits in the save folder as always; every other one is parked in `{save folder}\categories\{category name}\`
      - Until you make one, you are treated as being in a category named "main"
      - Each category holds up to 99 saves
      - The rows
        - "using: {category name}"
          - ← → pick where to switch to, then confirm to switch
        - New category, and switch to it (a copy of the latest save / a copy of every save)
          - Name it, and the new category becomes the one in use
          - "a copy of the latest save": only the newest save comes along
            - The copy is there so the list is never empty, which would take LOAD GAME off the title screen
            - Delete it in the game's own list if you do not want it
          - "a copy of every save": every save in the category in use is copied
          - The input box
            - Single-byte characters can be typed straight in
            - Text copied elsewhere can be pasted with Ctrl+V
              - That way full-width, kana and other multi-byte characters work too
            - Up to 24 characters
            - `\ / : * ? " < > | ,` cannot be used
            - Backspace deletes one character
            - A name a category already has cannot be confirmed (the error shows in red in the box)
            - Enter confirms, the hotkey cancels
        - Rename the category in use
          - Renames whichever category the "using:" row has selected
        - Delete "{category name}"
          - ← → pick the category to delete, then confirm
          - A category holding one save or more asks again: confirm once more (the question lapses after 8 seconds)
          - The category in use cannot be picked (switch to another one first if you want it gone)
          - It goes to the Windows Recycle Bin with everything in it, so a mistake can be undone from there
            - If your save folder is on a drive with no Recycle Bin (a network or removable one), it is deleted outright
      - The save-anywhere bookkeeping files and the `.bak` backups travel with the saves
      - If a switch is cut short by a crash or a power cut, the next start finishes it
    - Sort order
      - ← → pick the order the game builds its save list in: date (the game's own) / story order (Chapter 1 first) / story order (endgame first) / by save number
      - The order is applied as the list is built, so it takes effect the next time a save or load list opens
      - No timestamp is written to any file: the dates the list shows are the real save dates, in the game and in Explorer alike
      - The choice is remembered, so every list comes out in it until you pick another
      - A new save takes its place by the same keys as every other save -- on "date" that is the top, exactly as the game does it
      - Saves whose contents cannot be read sit at the bottom of the two story orders
      - If you used an older version's sorter, the first start of this version puts the real save dates back on the files it had re-stamped
    - Renumber the saves
      - Renumbers the saves in the category in use into story order (saves at the same point go by date)
      - The first confirm only asks (the row turns into the question, with how many saves would move); a second one runs it
      - If the renaming is cut short by a crash or a power cut, the next start either finishes it or puts the old numbers back
  - What the save list's menu can do (to whichever save the cursor is on)
    - The menu follows that cursor: A/D (or a pad) still move it while the menu is up, and every row
      switches to the save it lands on (the title line names it)
    - Note
      - Type a note and it is shown in the game's save list
      - It goes at the end of the first line, like "Save Data 05, Chapter 9 - text1"
      - The input box
        - Single-byte characters can be typed straight in
        - Text copied elsewhere can be pasted with Ctrl+V
          - That way full-width, kana and other multi-byte characters work too
        - Up to 24 characters
        - `,` cannot be used
        - Backspace deletes one character
        - Enter confirms, the hotkey cancels
      - The "make a new save" row on the save screen you open from the field cannot take a note
        - Once it is made, reopen the list and it can
    - Send to another category (the copy and move rows)
      - Copies or moves the save, or the whole marked range
      - ← → pick where it goes; the row says so ("copy -> {category name}")
      - Picking "<new category>" names a fresh one
      - It takes the first free number in the category it lands in (the number shown in the list changes with it)
      - If the destination has fewer free numbers than a range needs, nothing is sent and it says so
      - After a move, the row it came from is still drawn in the list you have open
        - Choosing it only says the load failed, which is harmless
        - Reopen the list and it is gone
      - Careful: if a move empties the category you sent it from, LOAD GAME disappears from the title screen
    - Range selection (work on several saves at once)
      - Run "select a range (save N is one end)" and that save becomes one end
      - Move the game's cursor to the other end (A/D, or a pad): the copy, move and delete rows turn into "saves N-M as listed (x of them)" as it goes
      - The range is what the list shows between the two ends, both included (the numbers name the two ends, not every number in between); with a chapter filter on, only the rows on screen count
      - Clear the mark by running the same row again; changing the filter or leaving the list also clears it
    - Delete
      - Deletes every save a marked range covers -- to delete one save, the game's own list does that with E
      - With no range marked, it cannot run
      - Confirm once and the row asks ("confirm: delete N save(s) -- [Enter] again to run"); confirm again and it runs
      - The question stands down after 8 seconds, or if you move off the row, **move the cursor to another save**, close the menu or clear the mark
      - Saves go to the Recycle Bin, each with its `.bak` file, so Windows can put them back
        - On a drive that has no Recycle Bin (a network or removable one) the shell deletes them outright
      - The rows stay drawn in the list you have open; reopen it and they are gone
      - Careful: deleting every save in the category takes LOAD GAME off the title screen, as an emptying move does

- **\[F10\] Battle overlay**
  - Shows the following during battle
    - TIME
      - GAME: in-game time
      - REAL: real time
      - ATB \*\*s: the seconds until a paradigm shift refills the whole party's ATB (READY = the next shift does)
    - TARGET
      - HP: the target's remaining HP
      - ATB: the value of the target's ATB gauge
      - CHAIN: how long the target's chain gauge lasts -- the seconds left before the multiplier falls back to 100%
  - When the game-speed feature is also in use, GAME (in-game time) moves at that speed; REAL does not

- **\[F11\] Battle picker**
  - Controls
    - Move in the menu: ↑ ↓ keys
    - Page through the menu: ← → keys
  - Shows a list of the battle scenes for the current field; select one to enter that battle
  - The hotkey toggles the menu open and closed; holding it confirms
  - Holding it while the menu is closed immediately starts the last selected battle (or the top one if nothing has been selected yet)
  - Shrouds can be used before entering a battle this way: with a Deceptisol, for example, a fight that allows a preemptive strike to begin with still gets one
  - After the battle, items are not consumed, and TP and Libra information return to what they were
  - A fight marked ● is one an Any% run has to do
  - A fight marked △ is one an Any% run does in some circumstances
  - The row for the fight your last ordinary battle was reads "here"
  - This feature has a lot of particularly unstable behavior, so please keep the following in mind
    - **After confirming, avoid inputs as much as possible.** At worst the game soft-locks or crashes
      - The pause menu (Start) is the dangerous one: opening it between confirming and the battle appearing can crash the game. Keep off Start until the fight is on screen
    - If the battle begins with enemies missing, a message says `btsc##### came up short -- pick it again`; selecting the same fight again works
    - A picked fight is not the fight the game's script expects at that spot. One was beaten on hardware with story progress intact, but please treat unusual outcomes (symbols that do not disappear, chests, story flags) as possible
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
      - The settings screen or `bgm_delay_ms` in the ini file adjusts the BGM start timing to some extent
      - If you would like the cutscene included for a battle that normally has one right before it, please contact me

## Files created automatically

- The `ff13-mods\` folder and the files inside it
  - Created on first launch
- `ff13-srpractice.ini`
  - Created in the same folder as `ffXiiiimg.exe` when you change a setting on the hotkeys-and-settings screen
- `{save data directory}\ff13-NN.dat.bak`
  - A backup of the save data, created automatically right before the mod adjusts its checkpoint
  - If a save ever fails to load, renaming the `.bak` back to `.dat` recovers it

## When something goes wrong, or the game crashes

### 1. Copy the log somewhere safe first

Copy `ff13-mods\ff13-srpractice.log` somewhere else -- your desktop, for instance.

> **Careful: starting the game again moves this log to `ff13-mods\prev.log` and starts a new one,
> and the start after that overwrites `prev.log`.** Copy it before you start the game twice (the log
> from right after it went wrong is the only thing there is to go on).

### 2. If you can reproduce it, reproduce it with `verbose`

Set the log level to `verbose` on the settings screen, start the game, and do
the same thing again to bring the problem back. On verbose the log records the game's internals as
well, which makes the cause far quicker to find. Once you have reproduced it, put that log
somewhere safe too.

### 3. Sending the log helps enormously

Get in touch with what happened (what you did, and what the game did) and the log you saved.

## Uninstall

- Uninstalling is just deleting `dinput8.dll` from the install folder
- The automatically created files and directories can be deleted too
  - **Exception: the `categories\` folder in your save directory holds the actual saves of every category not in use.** Before deleting it, move the saves you want out of `categories\` (into the save folder itself), or back the folder up
- Leaving them as they are has no effect on the game
- Data created with the save-anywhere feature can also be deleted in the game screen as usual

## Safety

- This mod performs no communication whatsoever and makes no network connections
- Among the game's own files, the only ones it rewrites are the save files created using the save-anywhere and tidy features

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
