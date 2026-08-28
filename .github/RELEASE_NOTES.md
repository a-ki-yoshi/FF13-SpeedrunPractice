
## 更新対象のファイル / Files to update

- dinput8.dll
- ff13-srpractice.ini (新機能のF9ホットキーについて追加。iniを編集して使っている場合は、その項をコピーして既存のファイルにペーストしておくことをお勧めします。ペーストしなくても動作はします)

## 更新内容

### 新規

- **セーブデータの整理**(新機能、`save_tidy`キー・既定F9)
  - 表示する章の絞り込み、カテゴリー管理、並び替え、各データへのメモなど。詳しくはREADMEを参照
- 戦闘オーバーレイ
  - CHAIN行: ターゲットのチェーンが切れるまでの残り秒数
  - ATB行: オプティマチェンジで味方全員のATBが全回復するまでの残り秒数(READY = 次のチェンジで全回復)
  - TARGET ATB行: 敵ATBのゲージの数値表示
  - 行構成を整理(TIME / TARGETの見出しでグループ化)
- 任意戦闘呼び出し
  - 3章 カルラ1戦目(btsc03032): 戦闘後もライブラ値が持続するパターンを追加
  - 4章 ドレッドノート1戦目(btsc04026): 戦闘後もライブラ値が持続するパターンを追加
  - 9章 カラヴィンカ1戦目: 戦闘後もライブラ値が持続するパターンを追加
  - 6章 エンキ エンリル(btsc05027): デプロテが入らないパターン2種を追加
  - 13章 ジャバウォック バンダースナッチ(btsc12081): ウィークが入らないパターンを追加
  - 13章 決戦騎ティアマット(btsc12083): 開幕の技を指定するパターン2種を追加
  - 13章 バルトアンデルス(btsc12090): 1回目のアルテマまでデプロテのみまたはウィークのみ入るパターン2種を追加
- その他
  - クラッシュレポート: ゲーム自身のassertによるクラッシュのとき、どのassertかをログに出すようにした
  - README: リージョンについての記載を追加(海外版でも動作する見込み)

### 修正

- 任意戦闘呼び出し
  - 4章 ドレッドノート1戦目(btsc04026): 余分なオブジェクトが戦闘に写り込む事象を修正
  - 7章 ウシュムガル2戦目(7037): 戦闘後もライブラ値が持続する設定を別パターンとして分離
  - 9章 バルトアンデルス(btsc09055)など: 選択時にクラッシュしうる事象の追加修正
  - 11章 ヒューズ*2(btsc10055): 一覧の表示位置など修正
  - 12章 守護騎アナヴァタプタ(btsc12001): 同じ戦闘を続けて呼び出すとクラッシュする事象を修正
  - 12章 プラウド・クラッド2戦目(btsc12042): 防空モードで開幕することがある事象を修正
  - 13章 オーファン1戦目(btsc12091): 特定条件で余分なオブジェクトが戦闘に写り込む事象を修正
  - 13章 オーファン2戦目(btsc12092): 特定条件で余分なオブジェクトが戦闘に写り込む事象を修正
  - 通常の戦闘を挟むと、呼び出した戦闘のBGMが鳴らないことがある事象を修正
- 戦闘オーバーレイ
  - 表示中にFPS低下などを極力避けるようパフォーマンス改善
- その他
  - 画面の異常や不具合が発生するリスクを軽減する各種対応
---

## What's changed

### New

- **Save-data tidying** (new feature, the `save_tidy` key, F9 by default)
  - Filter the list by chapter, manage categories, sort the list, put your own note on a save,
    and more. See the README for the details
- Battle overlay
  - CHAIN row: how long the target's chain gauge still holds
  - ATB row: how long until a paradigm shift refills the whole party's ATB (READY = the next
    shift does)
  - TARGET ATB row: the target's ATB as a figure, in gauge segments
  - Rows regrouped under TIME / TARGET headings
- Battle picker
  - Ch. 3 Garuda Interceptor #1 (btsc03032): a pattern that keeps your Libra readings after the fight
  - Ch. 4 Dreadnought #1 (btsc04026): a pattern that keeps your Libra readings after the fight
  - Ch. 9 Kalavinka Striker #1: a pattern that keeps your Libra readings after the fight
  - Ch. 6 Enki, Enlil (btsc05027): two "Deprotect will not land" patterns
  - Ch. 13 Jabberwocky, Bandersnatch (btsc12081): an "Imperil will not land" pattern
  - Ch. 13 Tiamat Eliminator (btsc12083): two patterns picking what it opens with
  - Ch. 13 Barthandelus (btsc12090): two patterns where only Deprotect, or only Imperil, lands,
    until the first Ultima
- Other
  - Crash report: when the game crashes on its own assert, the log now names which one
  - README: a note on regions (the mod is expected to work on non-Japanese installs)

### Fixed

- Battle picker
  - Ch. 4 Dreadnought #1 (btsc04026): an object that should not be there appeared in the fight
  - Ch. 7 Ushumgal Subjugator #2 (7037): keeping your Libra readings after the fight is its own
    pattern now
  - Ch. 9 Barthandelus (btsc09055) and others: further fixes for a crash when the fight is
    selected
  - Ch. 11 Circuitron*2 (btsc10055): where it sits in the list, and its mark
  - Ch. 12 Anavatapta Warmech (btsc12001): crash when the same fight was picked twice in a row
  - Ch. 12 The Proudclad #2 (btsc12042): the fight could open in its air-defence mode
  - Ch. 13 Orphan #1 (btsc12091): in some cases an object that should not be there appeared in
    the fight
  - Ch. 13 Orphan #2 (btsc12092): in some cases an object that should not be there appeared in
    the fight
  - The picked fight's BGM could stay silent after an ordinary battle
- Battle overlay
  - Performance work, so showing it costs as little frame rate as we can manage
- Other
  - Assorted work to make display glitches and other faults less likely
