// FF13-SpeedrunPractice -- everything the overlay says, in both languages.
//
// Copyright (C) 2026 Akiyoshi
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one file in the tracked tree allowed to hold Japanese; everything else stays English.
// The overlay text page is generated from the two tables below.
//
// Constraints -- each corrupts a line rather than merely looking wrong:
//   1. A Japanese string must carry the same %-conversions as its English one: same count, same
//      types, same ORDER. snprintf reads its arguments off the format string, and there is no
//      positional form here, so reword rather than reorder.
//   1b. One entry per line. A split C string is legal but this file's checker refuses it, and the
//      page generator cannot read it. Long is fine; wrapped is not.
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
    T_KILL_OFF_FLASH,     //  flash: the cycle reached OFF (ON is said by the status line)
    T_GAMESPEED,          //  %s = key label, %.2g = factor
    T_GAMESPEED_OFF,      //  flash, likewise
    T_SUPPRESS,           //  %s = key label
    T_SUPPRESS_OFF,       //  flash, likewise
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
    T_HELP_BADKEY,        //  appended to a row whose ini binding did not parse
    T_HELP_BADCFG,        //  %s = the refused setting names, from cfg_bad_list()
    T_SAVE_FIELD_ONLY,    //  save hotkey refused: not in FIELD_IDLE (same reason as the warp's)
    T_SAVE_NO_READ,       //  save hotkey refused: leader position unreadable
    T_SS_HEAD,            //  the menu's title line
    T_SS_NOTITLE,         //  the key was pressed where the menu must not open
    T_SS_CLOUD,           //  ...or while Steam Cloud is syncing this game's saves
    T_SS_GRP_SORT,        //  heading over the row that picks the order
    T_SS_GRP_RENUM,       //  ...and over the one that renames files
    T_SS_ORDER_DATE,      //  the four orders, in the order the arrows walk them
    T_SS_ORDER_ASC,
    T_SS_ORDER_DESC,
    T_SS_ORDER_NUMBER,
    T_SS_ORDER_KEYS,      //  the footer while that row is selected: two %s = left, right
    T_SS_GRP_FILTER,      //  heading over the row that picks what the list shows
    T_SS_FILTER_ALL,      //  that row, with no chapter picked
    T_SS_FILTER_ONE,      //  ...and with one: %d = chapter, %d = how many saves it holds
    T_SS_FILTER_KEYS,     //  the footer while that row is selected: two %s = left, right, confirm
    T_LABEL_PASTED,       //  the note was written; it shows the next time the list is drawn
    T_LABEL_CLEARED,      //  ...and the same for one taken off
    T_LABEL_NO_ROW,       //  the key was pressed with no save list on screen
    T_LABEL_NO_SAVE,      //  ...or on the save screen's "make a new save" row
    T_LABEL_NO_WINDOW,    //  the editor could not take the game's window
    T_LABEL_EDIT_HEAD,    //  %d = the save being labelled
    T_LABEL_EDIT_HELP,    //  what can go in, and how
    T_LABEL_EDIT_KEYS,    //  what the editor's keys do: the confirm phrase, its own key
    T_LABEL_EDIT_FULL,    //  %d = the length limit; shown when a keystroke was refused
    T_SS_RENUM,           //  the last row: it renames files
    T_SS_RENUM_ARM,       //  ...asking for the second confirm: %d = saves that would move, %s = the phrase
    T_SS_FOOT,            //  four %s = up, down, the confirm phrase, close
    T_SS_RENUM_DONE,      //  %d = saves renumbered
    T_SS_RENUM_FAIL,
    T_SS_RENUM_LIST,      //  a renumber was asked for while the game's load list is open
    T_SS_RENUM_PART,      //  a save would not land; the journal stays and the next start finishes
    T_SS_GRP_CAT,         //  heading over the category rows
    T_CAT_COUNT,          //  appended to a category's name wherever it is offered: %d = its saves
    T_SS_CAT_PICK,        //  that group's first row: %s = the one picked, %s = the category in use
    T_SS_CAT_PICK_NONE,   //  ...and with no other category to switch to: %s = the one in use
    T_SS_CAT_UNPICKED,    //  the switch row's value while none is picked
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
    T_OK_KEY,             //  the confirm phrase: %s = menu_ok
    T_SET_TITLE,          //  the settings screen's title: %s = help, up, down, left, right, then the confirm phrase (tidy_ok_phrase)
    T_SET_HEAD_KEYS,      //  heading over the hotkey rows
    T_SET_HEAD_SET,       //  heading over the setting rows
    T_SET_L_SPEED,        //  a hotkey row's name, also named in the "used by" flash
    T_SET_L_KILL,
    T_SET_L_WARP,
    T_SET_L_SUPPRESS,
    T_SET_L_SAVE,
    T_SET_L_CAMERA,
    T_SET_L_TIDY,
    T_SET_L_BATTLE,
    T_SET_L_PICKER,
    T_SET_L_HELP,
    T_SET_KEYS_LABEL,     //  the menu-key row's lead-in; the eight keys follow it
    T_SET_SUB_UP,         //  an item of that row, drawn as name[key]
    T_SET_SUB_DOWN,
    T_SET_SUB_LEFT,
    T_SET_SUB_RIGHT,
    T_SET_SUB_CLOSE,
    T_SET_SUB_OK,
    T_SET_SUB_LIST,       //  the list_prev item; list_next follows it with no name
    T_SET_FN_UP,          //  a menu key as the "used by" flash names it
    T_SET_FN_DOWN,
    T_SET_FN_LEFT,
    T_SET_FN_RIGHT,
    T_SET_FN_CLOSE,
    T_SET_FN_OK,
    T_SET_FN_LPREV,
    T_SET_FN_LNEXT,
    T_SET_L_FOCUS,        //  a setting row's name; ": value" follows it
    T_SET_L_MULT,
    T_SET_L_BGM,
    T_SET_L_MARK,
    T_SET_L_KEEPNOTE,
    T_SET_L_SAVEDIR,
    T_SET_L_TOP,
    T_SET_L_LOG,
    T_SET_V_ON,           //  a setting's value
    T_SET_V_OFF,
    T_SET_V_NONE,         //  the mark row with no mark
    T_SET_V_AUTODIR,      //  the save-folder row with save_dir empty
    T_SET_V_MS,           //  %d = milliseconds
    T_SET_V_PX,           //  %d = pixels
    T_SET_V_MULT_IN,      //  a factor candidate in the list: %s = the factor
    T_SET_V_MULT_OUT,     //  ...and one not in it
    T_SET_F_SPEED,        //  footer for the selected row: %s = the cycle, live from gs_steps_arrow()
    T_SET_F_KILL,
    T_SET_F_WARP,
    T_SET_F_SUPPRESS,
    T_SET_F_SAVE,
    T_SET_F_CAMERA,
    T_SET_F_TIDY,
    T_SET_F_BATTLE,
    T_SET_F_PICKER,       //  four %s = picker up, down, left, right
    T_SET_F_HELP,
    T_SET_F_KEYS,         //  %s = picker left, right, then the confirm phrase
    T_SET_F_FOCUS,
    T_SET_F_MULT,         //  %s = picker left, right, the confirm phrase, then the cycle
    T_SET_F_BGM,
    T_SET_F_MARK,
    T_SET_F_KEEPNOTE,
    T_SET_F_SAVEDIR,
    T_SET_F_TOP,
    T_SET_F_LOG,
    T_SET_F_CAPTURE,      //  while a key row waits for its key: %s = help, %s = cancel
    T_SET_F_DUP,          //  put in front of the footer of a row whose key another function also has
    T_SET_CANT_UNBIND_HELP,//  flash
    T_SET_CANT_UNBIND_OK, //  flash
    T_SET_USED,           //  flash: %s = the key, %s = the function that has it (a T_SET_L_* or T_SET_FN_* name)
    T_SET_OK_IS_TIDY,     //  flash
    T_SET_TIDY_IS_OK,     //  flash
    T_SET_MULT_MIN,       //  flash
    T_SET_MULT_MAX,       //  flash
    T_SET_INI_FAIL,       //  flash: the ini write failed
    T_SET_HINT_CHANGE,    //  dim, after a key row under the cursor: %s = the confirm phrase (tidy_ok_phrase)
    T_COUNT
} ovl_msg;

