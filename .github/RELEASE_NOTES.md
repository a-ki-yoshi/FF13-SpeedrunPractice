
## 更新対象のファイル / Files to update

- dinput8.dll
- ff13-srpractice.ini (新しい設定キー`menu_ok`・`list_prev`・`list_next`を追加。iniを編集して使っている場合は、その3項をコピーして既存のファイルにペーストしておくことをお勧めします。ペーストしなくても動作はします)

## 更新内容

### 新規

- セーブデータの整理
  - Enterなどでも決定可能な設定を追加(`menu_ok`)
  - タイトル画面 カテゴリーの削除機能を追加
  - タイトル画面 新しいカテゴリー作成時に使用中カテゴリーの全データをコピーする選択肢を追加
  - セーブデータ一覧 範囲選択機能を追加。一覧で複数のセーブをまとめてコピー・移動できるように
  - セーブデータ一覧 削除機能を追加
  - セーブデータ一覧 AやDなどのキーでゲームのカーソル移動可能な設定を追加(`list_prev`/`list_next`)
- 任意戦闘呼び出し
  - 3章 戦闘爆撃騎カルラ1戦目(btsc03032): ライブラ値持続パターンで戦闘終了時のオプティマの選択位置も戦闘後に保つよう機能追加
  - 4章 ドレッドノート1戦目(btsc04026): ライブラ値持続パターンで戦闘終了時のオプティマの選択位置も戦闘後に保つよう機能追加
  - 12章 プラウド・クラッド2戦目(btsc12042): スマッシュアッパーの成功率を下げるパターンを追加
  - 13章 ヴラディスラウス(btsc12084): デプロテの成功率を下げるパターンを追加
- ff13-srpractice.ini
  - ホットキーをキー名(Enter, Space, Up, Downなど)でも指定できるように変更

### 修正

- セーブデータの整理
  - 章フィルターを設定した状態でセーブ番号の振り直しの実行直後にセーブ一覧を開いた場合の表示対象の誤りを修正
  - 中断された整理(番号の振り直し・カテゴリー切替)の復旧が済むまで削除・コピー・移動を受け付けないように修正
  - ストーリー上の特定のチェックポイント(賛歌の拝廊など)で、どこでもセーブのサムネイルがその場所専用の背景にならない事象を修正
  - 「進行順(終盤が先頭)」で並べ替えてからセーブ番号の振り直しを実行した後にセーブ一覧を開いた場合の表示順の誤りを修正
  - 並び替えを改善: 日時が同じセーブは物語上の位置→プレイ時間の順で決める、ミッションの進行(クリア数・受諾)を反映、など
- 任意戦闘呼び出し
  - 3章 ガスト3(btsc03001): ライブラ値0の状態で開始するように修正
  - 3章 PSICOM治安猟兵4(btsc03036): 本編どおりスノウの物理攻撃力+30の状態で開始するように修正
  - 4章 バルバロイ2(btsc04055)など: 形態変化中に倒すと、次の戦闘でその敵が形態変化後の姿から開始する事象を修正
  - 9章 PSICOM強襲兵2 PSICOM強襲猟兵1(btsc09050): 本編どおりのパーティ・オプティマで開始するように修正
  - 11章 キングベヒーモス メギストテリウム(btsc11226)など: 呼び出した戦闘の終了後、操作不能になる事象を修正
  - テージンタワーでの一覧にアルカキルティ大平原のエンカウント6件が混ざっていた事象を修正
- その他、使いやすいように各種表示を改善

---

## What's changed

### New

- Save-data tidying
  - A setting so Enter (and other keys) can confirm the menus (`menu_ok`)
  - Title screen: a category can be deleted
  - Title screen: when making a new category, an option to copy every save of the one in use into it
  - Save list: range select -- mark a range in the list to copy or move several saves at once
  - Save list: delete (a marked range)
  - Save list: a setting so keys such as A and D move the game's own save cursor (`list_prev`/`list_next`)
- Battle picker
  - Ch. 3 Garuda Interceptor #1 (btsc03032): on the "Libra readings kept" pattern (its row now reads "Libra readings & paradigm kept"), the paradigm the fight ends on now stays selected after the fight
  - Ch. 4 Dreadnought #1 (btsc04026): on the "Libra readings kept" pattern (its row now reads "Libra readings & paradigm kept"), the paradigm the fight ends on now stays selected after the fight
  - Ch. 12 The Proudclad #2 (btsc12042): a "Launch less likely to lift" pattern
  - Ch. 13 Wladislaus (btsc12084): a "Deprotect less likely to land" pattern
- ff13-srpractice.ini
  - Hotkeys can be written as key names (Enter, Space, Up, Down, and so on)

### Fixed

- Save-data tidying
  - With a chapter filter on, the save list opened right after a renumber showed the wrong saves
  - While an interrupted tidy (renumbering, or a category switch) is still unrecovered, delete, copy and move are refused until recovery has run
  - At certain story checkpoints (such as The Narthex), an anywhere-save's thumbnail did not get the location's own background
  - After a "story order (endgame first)" sort, the save list opened after a renumber came up in the wrong order
  - Better sorting: saves with the same date are settled by story position, then play time; mission progress (cleared and accepted) counts as progress
- Battle picker
  - Ch. 3 Ghast*3 (btsc03001): opens with Libra value 0
  - Ch. 3 PSICOM Warden*4 (btsc03036): opens with Snow's physical attack +30, as the story has it
  - Ch. 4 Pulsework Soldier*2 (btsc04055) and others: an enemy killed while its form was changed opened the next fight already in that form
  - Ch. 9 PSICOM Raider*2, PSICOM Infiltrator*1 (btsc09050): opens with the party and paradigms the story gives it
  - Ch. 11 Behemoth King, Megistotherian (btsc11226) and others: you could end up unable to move after a picked fight
  - Six Archylte Steppe encounters showed up in Taejin's Tower's list
- Other: assorted display improvements for ease of use
