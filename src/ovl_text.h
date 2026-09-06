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
    T_BATTLE_HEAD_TIME,   //  a battle-overlay GROUP HEADING: drawn on its own, no figure
    T_BATTLE_HEAD_TARGET,
    T_TIMER_GAME,         //  a LABEL: the figure is drawn beside it, right-aligned
    T_TIMER_REAL,         //  likewise
    T_ENEMY_HP,           //  likewise
    T_ATB_SHIFT,          //  likewise, and %d = that refill's period in whole seconds
    T_CHAIN_LEFT,         //  likewise: how long the target's chain gauge still holds
    T_ENEMY_ATB,          //  likewise: the target's ATB gauge, in segments
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
    T_SS_HEAD,            //  the menu's title line
    T_SS_NOTITLE,         //  the key was pressed where the menu must not open
    T_SS_CLOUD,           //  ...or while Steam Cloud is syncing this game's saves
    T_SS_GRP_SORT,        //  heading over the sort rows
    T_SS_GRP_RENUM,       //  ...and over the one that renames files
    T_SS_STORY_ASC,       //  the three sort modes, in menu order
    T_SS_STORY_DESC,
    T_SS_NUMBER,
    T_SS_GRP_FILTER,      //  heading over the row that picks what the list shows
    T_SS_FILTER_ALL,      //  that row, with no chapter picked
    T_SS_FILTER_ONE,      //  ...and with one: %d = chapter, %d = how many saves it holds
    T_SS_FILTER_KEYS,     //  the footer while that row is selected: two %s = left, right, confirm
    T_LABEL_PASTED,       //  the note was written; it shows the next time the list is drawn
    T_LABEL_CLEARED,      //  ...and the same for one taken off
    T_LABEL_NO_ROW,       //  the key was pressed with no save list on screen
    T_LABEL_NO_SAVE,      //  ...or on the save screen's "make a new save" row
    T_LABEL_NO_TEXT,      //  nothing on the clipboard to paste
    T_LABEL_BAD,          //  %d = the length limit
    T_LABEL_NO_WINDOW,    //  the editor could not take the game's window
    T_LABEL_EDIT_HEAD,    //  %d = the save being labelled
    T_LABEL_EDIT_HELP,    //  what can go in, and how
    T_LABEL_EDIT_KEYS,    //  what the editor's keys do: the confirm phrase, its own key
    T_LABEL_EDIT_FULL,    //  %d = the length limit; shown when a keystroke was refused
    T_SS_RENUM,           //  the last row: it renames files
    T_SS_FOOT,            //  four %s = up, down, the confirm phrase, close
    T_SS_DONE,            //  %d = saves re-stamped
    T_SS_RENUM_DONE,      //  %d = saves renumbered
    T_SS_RENUM_FAIL,
    T_SS_RENUM_LIST,      //  a renumber was asked for while the game's load list is open
    T_SS_RENUM_PART,      //  a save would not land; the journal stays and the next start finishes
    T_HELP_SORT,
    T_SS_GRP_CAT,         //  heading over the category rows
    T_CAT_COUNT,          //  appended to a category's name wherever it is offered: %d = its saves
    T_SS_CAT_PICK,        //  that group's first row: %s = the category in use, %s = the one picked
    T_SS_CAT_PICK_NONE,   //  ...and with no other category to switch to: %s = the one in use
    T_SS_CAT_PICK_IDLE,   //  ...and with others parked but none picked yet: %s = the one in use
    T_SS_CAT_NEW,         //  start a category seeded with a copy of the newest save
    T_SS_CAT_NEW_ALL,     //  ...seeded with a copy of every save in the one in use
    T_SS_CAT_RENAME,      //  ...and give the one in use another name
    T_SS_CAT_DEL,         //  ...and throw a parked one away: %s = the one picked
    T_SS_CAT_DEL_IDLE,    //  ...with none picked
    T_SS_CAT_DEL_ARM,     //  ...asking for the second confirm: %s = the one picked, %s = the phrase
    T_SS_CAT_DEL_KEYS,    //  the footer while that row is selected: left, right, confirm
    T_SS_CAT_DEL_DONE,    //  %s = the category deleted
    T_SS_CAT_DEL_FAIL,
    T_SS_CAT_KEYS,        //  the footer while the first row is selected: left, right, confirm
    T_SS_CAT_DONE,        //  %s = the category now in use, %d = files stashed, %d brought back
    T_SS_CAT_NOTITLE,     //  a switch was asked for somewhere the files must not move
    T_SS_CAT_FAIL,
    T_SS_CAT_RENAMED,     //  %s = the new name
    T_SS_CAT_EXISTS,      //  drawn inside the editor when the committed name is already taken
    T_SS_CAT_BADNAME,     //  ...and when the filesystem cannot take it
    T_SS_CAT_NAME_HEAD,   //  the editor's title, naming a new category
    T_SS_CAT_NAME_HEAD_ALL, //  ...one that copies every save
    T_SS_CAT_RENAME_HEAD, //  ...and renaming the one in use
    T_SS_CAT_EDIT_HELP,   //  what can go in a category's name, and how
    T_SS_CAT_EDIT_KEYS,   //  what the editor's keys do there: the confirm phrase, its own key
    T_SEND_NEWCAT,        //  the destination stop that stands for a category not made yet
    T_SEND_COPY,          //  the two rows: what the send does to the original, %s = where to
    T_SEND_MOVE,
    T_SEND_DEST_COPY,     //  what left/right picks, named for the row it is on
    T_SEND_DEST_MOVE,
    T_SEND_KEYS,          //  six %s = up, down, left, right, the confirm phrase, close
    T_SEND_FULL,          //  %s = the category with no free save number left
    T_SEND_COPIED,        //  %d = the save sent, %s = the category, %d = its number there
    T_SEND_MOVED,         //  likewise
    T_SEND_FAIL,
    T_LSEL_HEAD,          //  the save list chooser's title: %d = the save under the cursor, %s =
                          //  how to move that cursor (empty when list_prev/list_next are unbound)
    T_LSEL_PICK,          //  ...tacked onto that title: two %s = the keys that walk the game's list
    T_LSEL_NOTE,          //  the note row
    T_LSEL_KEYS,          //  its footer on the note row: up, down, the confirm phrase, its own key
    T_LSEL_SEND_KEYS,     //  ...on a send row: up, down, left, right, what they pick, confirm, key
    T_LSEL_MARK_SET,      //  the range row, idle: %d = the save it would mark as one end
    T_LSEL_MARK_CLR,      //  ...and armed
    T_SEND_COPY_N,        //  ...with a range armed: %d = the two ends, %d = the count, %s = where to
    T_SEND_MOVE_N,
    T_RANGE_MARKED,       //  %d = the save just marked
    T_RANGE_CLEARED,
    T_RANGE_GONE,         //  a marked or cursor save that no longer resolves to a file
    T_RANGE_ROW_GONE,     //  ...the CURSOR end only: the mark still stands
    T_SEND_N_FULL,        //  %s = destination, %d = its free numbers, %d = the count needed
    T_SEND_N_PART,        //  %d = sent, %d = the count asked for
    T_SEND_N_COPIED,      //  %d = the count, %s = the destination
    T_SEND_N_MOVED,       //  likewise
    T_LSEL_DEL,           //  the delete row over a range: %d = the two ends, %d = the count
    T_LSEL_DEL_IDLE,      //  ...with no range marked, when the row cannot run
    T_LSEL_DEL_ARM,       //  ...asking for the second confirm: %d = the count, %s = the phrase
    T_DEL_NEED_RANGE,     //  confirmed with no range marked: what to do first
    T_DEL_N_DONE,         //  %d = the count deleted
    T_DEL_FAIL,
    T_OK_HOLD,            //  the confirm phrase with menu_ok off: %s = the tidy key, held
    T_OK_BOTH,            //  ...and with menu_ok bound: %s = that key; the hold still confirms too
    T_HELP_MENU_OK,       //  the help row for menu_ok
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
    /* T_BATTLE_HEAD_TIME   */ "TIME",
    /* T_BATTLE_HEAD_TARGET */ "TARGET",
    /* T_TIMER_GAME         */ "GAME",
    /* T_TIMER_REAL         */ "REAL",
    /* T_ENEMY_HP           */ "HP",
    /* T_ATB_SHIFT          */ "ATB %ds",
    /* T_CHAIN_LEFT         */ "CHAIN",
    /* T_ENEMY_ATB          */ "ATB",
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
    /* T_HELP_PICKER        */ "Battle picker -- [%s/%s] = move  [%s/%s] = page  hold = start (closed: last pick); \navoid inputs after that",
    /* T_HELP_BADKEY        */ "  * bad ini value -> default key",
    /* T_HELP_BADCFG        */ "* bad values in the ini: %s (defaults are in force)",
    /* T_SAVE_FIELD_ONLY    */ "SAVE: not during a battle or an event. Use it where you can walk.",
    /* T_SAVE_NO_READ       */ "SAVE: cannot read what it needs yet. Move the leader a little.",
    /* T_SS_HEAD            */ "TIDY THE GAME'S SAVE LIST",
    /* T_SS_NOTITLE         */ "TIDY: title screen only",
    /* T_SS_CLOUD           */ "TIDY: this feature is available with Steam Cloud sync turned OFF for this game",
    /* T_SS_GRP_SORT        */ " Sort the list",
    /* T_SS_GRP_RENUM       */ " Renumber the saves",
    /* T_SS_STORY_ASC       */ "story order (Chapter 1 first)",
    /* T_SS_STORY_DESC      */ "story order (endgame first)",
    /* T_SS_NUMBER          */ "save number order",
    /* T_SS_GRP_FILTER      */ " Which saves the list shows",
    /* T_SS_FILTER_ALL      */ "every chapter",
    /* T_SS_FILTER_ONE      */ "Chapter %d only (%d saves)",
    /* T_SS_FILTER_KEYS     */ "  [%s/%s]=pick a chapter  %s=show every chapter",
    /* T_LABEL_PASTED       */ "Note saved. It appears once the save list is drawn again -- go back to the title screen, for instance. \nSaving over that save clears it.",
    /* T_LABEL_CLEARED      */ "Note taken off. It goes once the save list is drawn again -- go back to the title screen, for instance.",
    /* T_LABEL_NO_ROW       */ "NOTE: open the game's save list and put the cursor on a save first",
    /* T_LABEL_NO_SAVE      */ "NOTE: that row has no save yet -- it is the one that makes a new one",
    /* T_LABEL_NO_TEXT      */ "NOTE: nothing on the clipboard",
    /* T_LABEL_BAD          */ "NOTE: at most %d characters, and no comma",
    /* T_LABEL_NO_WINDOW    */ "NOTE: the game's window would not take the editor",
    /* T_LABEL_EDIT_HEAD    */ "NOTE ON SAVE %d",
    /* T_LABEL_EDIT_HELP    */ "  Type it, or paste text copied outside the game with Ctrl+V. Typing is ASCII only; a paste can be anything.",
    /* T_LABEL_EDIT_KEYS    */ "  %s=set   tap [%s]=cancel   (empty = no label)",
    /* T_LABEL_EDIT_FULL    */ "  that one did not go in: %d characters at most, and no comma",
    /* T_SS_RENUM           */ "into story order",
    /* T_SS_FOOT            */ "  [%s/%s]=move  %s=run  [%s/tap]=close",
    /* T_SS_DONE            */ "SORT: %d saves re-ordered",
    /* T_SS_RENUM_DONE      */ "RENUMBER: %d saves given new numbers",
    /* T_SS_RENUM_FAIL      */ "RENUMBER: a save could not be renamed -- nothing was changed",
    /* T_SS_RENUM_LIST      */ "RENUMBER: close the load list first -- its rows keep their old numbers",
    /* T_SS_RENUM_PART      */ "RENUMBER: a save could not be put down -- it will finish on the next start",
    /* T_HELP_SORT          */ "Save list -- on the title screen it opens the tidy menu; \non the save list it lists what can be done with the save under the cursor",
    /* T_SS_GRP_CAT         */ " Manage categories",
    /* T_CAT_COUNT          */ " (%d)",
    /* T_SS_CAT_PICK        */ "using '%s' -> switch to '%s'",
    /* T_SS_CAT_PICK_NONE   */ "using '%s' (no other category)",
    /* T_SS_CAT_PICK_IDLE   */ "using '%s'",
    /* T_SS_CAT_NEW         */ "new category with a copy of the latest save (stash the rest)",
    /* T_SS_CAT_NEW_ALL     */ "new category with a copy of every save (a twin of this one)",
    /* T_SS_CAT_RENAME      */ "rename the current category",
    /* T_SS_CAT_DEL         */ "delete '%s'",
    /* T_SS_CAT_DEL_IDLE    */ "delete a category (none picked)",
    /* T_SS_CAT_DEL_ARM     */ "confirm: delete '%s' -- %s again to run",
    /* T_SS_CAT_DEL_KEYS    */ "  [%s/%s]=pick a category to delete (not the one in use)  %s=delete",
    /* T_SS_CAT_DEL_DONE    */ "CATEGORY: deleted '%s' (to the Recycle Bin)",
    /* T_SS_CAT_DEL_FAIL    */ "CATEGORY: delete failed -- see the log",
    /* T_SS_CAT_KEYS        */ "  [%s/%s]=pick a category  %s=switch",
    /* T_SS_CAT_DONE        */ "CATEGORY: now on '%s' (%d out, %d in)",
    /* T_SS_CAT_NOTITLE     */ "CATEGORY: title screen only (close the save list first)",
    /* T_SS_CAT_FAIL        */ "CATEGORY: switch failed -- see the log",
    /* T_SS_CAT_RENAMED     */ "CATEGORY: renamed to '%s'",
    /* T_SS_CAT_EXISTS      */ "  A category with that name already exists -- change the name",
    /* T_SS_CAT_BADNAME     */ "  That name cannot be used -- change the name",
    /* T_SS_CAT_NAME_HEAD   */ "NAME THE NEW CATEGORY",
    /* T_SS_CAT_NAME_HEAD_ALL */ "NAME THE NEW CATEGORY (a copy of every save)",
    /* T_SS_CAT_RENAME_HEAD */ "RENAME THE CATEGORY",
    /* T_SS_CAT_EDIT_HELP   */ "  Type it, or paste text copied outside the game with Ctrl+V. These cannot go in: \\ / : * ? \" < > | ,",
    /* T_SS_CAT_EDIT_KEYS   */ "  %s=set   tap [%s]=cancel",
    /* T_SEND_NEWCAT        */ "<new category>",
    /* T_SEND_COPY          */ "copy -> %s",
    /* T_SEND_MOVE          */ "move -> %s",
    /* T_SEND_DEST_COPY     */ "where to copy",
    /* T_SEND_DEST_MOVE     */ "where to move",
    /* T_SEND_KEYS          */ "  [%s/%s]=choose  [%s/%s]=where to  %s=go  [%s]=close",
    /* T_SEND_FULL          */ "SEND: '%s' has no free save number",
    /* T_SEND_COPIED        */ "SEND: save %d copied to '%s' as save %d",
    /* T_SEND_MOVED         */ "SEND: save %d moved to '%s' as save %d (reopen the list to see it go)",
    /* T_SEND_FAIL          */ "SEND: failed -- see the log",
    /* T_LSEL_HEAD          */ "WHAT TO DO WITH SAVE %d%s",
    /* T_LSEL_PICK          */ "   ([%s/%s] = another save)",
    /* T_LSEL_NOTE          */ "write or edit it",
    /* T_LSEL_KEYS          */ "  [%s/%s]=choose  %s=go  tap [%s]=close",
    /* T_LSEL_SEND_KEYS     */ "  [%s/%s]=choose  [%s/%s]=%s  %s=go  tap [%s]=close",
    /* T_LSEL_MARK_SET      */ "select a range (save data %d is one end)",
    /* T_LSEL_MARK_CLR      */ "clear the range mark",
    /* T_SEND_COPY_N        */ "copy saves %d-%d as listed (%d of them) -> %s",
    /* T_SEND_MOVE_N        */ "move saves %d-%d as listed (%d of them) -> %s",
    /* T_RANGE_MARKED       */ "RANGE: starts at save data %d -- now put the cursor on the other end",
    /* T_RANGE_CLEARED      */ "RANGE: mark cleared",
    /* T_RANGE_GONE         */ "RANGE: the picked rows cannot be resolved -- mark cleared",
    /* T_RANGE_ROW_GONE    */ "RANGE: this row is gone -- put the cursor on a save that is still there",
    /* T_SEND_N_FULL        */ "SEND: '%s' has %d free number(s), %d needed",
    /* T_SEND_N_PART        */ "SEND: stopped at %d of %d -- see the log",
    /* T_SEND_N_COPIED      */ "SEND: %d saves copied to '%s'",
    /* T_SEND_N_MOVED       */ "SEND: %d saves moved to '%s' (reopen the list to see them go)",
    /* T_LSEL_DEL          */ "delete saves %d-%d as listed (%d of them)",
    /* T_LSEL_DEL_IDLE     */ "delete a range of saves (none marked)",
    /* T_LSEL_DEL_ARM      */ "confirm: delete %d save(s) -- %s again to run",
    /* T_DEL_NEED_RANGE    */ "DELETE: mark a range first -- the row above starts one",
    /* T_DEL_N_DONE        */ "DELETE: %d saves to the Recycle Bin (reopen the list to see them go)",
    /* T_DEL_FAIL          */ "DELETE: failed -- see the log",
    /* T_OK_HOLD           */ "hold [%s]",
    /* T_OK_BOTH           */ "[%s/hold]",
    /* T_HELP_MENU_OK      */ "Save menus -- run the selected row (the tidy key's hold works too)",
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
    /* T_BATTLE_HEAD_TIME   */ "TIME",
    /* T_BATTLE_HEAD_TARGET */ "TARGET",
    /* T_TIMER_GAME         */ "GAME",
    /* T_TIMER_REAL         */ "REAL",
    /* T_ENEMY_HP           */ "HP",
    /* T_ATB_SHIFT          */ "ATB %ds",
    /* T_CHAIN_LEFT         */ "CHAIN",
    /* T_ENEMY_ATB          */ "ATB",
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
    /* T_HELP_PICKER        */ "任意戦闘呼び出し — [%s/%s]=移動 [%s/%s]=ページ送り 長押しで決定(未表示なら前回分を即開始)。\n決定後は入力しないこと",
    /* T_HELP_BADKEY        */ "  ※iniの値が無効のため既定キー",
    /* T_HELP_BADCFG        */ "※iniに無効な設定があります: %s (既定値で動作中)",
    /* T_SAVE_FIELD_ONLY    */ "SAVE: 戦闘・イベント中は使用不可。移動可能なフィールドで使えます",
    /* T_SAVE_NO_READ       */ "SAVE: 必要な情報を読み取れません。リーダーを少し動かしてください",
    /* T_SS_HEAD            */ "セーブデータの整理",
    /* T_SS_NOTITLE         */ "セーブデータの整理: タイトル画面でのみ開けます",
    /* T_SS_CLOUD           */ "セーブデータの整理: この機能は、Steam Cloud同期設定をOFFにすることで使用可能です",
    /* T_SS_GRP_SORT        */ " 一覧の並べ替え",
    /* T_SS_GRP_RENUM       */ " セーブ番号の振り直し",
    /* T_SS_STORY_ASC       */ "進行順 (1章が先頭)",
    /* T_SS_STORY_DESC      */ "進行順 (終盤が先頭)",
    /* T_SS_NUMBER          */ "セーブ番号順",
    /* T_SS_GRP_FILTER      */ " セーブデータ一覧に表示する範囲",
    /* T_SS_FILTER_ALL      */ "すべての章",
    /* T_SS_FILTER_ONE      */ "第%d章のみ (%d件)",
    /* T_SS_FILTER_KEYS     */ "  [%s/%s]=章を選ぶ  %s=すべての章に戻す",
    /* T_LABEL_PASTED       */ "メモを保存しました。一旦タイトル画面に戻るなどでセーブ一覧を再表示すると表示されます。\nセーブデータを上書き保存するとメモは消えます。",
    /* T_LABEL_CLEARED      */ "メモを外しました。一旦タイトル画面に戻るなどでセーブ一覧を再表示すると反映されます。",
    /* T_LABEL_NO_ROW       */ "LABEL: セーブ一覧を開き、対象にカーソルを合わせてから押してください",
    /* T_LABEL_NO_SAVE      */ "LABEL: この行にはまだセーブがありません(新規セーブデータを作成する行です)",
    /* T_LABEL_NO_TEXT      */ "LABEL: クリップボードに文字列がありません",
    /* T_LABEL_BAD          */ "LABEL: %d文字まで。, は使えません",
    /* T_LABEL_NO_WINDOW    */ "LABEL: ゲームのウィンドウを取得できませんでした",
    /* T_LABEL_EDIT_HEAD    */ "セーブ%d のメモ",
    /* T_LABEL_EDIT_HELP    */ "  入力するか、ゲーム外でコピーしたテキストを Ctrl+V で貼り付け。手入力は半角のみ。ペーストであれば全角やマルチバイトも可",
    /* T_LABEL_EDIT_KEYS    */ "  %s=決定   [%s]短押し=中止   (空にすると外す)",
    /* T_LABEL_EDIT_FULL    */ "  今の入力は無効: %d文字まで、, は使えません",
    /* T_SS_RENUM           */ "進行順に振り直す",
    /* T_SS_FOOT            */ "  [%s/%s]=選択  %s=実行  [%s/短押し]=閉じる",
    /* T_SS_DONE            */ "SORT: セーブ%d件を並べ替えました",
    /* T_SS_RENUM_DONE      */ "RENUMBER: セーブ%d件の番号を振り直しました",
    /* T_SS_RENUM_FAIL      */ "RENUMBER: リネームできないファイルがあったため、何も変更していません",
    /* T_SS_RENUM_LIST      */ "RENUMBER: ロード一覧を閉じてから実行してください (開いている一覧は古い番号のままのため)",
    /* T_SS_RENUM_PART      */ "RENUMBER: 置き直せないファイルがあったため、次回起動時に完了します",
    /* T_HELP_SORT          */ "セーブデータの整理 — タイトル画面では整理メニュー、\nセーブ一覧ではカーソル上のデータへの操作メニュー",
    /* T_SS_GRP_CAT         */ " カテゴリー管理",
    /* T_CAT_COUNT          */ " (%d件)",
    /* T_SS_CAT_PICK        */ "使用中: %s → 「%s」に切替",
    /* T_SS_CAT_PICK_NONE   */ "使用中: %s (他のカテゴリーなし)",
    /* T_SS_CAT_PICK_IDLE   */ "使用中: %s",
    /* T_SS_CAT_NEW         */ "新しいカテゴリーを作って切替 (最新の1件をコピー)",
    /* T_SS_CAT_NEW_ALL     */ "新しいカテゴリーを作って切替 (全件コピー)",
    /* T_SS_CAT_RENAME      */ "使用中のカテゴリーの名前を変更",
    /* T_SS_CAT_DEL         */ "「%s」を削除",
    /* T_SS_CAT_DEL_IDLE    */ "カテゴリーを削除 (未選択)",
    /* T_SS_CAT_DEL_ARM     */ "削除の確認: 「%s」 — もう一度%sで実行",
    /* T_SS_CAT_DEL_KEYS    */ "  [%s/%s]=削除するカテゴリーを選ぶ (使用中は削除不可)  %s=削除",
    /* T_SS_CAT_DEL_DONE    */ "カテゴリー: 「%s」を削除しました (ごみ箱へ)",
    /* T_SS_CAT_DEL_FAIL    */ "カテゴリー削除: 失敗 (ログ参照)",
    /* T_SS_CAT_KEYS        */ "  [%s/%s]=切替先を選ぶ  %s=切替",
    /* T_SS_CAT_DONE        */ "カテゴリー切替: 「%s」へ (退避%d件 / 復帰%d件)",
    /* T_SS_CAT_NOTITLE     */ "カテゴリー切替: タイトル画面でのみ実行できます (一覧は閉じてから)",
    /* T_SS_CAT_FAIL        */ "カテゴリー切替: 失敗 (ログ参照)",
    /* T_SS_CAT_RENAMED     */ "カテゴリー: 「%s」に改名",
    /* T_SS_CAT_EXISTS      */ "  同名のカテゴリーがあります。名前を変えてください",
    /* T_SS_CAT_BADNAME     */ "  その名前は使えません。名前を変えてください",
    /* T_SS_CAT_NAME_HEAD   */ "新しいカテゴリーの名前",
    /* T_SS_CAT_NAME_HEAD_ALL */ "新しいカテゴリーの名前 (全件コピー)",
    /* T_SS_CAT_RENAME_HEAD */ "カテゴリーの新しい名前",
    /* T_SS_CAT_EDIT_HELP   */ "  入力するか、ゲーム外でコピーしたテキストを Ctrl+V で貼り付け。使えない文字: \\ / : * ? \" < > | ,",
    /* T_SS_CAT_EDIT_KEYS   */ "  %s=決定   [%s]短押し=中止",
    /* T_SEND_NEWCAT        */ "<新しいカテゴリー>",
    /* T_SEND_COPY          */ "コピー → %s",
    /* T_SEND_MOVE          */ "移動 → %s",
    /* T_SEND_DEST_COPY     */ "コピー先カテゴリー",
    /* T_SEND_DEST_MOVE     */ "移動先カテゴリー",
    /* T_SEND_KEYS          */ "  [%s/%s]=選択  [%s/%s]=送り先  %s=実行  [%s]=閉じる",
    /* T_SEND_FULL          */ "送り先「%s」に空き番号がありません",
    /* T_SEND_COPIED        */ "セーブ%dを「%s」のセーブ%dへコピーしました",
    /* T_SEND_MOVED         */ "セーブ%dを「%s」のセーブ%dへ移動しました (一覧は開き直すと反映)",
    /* T_SEND_FAIL          */ "送り: 失敗 (ログ参照)",
    /* T_LSEL_HEAD          */ "セーブ%dの操作%s",
    /* T_LSEL_PICK          */ "   ([%s/%s]=別のセーブを選ぶ)",
    /* T_LSEL_NOTE          */ "メモを書く/編集する",
    /* T_LSEL_KEYS          */ "  [%s/%s]=選択  %s=実行  [%s]短押し=閉じる",
    /* T_LSEL_SEND_KEYS     */ "  [%s/%s]=選択  [%s/%s]=%s  %s=実行  [%s]短押し=閉じる",
    /* T_LSEL_MARK_SET      */ "範囲選択 (セーブデータ%dを始点にする)",
    /* T_LSEL_MARK_CLR      */ "範囲選択を解除",
    /* T_SEND_COPY_N        */ "コピー (一覧上のセーブ%d〜%dの%d件) → %s",
    /* T_SEND_MOVE_N        */ "移動 (一覧上のセーブ%d〜%dの%d件) → %s",
    /* T_RANGE_MARKED       */ "範囲の始点: セーブデータ%d — もう一方の端にカーソルを移す",
    /* T_RANGE_CLEARED      */ "範囲選択を解除しました",
    /* T_RANGE_GONE         */ "範囲を特定できません — 解除しました",
    /* T_RANGE_ROW_GONE    */ "この行はもうありません — 実在するセーブへカーソルを移してください",
    /* T_SEND_N_FULL        */ "送り先「%s」の空きは%d件 — %d件には足りません",
    /* T_SEND_N_PART        */ "送り: %d/%d件で停止 (ログ参照)",
    /* T_SEND_N_COPIED      */ "セーブ%d件を「%s」へコピーしました",
    /* T_SEND_N_MOVED       */ "セーブ%d件を「%s」へ移動しました (一覧は開き直すと反映)",
    /* T_LSEL_DEL          */ "削除 (一覧上のセーブ%d〜%dの%d件)",
    /* T_LSEL_DEL_IDLE     */ "範囲を削除 (先に範囲選択が必要)",
    /* T_LSEL_DEL_ARM      */ "削除の確認: %d件 — もう一度%sで実行",
    /* T_DEL_NEED_RANGE    */ "削除: 先に上の「範囲選択」の行で始点を決めてください",
    /* T_DEL_N_DONE        */ "セーブ%d件をごみ箱へ削除しました (一覧は開き直すと反映)",
    /* T_DEL_FAIL          */ "削除: 失敗 (ログ参照)",
    /* T_OK_HOLD           */ "[%s]長押し",
    /* T_OK_BOTH           */ "[%s/長押し]",
    /* T_HELP_MENU_OK      */ "セーブ整理メニューの決定 (整理キーの長押しでも可)",
};

#endif // FF13_OVL_TEXT_H
