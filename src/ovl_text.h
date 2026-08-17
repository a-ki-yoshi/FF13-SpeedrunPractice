// FF13-SpeedrunPractice -- everything the overlay says, in both languages.
//
// Copyright (C) 2026 Akiyoshi
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one file in the tracked tree allowed to hold Japanese; everything else stays English.
// Also edited by common/ui/keys_and_overlay_editor.html, which round-trips both columns through
// keys_and_overlay.json and rewrites the two tables below (common/ui/README.md).
//
// Constraints -- each corrupts a line rather than merely looking wrong:
//   1. A Japanese string must carry the same %-conversions as its English one: same count, same
//      types, same ORDER. snprintf reads its arguments off the format string, and there is no
//      positional form here, so reword rather than reorder.
//   1b. One entry per line. A split C string is legal but this file's checker refuses it, and the
//      editor page cannot round-trip it. Long is fine; wrapped is not.
//   2. UTF-8, no BOM. The Japanese column reaches the SHIFTJIS font via
//      MultiByteToWideChar(CP_UTF8, ...); a file saved as CP932 puts mojibake on screen.
// An empty Japanese string is not an error -- that line falls back to English on its own.
//
// Deliberately NOT here: the picker's row labels (<BOSS> / <EVENT> / <MISSION> / <STORY>, "CS",
// "here"). They sit in a fixed-width table whose columns are counted in ASCII glyphs (see
// BS_NAME_COL and the count above tagX), so a proportional Japanese glyph would shift every column
// on that row. The fight NAMES there are language-aware by another route (btsc_hand's jp/en).

#ifndef FF13_OVL_TEXT_H
#define FF13_OVL_TEXT_H

typedef enum {
    T_KILL_LINE = 0,      //  %s = key label, %s = one of the three mode names below
    T_KILL_INSTANT,       //  a NAME, dropped into T_KILL_LINE
    T_KILL_ONEHIT,
    T_KILL_OFF,
    T_GAMESPEED,          //  %s = key label, %.2g = factor
    T_SUPPRESS,           //  %s = key label
    T_SWAP,
    T_TIMER_GAME,         //  a LABEL: the figure is drawn beside it, right-aligned
    T_TIMER_REAL,         //  likewise
    T_ENEMY_HP,           //  likewise
    T_PICKER_HEAD,        //  %d = selected, %d = total
    T_PICKER_FOOT,        //  six %s = up, down, left, right, start (held), close
    T_WARP_ASK_WARP,
    T_WARP_ASK_KEYS,      //  three %s = warp, warp again (held), cancel
    T_PICK_LOST_WINDOW,
    T_PICK_FIELD_ONLY,
    T_PICK_CHOCOBO,
    T_PICK_NO_OBJECT,
    T_PICK_NEVER_STARTED,
    T_PICK_TEACH_ONE,     //  %u = zone
    T_WARP_FIELD_ONLY,    //  record and warp refuse for the same reason, in the same words
    T_WARP_NO_READ,       //  ...as do both "cannot read the party yet" cases
    T_WARP_NO_POINT,
    T_WARP_WARPED,
    T_WARP_NO_POINT_HOLD, //  %s = key label
    T_WARP_RECORDED,
    T_WARP_CANCELLED,
    T_BATTLE_OVL_ON,
    T_BATTLE_OVL_OFF,
    T_HELP_HEAD,          //  %s = the help key itself (the same key closes the list)
    T_HELP_GAMESPEED,     //  %s = the cycle, live from gs_steps_arrow()
    T_HELP_KILL,
    T_HELP_WARP,
    T_HELP_SUPPRESS,
    T_HELP_SAVE,
    T_HELP_CAMERA,
    T_HELP_BATTLE_OVL,
    T_HELP_PICKER,        //  four %s = picker up, down, left, right
    T_HELP_BADKEY,        //  appended to a row whose ini binding did not parse
    T_HELP_BADCFG,        //  %s = the refused setting names, from cfg_bad_list()
    T_SAVE_FIELD_ONLY,    //  save hotkey refused: not in FIELD_IDLE (same reason as the warp's)
    T_SAVE_NO_READ,       //  save hotkey refused: leader position unreadable
    T_COUNT
} ovl_msg;