static const char* const kOvlEn[T_COUNT] = {
    /* T_KILL_LINE          */ "(%s) %s: ON",
    /* T_KILL_INSTANT       */ "INSTANT WIN AT BATTLE START + ONE-HIT KILL",
    /* T_KILL_ONEHIT        */ "ONE-HIT KILL",
    /* T_KILL_OFF           */ "OFF",
    /* T_KILL_OFF_FLASH     */ "KILL MODE: OFF",
    /* T_GAMESPEED          */ "(%s) GAME SPEED %.2gx: ON",
    /* T_GAMESPEED_OFF      */ "GAME SPEED: OFF",
    /* T_SUPPRESS           */ "(%s) SUPPRESS ENEMY SPAWNS AT SAVE LOAD: ON",
    /* T_SUPPRESS_OFF       */ "SUPPRESS ENEMY SPAWNS AT SAVE LOAD: OFF",
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
    /* T_HELP_BADKEY        */ "  * bad ini value -> default key",
    /* T_HELP_BADCFG        */ "* bad values in the ini: %s (defaults are in force)",
    /* T_SAVE_FIELD_ONLY    */ "SAVE: not during a battle or an event. Use it where you can walk.",
    /* T_SAVE_NO_READ       */ "SAVE: cannot read what it needs yet. Move the leader a little.",
    /* T_SS_HEAD            */ "TIDY THE GAME'S SAVE LIST",
    /* T_SS_NOTITLE         */ "TIDY: title screen or a save list only",
    /* T_SS_CLOUD           */ "TIDY: this feature is available with Steam Cloud sync turned OFF for this game",
    /* T_SS_GRP_SORT        */ " Sort order",
    /* T_SS_GRP_RENUM       */ " Renumber the saves",
    /* T_SS_ORDER_DATE      */ "save date",
    /* T_SS_ORDER_ASC       */ "story order (earliest first)",
    /* T_SS_ORDER_DESC      */ "story order (endgame first)",
    /* T_SS_ORDER_NUMBER    */ "by save number",
    /* T_SS_ORDER_KEYS      */ "  [%s/%s]=pick the order (it applies the next time the list opens)",
    /* T_SS_GRP_FILTER      */ " Which saves the list shows",
    /* T_SS_FILTER_ALL      */ "every chapter",
    /* T_SS_FILTER_ONE      */ "Chapter %d only (%d saves)",
    /* T_SS_FILTER_KEYS     */ "  [%s/%s]=pick a chapter  %s=show every chapter",
    /* T_LABEL_PASTED       */ "Note saved. It appears once the save list is drawn again -- go back to the title screen, for instance.",
    /* T_LABEL_CLEARED      */ "Note taken off. It goes once the save list is drawn again -- go back to the title screen, for instance.",
    /* T_LABEL_NO_ROW       */ "NOTE: open the game's save list and put the cursor on a save first",
    /* T_LABEL_NO_SAVE      */ "NOTE: this is the slot that makes a new save, so it cannot be picked",
    /* T_LABEL_NO_WINDOW    */ "NOTE: the game's window would not take the editor",
    /* T_LABEL_EDIT_HEAD    */ "NOTE ON SAVE %d",
    /* T_LABEL_EDIT_HELP    */ "  Type it, or paste text copied outside the game with Ctrl+V. Typing is ASCII only; a paste can be anything.",
    /* T_LABEL_EDIT_KEYS    */ "  %s=set   [%s]=cancel   (empty = no note)",
    /* T_LABEL_EDIT_FULL    */ "  that did not go in: %d characters at most, and no comma",
    /* T_SS_RENUM           */ "renumber into story order",
    /* T_SS_RENUM_ARM       */ "confirm: renumber %d save(s) -- %s again to run",
    /* T_SS_FOOT            */ "  [%s/%s]=move  %s=run  [%s/%s]=close",
    /* T_SS_RENUM_DONE      */ "RENUMBER: %d saves renumbered into story order",
    /* T_SS_RENUM_FAIL      */ "RENUMBER: a save could not be renamed -- nothing was changed",
    /* T_SS_RENUM_LIST      */ "RENUMBER: close the load list first -- its rows keep their old numbers",
    /* T_SS_RENUM_PART      */ "RENUMBER: stopped partway -- it will finish on the next start",
    /* T_SS_GRP_CAT         */ " Manage categories",
    /* T_CAT_COUNT          */ " (%d)",
    /* T_SS_CAT_PICK        */ "switch to %s (using \"%s\")",
    /* T_SS_CAT_PICK_NONE   */ "using \"%s\" (no other category)",
    /* T_SS_CAT_UNPICKED    */ "----",
    /* T_SS_CAT_NEW         */ "new category with a copy of the latest save (stash the rest)",
    /* T_SS_CAT_NEW_ALL     */ "new category with a copy of every save (a twin of this one)",
    /* T_SS_CAT_RENAME      */ "rename the current category",
    /* T_SS_CAT_DEL         */ "delete %s",
    /* T_SS_CAT_DEL_IDLE    */ "delete a category (none picked)",
    /* T_SS_CAT_DEL_ARM     */ "confirm: delete \"%s\" -- %s again to run",
    /* T_SS_CAT_DEL_KEYS    */ "  [%s/%s]=pick a category to delete (not the one in use)  %s=delete",
    /* T_SS_CAT_DEL_DONE    */ "CATEGORY: deleted \"%s\" (to the Recycle Bin)",
    /* T_SS_CAT_DEL_FAIL    */ "CATEGORY: delete failed -- see the log",
    /* T_SS_CAT_KEYS        */ "  [%s/%s]=pick a category  %s, leaving the row or closing=switch",
    /* T_SS_CAT_DONE        */ "CATEGORY: now on \"%s\" (%d out, %d in)",
    /* T_SS_CAT_NOTITLE     */ "CATEGORY: title screen only (close the save list first)",
    /* T_SS_CAT_FAIL        */ "CATEGORY: switch failed -- see the log",
    /* T_SS_CAT_RENAMED     */ "CATEGORY: renamed to \"%s\"",
    /* T_SS_CAT_EXISTS      */ "  A category with that name already exists -- change the name",
    /* T_SS_CAT_BADNAME     */ "  That name cannot be used -- change the name",
    /* T_SS_CAT_NAME_HEAD   */ "NAME THE NEW CATEGORY",
    /* T_SS_CAT_NAME_HEAD_ALL */ "NAME THE NEW CATEGORY (a copy of every save)",
    /* T_SS_CAT_RENAME_HEAD */ "RENAME THE CATEGORY",
    /* T_SS_CAT_EDIT_HELP   */ "  Type it, or paste text copied outside the game with Ctrl+V. These cannot go in: \\ / : * ? \" < > | ,",
    /* T_SS_CAT_EDIT_KEYS   */ "  %s=set   [%s]=cancel",
    /* T_SEND_NEWCAT        */ "<new category>",
    /* T_SEND_COPY          */ "copy -> %s",
    /* T_SEND_MOVE          */ "move -> %s",
    /* T_SEND_DEST_COPY     */ "where to copy",
    /* T_SEND_DEST_MOVE     */ "where to move",
    /* T_SEND_FULL          */ "SEND: \"%s\" has no free save number",
    /* T_SEND_COPIED        */ "SEND: save %d copied to \"%s\" as save %d",
    /* T_SEND_MOVED         */ "SEND: save %d moved to \"%s\" as save %d. The list shows it once reopened (e.g. after going back to the title screen)",
    /* T_SEND_FAIL          */ "SEND: failed -- see the log",
    /* T_LSEL_HEAD          */ "WHAT TO DO WITH SAVE %d%s",
    /* T_LSEL_PICK          */ "   ([%s/%s] = another save)",
    /* T_LSEL_NOTE          */ "write or edit it",
    /* T_LSEL_KEYS          */ "  [%s/%s]=choose  %s=go  [%s]=close",
    /* T_LSEL_SEND_KEYS     */ "  [%s/%s]=choose  [%s/%s]=%s  %s=go  [%s]=close",
    /* T_LSEL_MARK_SET      */ "select a range (save %d is one end)",
    /* T_LSEL_MARK_CLR      */ "clear the range mark",
    /* T_SEND_COPY_N        */ "copy saves %d-%d as listed (%d of them) -> %s",
    /* T_SEND_MOVE_N        */ "move saves %d-%d as listed (%d of them) -> %s",
    /* T_RANGE_MARKED       */ "RANGE: starts at save %d -- now put the cursor on the other end",
    /* T_RANGE_CLEARED      */ "RANGE: mark cleared",
    /* T_RANGE_GONE         */ "RANGE: the picked rows cannot be resolved -- mark cleared",
    /* T_RANGE_ROW_GONE    */ "RANGE: this save is gone -- put the cursor on a save that is still there",
    /* T_SEND_N_FULL        */ "SEND: \"%s\" has %d free number(s), %d needed",
    /* T_SEND_N_PART        */ "SEND: stopped at %d of %d -- see the log",
    /* T_SEND_N_COPIED      */ "SEND: %d saves copied to \"%s\"",
    /* T_SEND_N_MOVED       */ "SEND: %d saves moved to \"%s\" (reopen the list to see them go)",
    /* T_LSEL_DEL          */ "delete saves %d-%d as listed (%d of them)",
    /* T_LSEL_DEL_IDLE     */ "delete a range of saves (none marked)",
    /* T_LSEL_DEL_ARM      */ "confirm: delete %d save(s) -- %s again to run",
    /* T_DEL_NEED_RANGE    */ "DELETE: mark a range first -- the row above starts one",
    /* T_DEL_N_DONE        */ "DELETE: %d saves to the Recycle Bin. The list shows it once reopened (e.g. after going back to the title screen)",
    /* T_DEL_FAIL          */ "DELETE: failed -- see the log",
    /* T_OK_KEY            */ "[%s]",
    /* T_SET_TITLE          */ "HOTKEYS & SETTINGS  ([%s] = close  [%s/%s] = pick  [%s/%s] = change  %s = assign a key)",
    /* T_SET_HEAD_KEYS      */ "HOTKEYS",
    /* T_SET_HEAD_SET       */ "SETTINGS",
    /* T_SET_L_SPEED        */ "GAME SPEED",
    /* T_SET_L_KILL         */ "KILL MODE",
    /* T_SET_L_WARP         */ "Warp",
    /* T_SET_L_SUPPRESS     */ "SUPPRESS ENEMY SPAWNS AT SAVE LOAD",
    /* T_SET_L_SAVE         */ "Save anywhere",
    /* T_SET_L_CAMERA       */ "Camera unstick",
    /* T_SET_L_TIDY         */ "TIDY THE GAME'S SAVE LIST",
    /* T_SET_L_BATTLE       */ "Battle overlay",
    /* T_SET_L_PICKER       */ "Battle picker",
    /* T_SET_L_HELP         */ "HOTKEYS & SETTINGS (this screen)",
    /* T_SET_KEYS_LABEL     */ "Picker/menu keys:",
    /* T_SET_SUB_UP         */ "up",
    /* T_SET_SUB_DOWN       */ "down",
    /* T_SET_SUB_LEFT       */ "left",
    /* T_SET_SUB_RIGHT      */ "right",
    /* T_SET_SUB_CLOSE      */ "close",
    /* T_SET_SUB_OK         */ "confirm",
    /* T_SET_SUB_LIST       */ "list",
    /* T_SET_FN_UP          */ "picker up",
    /* T_SET_FN_DOWN        */ "picker down",
    /* T_SET_FN_LEFT        */ "picker left",
    /* T_SET_FN_RIGHT       */ "picker right",
    /* T_SET_FN_CLOSE       */ "close",
    /* T_SET_FN_OK          */ "confirm",
    /* T_SET_FN_LPREV       */ "list prev",
    /* T_SET_FN_LNEXT       */ "list next",
    /* T_SET_L_FOCUS        */ "Hotkeys while another window is active",
    /* T_SET_L_MULT         */ "GAME SPEED factors",
    /* T_SET_L_BGM          */ "Battle picker music start timing",
    /* T_SET_L_MARK         */ "Text added to save-anywhere saves",
    /* T_SET_L_KEEPNOTE     */ "Keep a save's note when saving over it",
    /* T_SET_L_SAVEDIR      */ "Save folder",
    /* T_SET_L_TOP          */ "Overlay starts this far down",
    /* T_SET_L_LOG          */ "Log level",
    /* T_SET_V_ON           */ "ON",
    /* T_SET_V_OFF          */ "OFF",
    /* T_SET_V_NONE         */ "none",
    /* T_SET_V_AUTODIR      */ "auto-detect",
    /* T_SET_V_MS           */ "%d ms",
    /* T_SET_V_PX           */ "%d px",
    /* T_SET_V_MULT_IN      */ "[%s]",
    /* T_SET_V_MULT_OUT     */ " %s ",
    /* T_SET_F_SPEED        */ "Each press: %s in turn. The factors are set on the \"GAME SPEED factors\" row",
    /* T_SET_F_KILL         */ "Each press: instant win + one-hit kill, one-hit kill, OFF, in turn",
    /* T_SET_F_WARP         */ "hold = record here / tap twice = warp",
    /* T_SET_F_SUPPRESS     */ "Each press: ON/OFF. Turned ON after a load, it works from the next load",
    /* T_SET_F_SAVE         */ "turn Steam Cloud sync OFF to use it",
    /* T_SET_F_CAMERA       */ "when the camera jams after loading a save-anywhere save",
    /* T_SET_F_TIDY         */ "on the title screen, the save-tidy menu; on a save list, the menu for each save. Each press shows or hides it",
    /* T_SET_F_BATTLE       */ "shows the timer and the enemy's HP, ATB and chain in battle. Each press shows or hides it",
    /* T_SET_F_PICKER       */ "[%s/%s] = move  [%s/%s] = page  hold = start (closed: last pick); avoid inputs after that",
    /* T_SET_F_HELP         */ "shows or hides this screen; the ini's help key sets it too; it cannot be unbound. Each press shows or hides it",
    /* T_SET_F_KEYS         */ "[%s/%s] = pick an item  %s = assign a key",
    /* T_SET_F_FOCUS        */ "ON = the hotkeys fire even while another window is in front",
    /* T_SET_F_MULT         */ "[%s/%s] = pick  %s = in or out; up to 8 of them, stepped in ascending order: %s",
    /* T_SET_F_BGM          */ "0 to 8000 ms in steps of 100; 0 brings the music in at once",
    /* T_SET_F_MARK         */ "shown after the save's number in the game's list, on saves made from now on; other marks can be typed into the ini",
    /* T_SET_F_KEEPNOTE     */ "On: a note stays on a save you save over. Off: saving over clears it.",
    /* T_SET_F_SAVEDIR      */ "the folder holding ff13-NN.dat; set with save_dir in the ini (empty = auto-detect)",
    /* T_SET_F_TOP          */ "0 to 400 px in steps of 8; move it down when an FPS counter or another overlay covers the top lines",
    /* T_SET_F_LOG          */ "quiet = errors and startup only / normal = + your actions' results / verbose = + the game's internals",
    /* T_SET_F_CAPTURE      */ "Press the new key (the current key again = unbind; [%s] or [%s] = cancel)",
    /* T_SET_F_DUP          */ "! the same key as another function -- ",
    /* T_SET_CANT_UNBIND_HELP */ "the key that opens this screen cannot be unbound",
    /* T_SET_CANT_UNBIND_OK */ "the confirm key cannot be unbound (the menus have no other way to confirm)",
    /* T_SET_USED           */ "%s is used by \"%s\"",
    /* T_SET_OK_IS_TIDY     */ "the tidy key cannot be the confirm key (its press closes the menu)",
    /* T_SET_TIDY_IS_OK     */ "the confirm key cannot be the tidy key (its press closes the menu)",
    /* T_SET_MULT_MIN       */ "this factor cannot be taken out: at least one is needed",
    /* T_SET_MULT_MAX       */ "this factor cannot be put in: 8 at most",
    /* T_SET_INI_FAIL       */ "could not save the ini file; restarting the game brings back the setting from before the change",
    /* T_SET_HINT_CHANGE    */ "  (%s to change)",
};

