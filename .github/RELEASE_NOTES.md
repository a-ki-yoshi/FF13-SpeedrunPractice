
## 更新対象のファイル / Files to update

- dinput8.dll
- ff13-run.bat
- ff13-practice.bat
- README.md / README.ja.md

Linux(Steam/Proton)では、`-linux.zip`の方をダウンロードしてください(batの代わりに`ff13-practice.sh`と`ff13-run.sh`が入っています)。

## 更新内容

### 追加

- 「ホットキーと設定」の画面
  - F1キーの一覧にて、iniの全項目をその場で変えられるようにしました。値を変えるとすぐ適用され、iniファイルの該当行だけを書き換えます。元々使用していたiniファイルが存在する場合、その設定を読み込みます
  - 「上書きセーブ時にメモを残す」(既定: OFF)
  - 「画面上端からテキスト表示開始する位置」。SteamのFPS表示などとModの表示が重ならないよう、表示全体を下げて始めます
- 任意戦闘呼び出し
  - 3章「戦闘爆撃騎カルラ 2戦目<ターゲット ヴァニラ>」(btsc03038)を追加: 敵の3回目以降の狙う行動のショックブレスがヴァニラに向かいます
- Linux(Steam/Proton)用のzip(`ff13-practice.sh`・`ff13-run.sh`入り)。起動は確認しましたが、完全な確認ではありません

### 変更

- GAME SPEED
  - 戦闘のGAMEタイマーも倍率どおりに進むようになりました
- セーブデータの整理
  - 並べ替えでセーブファイルの日時を書き換えないようにしました。以前のバージョンで書き換えた日時は、初回の起動時に元の日時へ戻します
  - 並べ替えは「並び順」の1行で選びます(日時順/進行順(序盤が先頭)/進行順(終盤が先頭)/セーブ番号順)
  - 決定は`menu_ok`のキー(既定: Enter)だけになりました(整理キーの長押しでの決定はなくなりました)
  - 振り直しは実行前に確認するようにしました
- ホットキー
  - `require_focus`の代わりに`global_hotkeys`(既定: OFF。ONにすると、ゲームが前面にないときもホットキーが効きます)。今のiniの`require_focus`はそのまま読み替えます
- オーバーレイ
  - 表示言語はゲームの言語に従います(`[overlay] language`はなくなりました)
  - 長い文は表示幅に合わせて、句点・読点・空白の後ろで折り返します
- ログ
  - 前回起動の分を`ff13-mods\prev.log`に残します
  - `ff13-srpractice.ini.bak`は作らなくなりました
- Linux(Proton)でも、Steamクラウド同期の設定を読むようにしました
- 同梱のbatファイル
  - `dinput8.dll`がこのModのものでない場合は、ファイル名を変えず、ゲームも起動せずに止まるようにしました
  - `dinput8.dll`と`dinput8.dll.off`が両方あるとき、ゲームが起動中のときも止まるようにしました

### 修正

- FF13Fix系の`d3d9.dll`を入れていないと、オーバーレイが表示されない不具合を修正
- Windowsを24.9日以上再起動せずに使っていると、任意戦闘呼び出しで不具合が発生する事象を修正

---

## Changes

### Added

- The "Hotkeys and settings" screen
  - The F1 list is now a screen where every ini setting can be changed on the spot. A change takes effect at once, and only that line of the ini file is rewritten. If an ini file you already use is there, its settings are read
  - "Keep a save's note when saving over it" (default: OFF)
  - "Overlay starts this far down". The whole display starts lower, clear of the Steam FPS counter and the like
- Battle picker
  - Ch. 3 "Garuda Interceptor #2 <Target Vanille>" (btsc03038): from the enemy's third aimed action on, Shock Breath goes to Vanille
- A zip for Linux (Steam / Proton) with `ff13-practice.sh` and `ff13-run.sh`. Starting the game was checked, but not everything was

### Changed

- GAME SPEED
  - The battle GAME timer now runs at the chosen speed too
- Tidying the save list
  - Sorting no longer rewrites the save files' dates. Dates rewritten by earlier versions are put back on the first start
  - The order is picked on one "Order" row (by date / by progress, early first / by progress, late first / by save number)
  - Only the `menu_ok` key (default: Enter) confirms; holding the tidy key no longer does
  - Renumbering asks before it runs
- Hotkeys
  - `global_hotkeys` replaces `require_focus` (default: OFF; ON lets the hotkeys work while the game is not in front). An existing ini's `require_focus` is still read
- Overlay
  - The language follows the game's (`[overlay] language` is gone)
  - Long lines wrap to the display width, after a full stop, a comma or a space
- Log
  - The previous launch's log is kept as `ff13-mods\prev.log`
  - `ff13-srpractice.ini.bak` is no longer written
- Under Linux (Proton), the Steam Cloud setting is read too
- The included bat files
  - When `dinput8.dll` is not this mod's, they stop without renaming anything or starting the game
  - They also stop when both `dinput8.dll` and `dinput8.dll.off` are present, or while the game is running

### Fixed

- The overlay did not show without an FF13Fix-family `d3d9.dll`
- Fixed problems in picked fights after more than 24.9 days without restarting Windows
