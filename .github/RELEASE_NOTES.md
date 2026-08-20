
## 更新対象のファイル / Files to update

- dinput8.dll
- ff13-srpractice.ini (コメントアウトの説明文追加のみ)

## 更新内容

### 新規

- 任意戦闘呼び出し
  - 4章 PSICOM特務兵1 警備軍一般兵射手4(btsc04027): メンバーチェンジ処理を追加(実戦とライブラ値を揃えやすくするため)
- iniファイル
  - \[log\]level: 説明文を追加。**設定を変えている場合は、説明文の箇所のみをコピーして既存のファイルにペーストしておくなどお願いします**
- その他
  - ログ: クラッシュ時にレポート出力する機能を追加。`[log] level`の設定に関係なく常に書かれる
  - README: 不具合時の手順(再起動前にログを退避 → できればverboseで再現 → ログを送る)を記載

### 修正

- 任意戦闘呼び出し
  - 3章 カルラ1戦目(btsc03032): 戦闘後のオプティマのデフォルト位置修正
  - 4章 ドレッドノート1戦目(btsc04026): 勝利後の挙動を改善
  - 4章 PSICOM特務兵2(btsc04031): 一覧の表示位置など修正
  - 5章 クロウラー4(btsc06041): 一覧の表示位置など修正
  - 5章 ユイジンシャン(btsc06046): 属性指定の場合、2連続で同じ属性になる不具合を修正
  - 5章 ユイジンシャン(btsc06046): 1発目の魔導弾後の追加攻撃が一定確率で出るように調整
  - 7章 ゲーム中に実在しない戦闘(btsc07068): 一覧から除外
  - 9章 カラヴィンカ2戦目(btsc09042): BGM修正
  - 9章 バルトアンデルス(btsc09055)など: 選択時にクラッシュする事象を修正
- 戦闘オーバーレイ
  - REALタイマー: GAMEタイマーが動き始めるまで直前の戦闘での時間を表示する事象を修正
- その他
  - 画面の異常や不具合が発生するリスクを軽減する各種対応
---

## What's changed

### New

- Battle picker
  - Ch. 4 PSICOM Tracker*1, Corps Gunner*4 (btsc04027): the party is now changed for
    this fight (so its Libra readings line up with the real one)
- ini file
  - \[log\] level: explanatory lines added. **If you have edited your ini, copy just those
    comment lines across to the file you already have**
- Other
  - Log: crash reporting added. It is written whatever `[log] level` is set to
  - README: what to do when something goes wrong (save the log before restarting ->
    reproduce on verbose if you can -> send the log)

### Fixed

- Battle picker
  - Ch. 3 Garuda Interceptor #1 (btsc03032): the paradigm the field is left holding
    after the fight
  - Ch. 4 Dreadnought #1 (btsc04026): what happens after you win
  - Ch. 4 PSICOM Tracker*2 (btsc04031): where it sits in the list, and its mark
  - Ch. 5 Crawler*4 (btsc06041): where it sits in the list, and its mark
  - Ch. 5 Aster Protoflorian (btsc06046): with an element pinned, the same element could
    come up twice in a row
  - Ch. 5 Aster Protoflorian (btsc06046): the extra attack after the first magic shot now
    comes out often enough to practise
  - Ch. 7 a battle that is not in the retail game (btsc07068): off the list
  - Ch. 9 Kalavinka Striker #2 (btsc09042): BGM
  - Ch. 9 Barthandelus (btsc09055) and others: crash when the fight was selected
- Battle overlay
  - REAL timer: it showed the previous fight's time until the GAME timer started moving
- Other
  - Assorted work to make display glitches and other faults less likely