static const char* const kOvlEn[T_COUNT] = {
    /* T_KILL_LINE          */ "(%s) %s: ON",
    /* T_KILL_INSTANT       */ "INSTANT WIN AT BATTLE START + ONE-HIT KILL",
    /* T_KILL_ONEHIT        */ "ONE-HIT KILL",
    /* T_KILL_OFF           */ "OFF",
    /* T_GAMESPEED          */ "(%s) GAME SPEED %.2gx: ON",
    /* T_SUPPRESS           */ "(%s) SUPPRESS ENEMY SPAWNS AT SAVE LOAD: ON",
    /* T_SWAP               */ "PREPARING...",
    /* T_TIMER_GAME         */ "GAME",
    /* T_TIMER_REAL         */ "REAL",
    /* T_ENEMY_HP           */ "HP",
    /* T_PICKER_HEAD        */ "BATTLE PICKER  (%d/%d)",
    /* T_PICKER_FOOT        */ "  [%s/%s]=move  [%s/%s]=page  hold [%s]=fight  [%s]=close",
    /* T_WARP_ASK_WARP        */ "Warp the leader to this zone's recorded point?",
    /* T_WARP_ASK_KEYS        */ "  [%s] again = OK   hold [%s] = record here   [%s] = Cancel",
    /* T_PICK_LOST_WINDOW   */ "the fight lost its window -- pick it again",
    /* T_PICK_FIELD_ONLY    */ "Battle picker: field only",
    /* T_PICK_CHOCOBO       */ "Battle picker: dismount the chocobo first",
    /* T_PICK_NO_OBJECT     */ "Battle picker: cannot be used from here",
    /* T_PICK_NEVER_STARTED */ "that fight never started -- the field is yours again",
    /* T_PICK_TEACH_ONE     */ "Battle picker: this area is not supported yet. Note the area number (zone=%u) and let the developer know.",
    /* T_WARP_FIELD_ONLY      */ "WARP: not during a battle or an event. Use it where you can walk.",
    /* T_WARP_NO_READ         */ "WARP: cannot read what it needs yet. Move the leader a little.",
    /* T_WARP_NO_POINT        */ "WARP: no point recorded in this zone.",
    /* T_WARP_WARPED          */ "WARP: warped to the recorded point.",
    /* T_WARP_NO_POINT_HOLD   */ "WARP: no point in THIS zone yet -- hold %s here to record one.",
    /* T_WARP_RECORDED        */ "WARP: point recorded for this zone.",
    /* T_WARP_CANCELLED       */ "WARP: cancelled.",
    /* T_BATTLE_OVL_ON      */ "Battle overlay: ON (timer + enemy HP)",
    /* T_BATTLE_OVL_OFF     */ "Battle overlay: OFF",
    /* T_HELP_HEAD          */ "HOTKEYS  ([%s] = close)",
    /* T_HELP_GAMESPEED     */ "Game speed -- each press: %s (the list is set in ff13-srpractice.ini)",
    /* T_HELP_KILL          */ "KILL mode -- each press: instant win + one-hit kill -> one-hit kill -> OFF",
    /* T_HELP_WARP          */ "Warp -- hold = record here / tap twice = warp",
    /* T_HELP_SUPPRESS      */ "Suppress enemy spawns on save load -- turned ON after a load, it works from the next load",
    /* T_HELP_SAVE          */ "Save anywhere -- turn Steam cloud sync OFF to use it",
    /* T_HELP_CAMERA        */ "Camera unstick -- when the camera jams after loading a save-anywhere save",
    /* T_HELP_BATTLE_OVL    */ "Battle overlay -- shows the timer and enemy HP in battle",
    /* T_HELP_PICKER        */ "Battle picker -- [%s/%s] = move  [%s/%s] = page  hold = start (closed: last pick); avoid inputs after that",
    /* T_HELP_BADKEY        */ "  * bad ini value -> default key",
    /* T_HELP_BADCFG        */ "* bad values in the ini: %s (defaults are in force)",
    /* T_SAVE_FIELD_ONLY    */ "SAVE: not during a battle or an event. Use it where you can walk.",
    /* T_SAVE_NO_READ       */ "SAVE: cannot read what it needs yet. Move the leader a little.",
};