// UTF-8. "" = not translated, falls back to the English above.
// GAME and REAL are left in English on purpose: that is what a speedrun timer's clocks are called
// in Japanese too, and translating them would read worse against a splits file.
static const char* const kOvlJp[T_COUNT] = {
    /* T_KILL_LINE          */ "(%s) %s: ON",
    /* T_KILL_INSTANT       */ "戦闘開始時に即勝利 + 一撃必殺",
    /* T_KILL_ONEHIT        */ "一撃必殺",
    /* T_KILL_OFF           */ "OFF",
    /* T_KILL_OFF_FLASH     */ "KILLモード: OFF",
    /* T_GAMESPEED          */ "(%s) GAME SPEED %.2gx: ON",
    /* T_GAMESPEED_OFF      */ "GAME SPEED: OFF",
    /* T_SUPPRESS           */ "(%s) セーブデータロード時の敵の出現を抑止: ON",
    /* T_SUPPRESS_OFF       */ "セーブデータロード時の敵の出現を抑止: OFF",
    /* T_SWAP               */ "PREPARING... (できるだけ何も入力しないよう気を付けてください)",
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
    /* T_WARP_ASK_KEYS        */ "  [%s]もう一度でOK   [%s]長押しで現在地を記録   [%s]でキャンセル",
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
    /* T_HELP_BADKEY        */ "  ※iniの値が無効のため既定キー",
    /* T_HELP_BADCFG        */ "※iniに無効な設定があります: %s (既定値で動作中)",
    /* T_SAVE_FIELD_ONLY    */ "SAVE: 戦闘・イベント中は使用不可。移動可能なフィールドで使えます",
    /* T_SAVE_NO_READ       */ "SAVE: 必要な情報を読み取れません。リーダーを少し動かしてください",
    /* T_SS_HEAD            */ "セーブデータの整理",
    /* T_SS_NOTITLE         */ "セーブデータの整理: タイトル画面かセーブ一覧でのみ開けます",
    /* T_SS_CLOUD           */ "セーブデータの整理: この機能は、Steam Cloud同期設定をOFFにすることで使用可能です",
    /* T_SS_GRP_SORT        */ " 並び順",
    /* T_SS_GRP_RENUM       */ " セーブ番号の再採番",
    /* T_SS_ORDER_DATE      */ "セーブ日時順",
    /* T_SS_ORDER_ASC       */ "進行順(序盤が先頭)",
    /* T_SS_ORDER_DESC      */ "進行順(終盤が先頭)",
    /* T_SS_ORDER_NUMBER    */ "セーブ番号順",
    /* T_SS_ORDER_KEYS      */ "  [%s/%s]=並び順を選ぶ(次に一覧を開いたときから反映)",
    /* T_SS_GRP_FILTER      */ " セーブデータ一覧に表示する範囲",
    /* T_SS_FILTER_ALL      */ "すべての章",
    /* T_SS_FILTER_ONE      */ "第%d章のみ(%d件)",
    /* T_SS_FILTER_KEYS     */ "  [%s/%s]=章を選ぶ  %s=すべての章に戻す",
    /* T_LABEL_PASTED       */ "メモを保存しました。一旦タイトル画面に戻るなどでセーブ一覧を再表示すると表示されます。",
    /* T_LABEL_CLEARED      */ "メモを外しました。一旦タイトル画面に戻るなどでセーブ一覧を再表示すると反映されます。",
    /* T_LABEL_NO_ROW       */ "メモ: セーブ一覧を開き、対象にカーソルを合わせてから押してください",
    /* T_LABEL_NO_SAVE      */ "メモ: 新規セーブデータを作成するスロットなので選択できません",
    /* T_LABEL_NO_WINDOW    */ "メモ: ゲームのウィンドウを取得できませんでした",
    /* T_LABEL_EDIT_HEAD    */ "セーブデータ%dのメモ",
    /* T_LABEL_EDIT_HELP    */ "  入力するか、ゲーム外でコピーしたテキストをCtrl+Vで貼り付け。手入力は半角のみ。ペーストであれば全角やマルチバイトも可",
    /* T_LABEL_EDIT_KEYS    */ "  %s=決定   [%s]=中止   (空にすると外す)",
    /* T_LABEL_EDIT_FULL    */ "  今の入力は無効: %d文字まで、「,」は使えません",
    /* T_SS_RENUM           */ "進行順に再採番する",
    /* T_SS_RENUM_ARM       */ "再採番の確認: %d件 — もう一度%sで実行",
    /* T_SS_FOOT            */ "  [%s/%s]=選択  %s=実行  [%s/%s]=閉じる",
    /* T_SS_RENUM_DONE      */ "セーブ番号を進行順に再採番しました(%d件)",
    /* T_SS_RENUM_FAIL      */ "再採番: リネームできないファイルがあったため、何も変更していません",
    /* T_SS_RENUM_LIST      */ "再採番: ロード一覧を閉じてから実行してください(開いている一覧は古い番号のままのため)",
    /* T_SS_RENUM_PART      */ "再採番: 途中で止まりました — 次回起動時に完了します",
    /* T_SS_GRP_CAT         */ " カテゴリー管理",
    /* T_CAT_COUNT          */ " (%d件)",
    /* T_SS_CAT_PICK        */ "%s に切替(使用中: %s)",
    /* T_SS_CAT_PICK_NONE   */ "使用中: %s (他のカテゴリーなし)",
    /* T_SS_CAT_UNPICKED    */ "----",
    /* T_SS_CAT_NEW         */ "新しいカテゴリーを作って切替(最新の1件をコピー)",
    /* T_SS_CAT_NEW_ALL     */ "新しいカテゴリーを作って切替(全件コピー)",
    /* T_SS_CAT_RENAME      */ "使用中のカテゴリーの名前を変更",
    /* T_SS_CAT_DEL         */ "%s を削除",
    /* T_SS_CAT_DEL_IDLE    */ "カテゴリーを削除(未選択)",
    /* T_SS_CAT_DEL_ARM     */ "削除の確認: 「%s」 — もう一度%sで実行",
    /* T_SS_CAT_DEL_KEYS    */ "  [%s/%s]=削除するカテゴリーを選ぶ(使用中は削除不可)  %s=削除",
    /* T_SS_CAT_DEL_DONE    */ "カテゴリー: 「%s」を削除しました(ごみ箱へ)",
    /* T_SS_CAT_DEL_FAIL    */ "カテゴリー削除: 失敗(ログ参照)",
    /* T_SS_CAT_KEYS        */ "  [%s/%s]=切替先を選ぶ  %s・行を離れる・閉じる=切替",
    /* T_SS_CAT_DONE        */ "カテゴリー切替: 「%s」へ(退避%d件 / 復帰%d件)",
    /* T_SS_CAT_NOTITLE     */ "カテゴリー切替: タイトル画面でのみ実行できます(一覧は閉じてから)",
    /* T_SS_CAT_FAIL        */ "カテゴリー切替: 失敗(ログ参照)",
    /* T_SS_CAT_RENAMED     */ "カテゴリー: 「%s」に改名",
    /* T_SS_CAT_EXISTS      */ "  同名のカテゴリーがあります。名前を変えてください",
    /* T_SS_CAT_BADNAME     */ "  その名前は使えません。名前を変えてください",
    /* T_SS_CAT_NAME_HEAD   */ "新しいカテゴリーの名前",
    /* T_SS_CAT_NAME_HEAD_ALL */ "新しいカテゴリーの名前(全件コピー)",
    /* T_SS_CAT_RENAME_HEAD */ "カテゴリーの新しい名前",
    /* T_SS_CAT_EDIT_HELP   */ "  入力するか、ゲーム外でコピーしたテキストをCtrl+Vで貼り付け。使えない文字: \\ / : * ? \" < > | ,",
    /* T_SS_CAT_EDIT_KEYS   */ "  %s=決定   [%s]=中止",
    /* T_SEND_NEWCAT        */ "<新しいカテゴリー>",
    /* T_SEND_COPY          */ "コピー → %s",
    /* T_SEND_MOVE          */ "移動 → %s",
    /* T_SEND_DEST_COPY     */ "コピー先カテゴリー",
    /* T_SEND_DEST_MOVE     */ "移動先カテゴリー",
    /* T_SEND_FULL          */ "送り先「%s」に空き番号がありません",
    /* T_SEND_COPIED        */ "セーブデータ%dを「%s」のセーブデータ%dへコピーしました",
    /* T_SEND_MOVED         */ "セーブデータ%dを「%s」のセーブデータ%dへ移動しました。一覧へは一旦タイトル画面に戻るなどで開き直すと反映されます",
    /* T_SEND_FAIL          */ "送り: 失敗(ログ参照)",
    /* T_LSEL_HEAD          */ "セーブデータ%dの操作%s",
    /* T_LSEL_PICK          */ "   ([%s/%s]=別のセーブデータを選ぶ)",
    /* T_LSEL_NOTE          */ "メモを書く/編集する",
    /* T_LSEL_KEYS          */ "  [%s/%s]=選択  %s=実行  [%s]=閉じる",
    /* T_LSEL_SEND_KEYS     */ "  [%s/%s]=選択  [%s/%s]=%s  %s=実行  [%s]=閉じる",
    /* T_LSEL_MARK_SET      */ "範囲選択(セーブデータ%dを始点にする)",
    /* T_LSEL_MARK_CLR      */ "範囲選択を解除",
    /* T_SEND_COPY_N        */ "コピー(一覧上のセーブデータ%d〜%dの%d件) → %s",
    /* T_SEND_MOVE_N        */ "移動(一覧上のセーブデータ%d〜%dの%d件) → %s",
    /* T_RANGE_MARKED       */ "範囲の始点: セーブデータ%d — もう一方の端にカーソルを移す",
    /* T_RANGE_CLEARED      */ "範囲選択を解除しました",
    /* T_RANGE_GONE         */ "範囲を特定できません — 解除しました",
    /* T_RANGE_ROW_GONE    */ "このセーブデータはもうありません — 実在するセーブデータへカーソルを移してください",
    /* T_SEND_N_FULL        */ "送り先「%s」の空きは%d件 — %d件には足りません",
    /* T_SEND_N_PART        */ "送り: %d/%d件で停止(ログ参照)",
    /* T_SEND_N_COPIED      */ "セーブデータ%d件を「%s」へコピーしました",
    /* T_SEND_N_MOVED       */ "セーブデータ%d件を「%s」へ移動しました(一覧は開き直すと反映)",
    /* T_LSEL_DEL          */ "削除(一覧上のセーブデータ%d〜%dの%d件)",
    /* T_LSEL_DEL_IDLE     */ "範囲を削除(先に範囲選択が必要)",
    /* T_LSEL_DEL_ARM      */ "削除の確認: %d件 — もう一度%sで実行",
    /* T_DEL_NEED_RANGE    */ "削除: 先に上の「範囲選択」の行で始点を決めてください",
    /* T_DEL_N_DONE        */ "セーブデータ%d件をごみ箱へ削除しました。一覧へは一旦タイトル画面に戻るなどで開き直すと反映されます",
    /* T_DEL_FAIL          */ "削除: 失敗(ログ参照)",
    /* T_OK_KEY            */ "[%s]",
    /* T_SET_TITLE          */ "ホットキーと設定  ([%s]=閉じる  [%s/%s]=選ぶ  [%s/%s]=変える  %s=キーを割り当てる)",
    /* T_SET_HEAD_KEYS      */ "ホットキー",
    /* T_SET_HEAD_SET       */ "設定",
    /* T_SET_L_SPEED        */ "GAME SPEED",
    /* T_SET_L_KILL         */ "KILLモード",
    /* T_SET_L_WARP         */ "ワープ",
    /* T_SET_L_SUPPRESS     */ "セーブデータロード時の敵の出現を抑止",
    /* T_SET_L_SAVE         */ "どこでもセーブ",
    /* T_SET_L_CAMERA       */ "カメラ固定解除",
    /* T_SET_L_TIDY         */ "セーブデータの整理",
    /* T_SET_L_BATTLE       */ "戦闘オーバーレイ",
    /* T_SET_L_PICKER       */ "任意戦闘呼び出し",
    /* T_SET_L_HELP         */ "ホットキーと設定(この画面)",
    /* T_SET_KEYS_LABEL     */ "ピッカー/メニューの操作:",
    /* T_SET_SUB_UP         */ "上",
    /* T_SET_SUB_DOWN       */ "下",
    /* T_SET_SUB_LEFT       */ "左",
    /* T_SET_SUB_RIGHT      */ "右",
    /* T_SET_SUB_CLOSE      */ "閉じる",
    /* T_SET_SUB_OK         */ "決定",
    /* T_SET_SUB_LIST       */ "一覧",
    /* T_SET_FN_UP          */ "上",
    /* T_SET_FN_DOWN        */ "下",
    /* T_SET_FN_LEFT        */ "左",
    /* T_SET_FN_RIGHT       */ "右",
    /* T_SET_FN_CLOSE       */ "閉じる",
    /* T_SET_FN_OK          */ "決定",
    /* T_SET_FN_LPREV       */ "一覧の前",
    /* T_SET_FN_LNEXT       */ "一覧の次",
    /* T_SET_L_FOCUS        */ "他のウィンドウ操作中もホットキー有効",
    /* T_SET_L_MULT         */ "「GAME SPEED」の倍率設定",
    /* T_SET_L_BGM          */ "任意戦闘呼び出しのBGM開始タイミング",
    /* T_SET_L_MARK         */ "どこでもセーブを使用したセーブデータに付けるテキスト",
    /* T_SET_L_KEEPNOTE     */ "上書きセーブ時にメモを残す",
    /* T_SET_L_SAVEDIR      */ "セーブフォルダ",
    /* T_SET_L_TOP          */ "画面上端からテキスト表示開始する位置",
    /* T_SET_L_LOG          */ "ログレベル",
    /* T_SET_V_ON           */ "ON",
    /* T_SET_V_OFF          */ "OFF",
    /* T_SET_V_NONE         */ "なし",
    /* T_SET_V_AUTODIR      */ "自動検出",
    /* T_SET_V_MS           */ "%dms",
    /* T_SET_V_PX           */ "%dpx",
    /* T_SET_V_MULT_IN      */ "●%s",
    /* T_SET_V_MULT_OUT     */ "○%s",
    /* T_SET_F_SPEED        */ "キー入力ごとに%sの順で切替。下の \"「GAME SPEED」の倍率設定\" の行で変更可能",
    /* T_SET_F_KILL         */ "キー入力ごとに 即勝利+一撃必殺、一撃必殺、OFF の順で切替",
    /* T_SET_F_WARP         */ "長押し=現在地を記録 / 短押し2回=ワープ",
    /* T_SET_F_SUPPRESS     */ "キー入力ごとにON/OFF切替。ロード後にONにした場合は、次のロードから有効",
    /* T_SET_F_SAVE         */ "Steam Cloud同期をOFFにして使う",
    /* T_SET_F_CAMERA       */ "どこでもセーブのデータロード後にカメラが固まった時に",
    /* T_SET_F_TIDY         */ "タイトル画面ではセーブデータの整理用メニューを表示。セーブ一覧では各セーブデータ操作用のメニューを表示。キー入力ごとに表示ON/OFF切替",
    /* T_SET_F_BATTLE       */ "戦闘中にタイマーや敵のHP・ATB・チェーンを表示。キー入力ごとに表示ON/OFF切替",
    /* T_SET_F_PICKER       */ "[%s/%s]=移動 [%s/%s]=ページ送り 長押しで決定(未表示なら前回分を即開始)。決定後は入力しないこと",
    /* T_SET_F_HELP         */ "この画面の表示/非表示。iniのhelpでも変えられる。解除はできない。キー入力ごとに表示ON/OFF切替",
    /* T_SET_F_KEYS         */ "[%s/%s]=項目を選ぶ %s=キーを登録する",
    /* T_SET_F_FOCUS        */ "ONにすると他のウィンドウを操作中でもホットキーが有効",
    /* T_SET_F_MULT         */ "[%s/%s]=候補を選ぶ %s=入れる/外す。最大8個まで、押す順は昇順: %s",
    /* T_SET_F_BGM          */ "0〜8000ms、100ms刻み。0で即座に曲が入る",
    /* T_SET_F_MARK         */ "ゲームのセーブ一覧で、どこでもセーブで作ったデータの番号の右に付く目印。次に保存したデータから有効",
    /* T_SET_F_KEEPNOTE     */ "ONにすると、メモの付いたセーブデータに上書きしてもメモが残る。OFFなら上書きで消える",
    /* T_SET_F_SAVEDIR      */ "ff13-NN.datのあるフォルダ。iniのsave_dirで変更(空=自動検出)",
    /* T_SET_F_TOP          */ "0〜400px、8px刻み。FPS表示などが上の行に重なるときに増やす",
    /* T_SET_F_LOG          */ "quiet=エラーと起動時のみ / normal=+操作の結果 / verbose=+ゲーム内部の動作",
    /* T_SET_F_CAPTURE      */ "新しいキーを押してください(今のキーをもう一度押すと解除、[%s]か[%s]で中止)",
    /* T_SET_F_DUP          */ "! 他の機能と同じキー — ",
    /* T_SET_CANT_UNBIND_HELP */ "この画面を開くキーは解除できません",
    /* T_SET_CANT_UNBIND_OK */ "決定のキーは解除できません(他に決定する方法が無いため)",
    /* T_SET_USED           */ "%sは「%s」に使用中",
    /* T_SET_OK_IS_TIDY     */ "整理用のキーと同じキーは決定に使えません(押すとメニューが閉じるため)",
    /* T_SET_TIDY_IS_OK     */ "決定と同じキーは整理キーに使えません(押すとメニューが閉じるため)",
    /* T_SET_MULT_MIN       */ "この倍率設定は外せません(1つ以上必要です)",
    /* T_SET_MULT_MAX       */ "この倍率設定は入れられません(最大8個まで)",
    /* T_SET_INI_FAIL       */ "iniファイル保存エラーが発生しました。ゲームを再起動すると変更前に戻ります。",
    /* T_SET_HINT_CHANGE    */ "  (%sで変更)",
};

#endif // FF13_OVL_TEXT_H