// UTF-8. "" = not translated, falls back to the English above.
// GAME and REAL are left in English on purpose: that is what a speedrun timer's clocks are called
// in Japanese too, and translating them would read worse against a splits file.
static const char* const kOvlJp[T_COUNT] = {
    /* T_KILL_LINE          */ "(%s) %s: ON",
    /* T_KILL_INSTANT       */ "戦闘開始時に即勝利 + 一撃必殺",
    /* T_KILL_ONEHIT        */ "一撃必殺",
    /* T_KILL_OFF           */ "OFF",
    /* T_GAMESPEED          */ "(%s) GAME SPEED %.2gx: ON",
    /* T_SUPPRESS           */ "(%s) セーブデータロード時の敵の出現を抑止: ON",
    /* T_SWAP               */ "PREPARING...",
    /* T_TIMER_GAME         */ "GAME",
    /* T_TIMER_REAL         */ "REAL",
    /* T_ENEMY_HP           */ "HP",
    /* T_PICKER_HEAD        */ "BATTLE PICKER  (%d/%d)",
    /* T_PICKER_FOOT        */ "  [%s/%s]=移動  [%s/%s]=ページ送り  [%s]長押し=開始  [%s]=閉じる",
    /* T_WARP_ASK_WARP        */ "記録された地点へワープしますか?",
    /* T_WARP_ASK_KEYS        */ "  [%s]もう一度で OK   [%s]長押しで現在地を記録   [%s]でキャンセル",
    /* T_PICK_LOST_WINDOW   */ "戦闘を開始できませんでした。もう一度選んでください",
    /* T_PICK_FIELD_ONLY    */ "Battle Picker: フィールドでのみ使えます",
    /* T_PICK_CHOCOBO       */ "Battle Picker: チョコボに乗っている間は使えません",
    /* T_PICK_NO_OBJECT     */ "Battle Picker: この場所では使えません",
    /* T_PICK_NEVER_STARTED */ "Battle Picker: 戦闘が始まりませんでした。フィールドに戻します",
    /* T_PICK_TEACH_ONE     */ "Battle Picker: 未対応の戦闘です。ゾーン(%u)をメモして開発者へ連絡をお願いします",
    /* T_WARP_FIELD_ONLY      */ "WARP: 戦闘・イベント中は使用不可。移動可能なフィールドで使えます",
    /* T_WARP_NO_READ         */ "WARP: 必要な情報を読み取れません。リーダーを少し動かしてください",
    /* T_WARP_NO_POINT        */ "WARP: このゾーンには記録がありません。",
    /* T_WARP_WARPED          */ "WARP: 移動完了",
    /* T_WARP_NO_POINT_HOLD   */ "WARP: このゾーンは未記録です。%sの長押しで記録します",
    /* T_WARP_RECORDED        */ "WARP: このゾーンの地点を記録しました",
    /* T_WARP_CANCELLED       */ "WARP: キャンセルしました",
    /* T_BATTLE_OVL_ON      */ "戦闘中のタイマー&敵HP表示: ON",
    /* T_BATTLE_OVL_OFF     */ "戦闘中のタイマー&敵HP表示: OFF",
    /* T_HELP_HEAD          */ "ホットキー一覧  ([%s]=閉じる)",
    /* T_HELP_GAMESPEED     */ "ゲーム速度変更 — 押すたび %s の順で切替 (倍率はff13-srpractice.iniファイルにて変更可能)",
    /* T_HELP_KILL          */ "KILLモード — 押すたび 即勝利+一撃必殺→一撃必殺→OFF の順で切替",
    /* T_HELP_WARP          */ "ワープ — 長押し=現在地を記録 / 短押し2回=ワープ",
    /* T_HELP_SUPPRESS      */ "セーブデータロード時の敵の出現を抑止 — ロード後にONにした場合は、次のロードから有効",
    /* T_HELP_SAVE          */ "どこでもセーブ — クラウド同期をOFFにして使う",
    /* T_HELP_CAMERA        */ "カメラ固定解除 — どこでもセーブのデータロード後にカメラが固まった時に",
    /* T_HELP_BATTLE_OVL    */ "戦闘オーバーレイ — 戦闘中にタイマーと敵HPを表示",
    /* T_HELP_PICKER        */ "任意戦闘呼び出し — [%s/%s]=移動 [%s/%s]=ページ送り 長押しで決定(未表示なら前回分を即開始)。決定後は入力しない",
    /* T_HELP_BADKEY        */ "  ※iniの値が無効のため既定キー",
    /* T_HELP_BADCFG        */ "※iniに無効な設定があります: %s (既定値で動作中)",
    /* T_SAVE_FIELD_ONLY    */ "SAVE: 戦闘・イベント中は使用不可。移動可能なフィールドで使えます",
    /* T_SAVE_NO_READ       */ "SAVE: 必要な情報を読み取れません。リーダーを少し動かしてください",
};

#endif // FF13_OVL_TEXT_H
