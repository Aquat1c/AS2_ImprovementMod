#pragma once

/**
 * Alice Senki 2 - UI string table.
 *
 * Every label the mod's own menus draw comes through here, so the whole UI can
 * switch language at once. One X-macro row per string keeps English and
 * Japanese in step: adding a row without both columns does not compile, and
 * the enum, both tables and the glyph-collection text are all generated from
 * the same list.
 *
 * Strings are UTF-8. Format strings keep their arguments in the same order in
 * both languages, so a call site never has to know which one is live.
 */

#include <stddef.h>
#include <stdint.h>

namespace Ui {

enum class Lang : uint8_t {
    English = 0,
    Japanese,
    Count,
};

// X(id, english, japanese)
#define AS2_UI_STRINGS(X)                                                                              \
    /* --- shared ------------------------------------------------------------------ */                \
    X(Common_On,               "On",                       "オン")                                      \
    X(Common_Off,              "Off",                      "オフ")                                      \
    X(Common_Back,             "Back",                     "戻る")                                      \
    X(Common_Settings,         "Settings",                 "設定")                                       \
    X(Common_Unknown,          "Unknown",                  "不明")                                       \
    X(Common_Status,           "Status",                   "状態")                                       \
    X(Common_Address,          "Address",                  "アドレス")                                   \
    X(Common_Host,             "Host",                     "ホスト")                                    \
    X(Common_Players,          "Players",                  "プレイヤー")                                \
    X(Common_Player,           "Player",                   "プレイヤー")                                \
    X(Common_P1,               "P1",                       "1P")                                         \
    X(Common_P2,               "P2",                       "2P")                                         \
    X(Common_Both,             "Both",                     "両方")                                       \
    X(Common_None,             "None",                     "なし")                                       \
    X(Common_Local,            "Local",                    "ローカル")                                  \
    X(Common_Remote,           "Remote",                   "リモート")                                  \
    X(Common_Keyboard,         "Keyboard",                 "キーボード")                                \
    X(Common_Yes,              "Yes",                      "はい")                                       \
    X(Common_No,               "No",                       "いいえ")                                     \
    X(Common_Language,         "Language",                 "言語")                                       \
    X(Common_LanguageEnglish,  "English",                  "English")                                    \
    X(Common_LanguageJapanese, "日本語",                   "日本語")                                     \
    X(Common_LanguageHint,     "menu language",            "メニューの言語")                            \
    /* --- pause menu tabs -------------------------------------------------------- */                \
    X(Pm_TabDummy,             "Dummy",                    "ダミー")                                    \
    X(Pm_TabRecovery,          "Recovery",                 "回復")                                       \
    X(Pm_TabTriggers,          "Triggers",                 "トリガー")                                  \
    X(Pm_TabDisplay,           "Display",                  "表示")                                       \
    X(Pm_TabMatch,             "Match",                    "試合")                                       \
    X(Pm_TabState,             "State",                    "状態")                                       \
    X(Pm_TabExit,              "Exit",                     "終了")                                       \
    X(Pm_TabAutoRecovery,      "Auto-Recovery",            "自動回復")                                  \
    X(Pm_TabHud,               "HUD",                      "HUD")                                        \
    /* --- pause menu rows -------------------------------------------------------- */                \
    X(Pm_Advanced,             "Advanced...",              "詳細設定...")                               \
    X(Pm_HudPage,              "HUD...",                   "HUD...")                                     \
    X(Pm_MidScreen,            "Mid Screen",               "画面中央")                                   \
    X(Pm_Corner,               "Corner",                   "画面端")                                     \
    X(Pm_RoundStart,           "Round Start",              "ラウンド開始位置")                          \
    X(Pm_SwapSides,            "Swap Sides",               "左右入れ替え")                              \
    X(Pm_SavePosition,         "Save Position",            "位置を保存")                                 \
    X(Pm_LoadPosition,         "Load Position",            "位置を読込")                                 \
    X(Pm_RoundReset,           "Round Reset",              "ラウンドリセット")                          \
    X(Pm_FrameStep,            "Frame Step",               "コマ送り")                                   \
    X(Pm_RecordMacro,          "Record Macro",             "マクロ記録")                                 \
    X(Pm_PlayMacro,            "Play Macro",               "マクロ再生")                                 \
    X(Pm_SaveState,            "Save State",               "状態を保存")                                 \
    X(Pm_LoadState,            "Load State",               "状態を読込")                                 \
    X(Pm_Resume,               "Resume",                   "再開")                                       \
    X(Pm_ReplayList,           "Replay List",              "リプレイ一覧")                              \
    X(Pm_CharacterSelect,      "Character Select",         "キャラクター選択")                          \
    X(Pm_ExitMatch,            "Exit Match",               "試合を終了")                                 \
    X(Pm_HintDefault,          "Up/Down move    Left/Right change tab or value    A select    START resume",   \
                               "上下:移動    左右:タブ/値の変更    A:決定    START:再開")               \
    X(Pm_HintNumeric,          "Type a value    Backspace deletes    Enter confirms    Esc cancels",           \
                               "数値を入力    Backspace:削除    Enter:確定    Esc:取消")                \
    X(Pm_HintPopup,            "Move to pick the motion    Left/Right on a field changes it    B closes",     \
                               "移動でコマンドを選択    項目上で左右:変更    B:閉じる")                  \
    X(Pm_Shown,                "Shown",                    "表示")                                       \
    X(Pm_Hidden,               "Hidden",                   "非表示")                                     \
    X(Pm_AllDefense,           "All (defense)",            "全て(防御)")                                 \
    /* --- practice setting labels ------------------------------------------------ */                \
    X(Pr_Type,                 "Type",                     "種類")                                       \
    X(Pr_AutoBlock,            "Auto-Block",               "オートガード")                              \
    X(Pr_Stance,               "Stance",                   "構え")                                       \
    X(Pr_AutoJump,             "Auto-Jump",                "オートジャンプ")                            \
    X(Pr_JumpRate,             "Jump Rate",                "ジャンプ間隔")                              \
    X(Pr_Defense,              "Defense",                  "防御")                                       \
    X(Pr_Cpu,                  "CPU",                      "CPU")                                        \
    X(Pr_AirTech,              "Air Tech",                 "空中受け身")                                 \
    X(Pr_GroundTech,           "Ground Tech",              "地上受け身")                                 \
    X(Pr_Guard,                "Guard",                    "ガード")                                    \
    X(Pr_State,                "State",                    "状態")                                       \
    X(Pr_Health,               "Health",                   "体力")                                       \
    X(Pr_Meter,                "Meter",                    "気力")                                       \
    X(Pr_BothNeutral,          "Both Neutral",             "両者ニュートラル時")                        \
    X(Pr_Delay,                "Delay",                    "遅延")                                       \
    X(Pr_Triggers,             "Triggers",                 "トリガー")                                  \
    X(Pr_Target,               "Target",                   "対象")                                       \
    X(Pr_RandomSkip,           "Random Skip",              "ランダム省略")                              \
    X(Pr_WakeBuffer,           "Wake Buffer",              "起き上がり先行入力")                        \
    X(Pr_Hitboxes,             "Hitboxes",                 "判定表示")                                   \
    X(Pr_Combo,                "Combo",                    "コンボ")                                     \
    X(Pr_Inputs,               "Inputs",                   "入力表示")                                   \
    X(Pr_Damage,               "Damage",                   "ダメージ")                                   \
    X(Pr_FrameAdv,             "Frame Adv",                "フレーム有利")                              \
    X(Pr_Regen,                "Regen",                    "体力回復")                                   \
    X(Pr_MeterLevel,           "Meter Level",              "気力レベル")                                 \
    X(Pr_Button,               "Button",                   "ボタン")                                     \
    X(Pr_ControlP2,            "Control P2",               "2Pを操作")                                   \
    X(Pr_MacroSlot,            "Macro Slot",               "マクロスロット")                            \
    X(Pr_Freeze,               "Freeze",                   "停止")                                       \
    X(Pr_HpValue,              "HP Value",                 "体力の値")                                   \
    X(Pr_MeterValue,           "Meter Value",              "気力の値")                                   \
    X(Pr_GuardValue,           "Guard Value",              "ガードの値")                                \
    /* --- practice value labels -------------------------------------------------- */                \
    X(Pr_TypeMod,              "Mod",                      "MOD")                                        \
    X(Pr_TypeVanilla,          "Vanilla",                  "標準")                                       \
    X(Pr_Full,                 "Full",                     "全回復")                                     \
    X(Pr_Custom,               "Custom",                   "カスタム")                                   \
    X(Pr_Up,                   "Up",                       "上")                                         \
    X(Pr_Forward,              "Forward",                  "前")                                         \
    X(Pr_Neutral,              "Neutral",                  "ニュートラル")                              \
    X(Pr_Back,                 "Back",                     "後ろ")                                       \
    X(Pr_Normal,               "Normal",                   "通常")                                       \
    X(Pr_OneHit,               "1 Hit",                    "1ヒット")                                    \
    X(Pr_Stand,                "Stand",                    "立ち")                                       \
    X(Pr_Crouch,               "Crouch",                   "しゃがみ")                                   \
    X(Pr_Jump,                 "Jump",                     "ジャンプ")                                   \
    X(Pr_Auto,                 "Auto",                     "自動")                                       \
    X(Pr_BlockAll,             "All",                      "全て")                                       \
    X(Pr_BlockFirstHit,        "First Hit",                "初段のみ")                                   \
    X(Pr_BlockAfterFirstHit,   "After First Hit",          "初段以降")                                   \
    X(Pr_BlockRandom,          "Random",                   "ランダム")                                   \
    X(Pr_BlockStanceOnly,      "Stance Only",              "構えのみ")                                   \
    X(Pr_JumpDisabled,         "Disabled",                 "なし")                                       \
    X(Pr_JumpBackward,         "Backward",                 "後ろ")                                       \
    X(Pr_JumpHold,             "Hold",                     "常時")                                       \
    X(Pr_DefGuardOnly,         "Guard Only",               "ガードのみ")                                \
    X(Pr_DefNative,            "Native",                   "キャラ固有")                                 \
    X(Pr_DefJustParry,         "Just Parry",               "ジャストディフェンス")                      \
    X(Pr_DefDodgeBack,         "Dodge Back",               "後ろ回避")                                   \
    X(Pr_DefRepel,             "Repel",                    "弾き")                                       \
    X(Pr_DefCounter6D,         "Counter 6D",               "カウンター 6D")                             \
    X(Pr_DefPerfectParry,      "Perfect Parry",            "完全押し返し")                              \
    X(Pr_DefAbsBlock,          "Abs Def (Block)",          "絶対防御(ガード中)")                        \
    X(Pr_DefParry,             "Parry",                    "押し返し")                                   \
    X(Pr_DefDodgeFwd,          "Dodge Fwd",                "前回避")                                     \
    X(Pr_DefCounter4D,         "Counter 4D",               "カウンター 4D")                             \
    X(Pr_DefAbsFirst,          "Abs Def (First)",          "絶対防御(先出し)")                          \
    X(Pr_TrigAfterBlock,       "After Block",              "ガード後")                                  \
    X(Pr_TrigOnWakeup,         "On Wakeup",                "起き上がり")                                 \
    X(Pr_TrigAfterHitstun,     "After Hitstun",            "のけぞり後")                                 \
    X(Pr_TrigAfterAirtech,     "After Airtech",            "空中受け身後")                              \
    X(Pr_TrigAfterGroundTech,  "After Ground Tech",        "地上受け身後")                              \
    X(Pr_TrigAfterDefence,     "After Defence",            "防御成功後")                                 \
    X(Pr_TrigShortBlock,       "Block",                    "ガード")                                    \
    X(Pr_TrigShortWakeup,      "Wakeup",                   "起き上がり")                                 \
    X(Pr_TrigShortHitstun,     "Hitstun",                  "のけぞり")                                   \
    X(Pr_TrigShortAirTech,     "Air Tech",                 "空中受け身")                                 \
    X(Pr_TrigShortGroundTech,  "Ground Tech",              "地上受け身")                                 \
    X(Pr_TrigShortDefence,     "Defence",                  "防御")                                       \
    X(Pr_ActNone,              "None",                     "なし")                                       \
    X(Pr_ActJump,              "Jump",                     "ジャンプ")                                   \
    X(Pr_ActDashForward,       "Dash Forward",             "前ダッシュ")                                 \
    X(Pr_ActDashBack,          "Dash Back",                "後ろダッシュ")                              \
    X(Pr_MeterZero,            "0",                        "0")                                          \
    X(Pr_Meter3000,            "3000",                     "3000")                                       \
    X(Pr_Meter6000,            "6000",                     "6000")                                       \
    X(Pr_Meter9000,            "9000",                     "9000")                                       \
    /* --- practice toasts -------------------------------------------------------- */                \
    X(Pr_ToastRoundStartRestored, "Round start restored",  "ラウンド開始位置に戻しました")              \
    X(Pr_ToastPositionSaved,   "Position saved",           "位置を保存しました")                        \
    X(Pr_ToastPositionSaveUnavailable, "Position save unavailable", "位置を保存できません")             \
    X(Pr_ToastSavedPositionRestored, "Saved position restored", "保存した位置に戻しました")              \
    X(Pr_ToastRoundStart,      "Round Start",              "ラウンド開始位置")                          \
    X(Pr_ToastMidScreen,       "Mid Screen",               "画面中央")                                   \
    X(Pr_ToastPositionsSwapped, "Positions swapped",       "位置を入れ替えました")                      \
    X(Pr_ToastWaitForRoundStart, "Wait for round start",   "ラウンド開始までお待ちください")            \
    X(Pr_ToastNoSavedPosition, "No saved position",        "保存した位置がありません")                  \
    X(Pr_ToastControlsSwappedP2, "Controls Swapped (P2)",  "操作を入れ替え (2P)")                       \
    X(Pr_ToastControlsNormalP1, "Controls Normal (P1)",    "操作を通常に (1P)")                         \
    /* --- character names -------------------------------------------------------- */                \
    X(Chr_Rance,               "Rance",                    "ランス")                                    \
    X(Chr_Hatsune,             "Hatsune",                  "初音")                                       \
    X(Chr_Patton,              "Patton",                   "パットン")                                  \
    X(Chr_Seed,                "Seed",                     "シード")                                     \
    X(Chr_Raysen,              "Raysen",                   "レイセン")                                  \
    X(Chr_Aria,                "Aria",                     "アリア")                                     \
    X(Chr_Maria,               "Maria",                    "マリア")                                     \
    X(Chr_Shizuka,             "Shizuka",                  "志津香")                                     \
    X(Chr_Fanel,               "Fanel",                    "フェーネル")                                \
    X(Chr_Miki,                "Miki",                     "美樹")                                       \
    X(Chr_Menad,               "Menad",                    "メナド")                                     \
    X(Chr_HannyKing,           "Hanny King",               "ハニーキング")                              \
    X(Chr_Satsu,               "Satsu",                    "サツ")                                       \
    X(Chr_TigerJoe,            "Tiger Joe",                "タイガー・ジョー")                          \
    X(Chr_Escalayer,           "Escalayer",                "エスカレイヤー")                            \
    X(Chr_Makutsudo,           "Makutsudo",                "マクツド")                                  \
    X(Chr_Alietta,             "Alietta",                  "アリエッタ")                                \
    X(Chr_Nalzgis,             "Nalzgis",                  "ナルジス")                                  \
    X(Chr_DemonRance,          "Demon Rance",              "魔人ランス")                                 \
    X(Chr_LittlePrincess,      "Little Princess",          "リトルプリンセス")                          \
    X(Chr_Tada,                "TADA",                     "TADA")                                       \
    X(Chr_NalzgisBoss,         "Nalzgis (Boss)",           "ナルジス(ボス)")                            \
    /* --- game settings ---------------------------------------------------------- */                \
    X(Gs_Title,                "SETTINGS",                 "設定")                                       \
    X(Gs_System,               "SYSTEM",                   "システム")                                   \
    X(Gs_KeySettings,          "KEY SETTINGS",             "キー設定")                                   \
    X(Gs_PracticeHotkeys,      "PRACTICE HOTKEYS",         "トレーニング用ホットキー")                  \
    X(Gs_VoiceVolume,          "VOICE VOLUME",             "ボイス音量")                                 \
    X(Gs_Difficulty,           "Difficulty",               "難易度")                                     \
    X(Gs_Rounds,               "Rounds",                   "ラウンド数")                                 \
    X(Gs_BattleRecording,      "Battle Recording",         "対戦記録")                                   \
    X(Gs_VoiceVolumeRow,       "Voice Volume",             "ボイス音量")                                 \
    X(Gs_SeVolume,             "SE Volume",                "SE音量")                                     \
    X(Gs_BgmVolume,            "BGM Volume",               "BGM音量")                                    \
    X(Gs_SystemVoice,          "System Voice",             "システムボイス")                            \
    X(Gs_AiLearning,           "AI Learning",              "AI学習")                                     \
    X(Gs_Easy,                 "Easy",                     "やさしい")                                   \
    X(Gs_NormalDifficulty,     "Normal",                   "ふつう")                                     \
    X(Gs_Hard,                 "Hard",                     "むずかしい")                                 \
    X(Gs_LockedOnline,         "Locked while an online session is active.", "オンライン対戦中は変更できません。") \
    X(Gs_OfflineOnlyAi,        "Offline only. Netplay disables CPU learning.", "オフライン専用。ネット対戦中はCPU学習を無効化します。") \
    X(Gs_KeyUp,                "Up",                       "上")                                         \
    X(Gs_KeyDown,              "Down",                     "下")                                         \
    X(Gs_KeyLeft,              "Left",                     "左")                                         \
    X(Gs_KeyRight,             "Right",                    "右")                                         \
    X(Gs_KeyA,                 "A (Light)",                "A (弱)")                                     \
    X(Gs_KeyB,                 "B (Medium)",               "B (中)")                                     \
    X(Gs_KeyC,                 "C (Heavy)",                "C (強)")                                     \
    X(Gs_KeyD,                 "D (Special)",              "D (特殊)")                                   \
    X(Gs_KeyStart,             "Start",                    "スタート")                                  \
    X(Gs_KeySelect,            "Select",                   "セレクト")                                  \
    X(Gs_PressAny,             "press...",                 "入力待ち...")                                \
    X(Gs_DPad,                 "D-Pad",                    "十字キー")                                   \
    X(Gs_AnyMode,              "any mode",                 "全モード")                                   \
    X(Gs_ResetP1,              "Reset Player 1 to defaults", "1Pを初期設定に戻す")                       \
    X(Gs_ResetP2,              "Reset Player 2 to defaults", "2Pを初期設定に戻す")                       \
    X(Gs_ResetDefaults,        "Reset to defaults",        "初期設定に戻す")                            \
    X(Gs_HintPressKey,         "Press any key or pad button. ESC cancels.", "キーまたはボタンを押してください。ESCで取消。") \
    X(Gs_HintKeyColumns,       "Left/Right picks the player, A rebinds.", "左右:プレイヤー選択  A:割り当て") \
    X(Gs_HintHotkeys,          "A rebinds, Left/Right unbinds.", "A:割り当て  左右:解除")                \
    X(Gs_PressKeyFor,          "Press a key for P%d %s",   "P%d %s のキーを押してください")             \
    X(Gs_PlayerReset,          "Player %d reset to defaults", "プレイヤー%dを初期設定に戻しました")     \
    X(Gs_HotkeysReset,         "Hotkeys reset to defaults", "ホットキーを初期設定に戻しました")        \
    X(Gs_RebindCanceled,       "Rebind canceled.",         "割り当てを取り消しました。")                \
    X(Gs_PlayerN,              "PLAYER %d",                "プレイヤー%d")                              \
    X(Gs_LanguageStatus,       "Language: %s",             "言語: %s")                                   \
    X(Gs_SysFullscreen,        "Fullscreen",               "フルスクリーン")                            \
    X(Gs_SysFullscreenHint,    "borderless window",        "ボーダーレスウィンドウ")                    \
    X(Gs_SysKeepAspect,        "Keep Aspect",              "アスペクト比維持")                          \
    X(Gs_SysKeepAspectHint,    "4:3 letterbox",            "4:3 レターボックス")                        \
    X(Gs_SysWindowSize,        "Window Size",              "ウィンドウサイズ")                          \
    X(Gs_SysWindowSizeHint,    "windowed only",            "ウィンドウ時のみ")                          \
    X(Gs_SysBackgroundInput,   "Background Input",         "非アクティブ時入力")                        \
    X(Gs_SysBackgroundInputHint, "keep playing unfocused", "フォーカス外でも操作可")                    \
    X(Gs_SysSwap,              "Swap P1/P2",               "1P/2P入れ替え")                             \
    X(Gs_SysSwapHint,          "trade control sides",      "操作側を交換")                              \
    X(Gs_SysDebugCapture,      "Debug Capture",            "デバッグ出力記録")                          \
    X(Gs_SysDebugCaptureHint,  "log the game's output",    "ゲームの出力をログに記録")                  \
    X(Gs_SysLocalRematch,      "Local Rematch",            "ローカル再戦")                              \
    X(Gs_SysLocalRematchHint,  "VS continue prompt",       "対戦後のコンティニュー確認")                \
    X(Gs_SysPracticeKeys,      "Practice Hotkeys",         "トレーニング用ホットキー")                  \
    X(Gs_SysPracticeKeysHint,  "rebind mod keys",          "MODのキーを割り当て")                        \
    X(Gs_SysBackHint,          "settings",                 "設定へ")                                     \
    X(Gs_SysNeedsProxy,        "Display settings need the mod's d3d9 proxy.", "表示設定にはMODのd3d9プロキシが必要です。") \
    X(Gs_SysWindowSizeStatus,  "Window size: %dx (%dx%d)", "ウィンドウサイズ: %d倍 (%dx%d)")            \
    X(Gs_RootGeneral,          "General Settings",         "一般設定")                                   \
    X(Gs_RootSystem,           "System",                   "システム")                                   \
    X(Gs_RootKeys,             "Key Settings",             "キー設定")                                   \
    X(Gs_RootBattleHistory,    "Battle History",           "対戦履歴")                                   \
    X(Gs_RootTitles,           "Titles",                   "称号")                                       \
    X(Gs_RootExit,             "Exit Settings",            "設定を終了")                                 \
    X(Gs_RootGeneralHint,      "rules and audio",          "ルールと音声")                              \
    X(Gs_RootSystemHint,       "mod options",              "MODの設定")                                  \
    X(Gs_RootKeysHint,         "controls",                 "操作設定")                                   \
    X(Gs_RootBattleHistoryHint, "past results",            "過去の戦績")                                 \
    X(Gs_RootTitlesHint,       "earned titles",            "獲得した称号")                              \
    X(Gs_RootExitHint,         "back to title",            "タイトルへ戻る")                            \
    /* --- hotkey action names (HotkeyAction order) ------------------------------- */                \
    X(Hk_HitboxToggle,         "Hitbox Toggle",            "判定表示切替")                              \
    X(Hk_PauseToggle,          "Pause Toggle",             "停止切替")                                   \
    X(Hk_FrameStep,            "Frame Step",               "コマ送り")                                   \
    X(Hk_ControlSwap,          "Control Swap",             "操作入れ替え")                              \
    X(Hk_SaveState,            "Save State",               "状態を保存")                                 \
    X(Hk_LoadState,            "Load State",               "状態を読込")                                 \
    X(Hk_PositionLoad,         "Position Load",            "位置を読込")                                 \
    X(Hk_PositionSave,         "Position Save",            "位置を保存")                                 \
    X(Hk_MacroRecord,          "Macro Record",             "マクロ記録")                                 \
    X(Hk_MacroPlayStop,        "Macro Play/Stop",          "マクロ再生/停止")                            \
    X(Hk_MacroSlotNext,        "Macro Slot Next",          "マクロ次スロット")                          \
    X(Hk_OverlayToggle,        "Hide Netplay/Replay Overlay", "ネット対戦/リプレイ表示を隠す")           \
    /* --- practice value formats ------------------------------------------------- */                \
    X(Pr_MeterOneBar,          "1 Bar",                    "1本")                                        \
    X(Pr_MeterBarsFmt,         "%d Bars",                  "%d本")                                       \
    /* --- netplay menu ----------------------------------------------------------- */                \
    X(Np_TagOnline,            "ONLINE",                   "オンライン")                                \
    X(Np_TagWatch,             "WATCH",                    "観戦")                                       \
    X(Np_TagSetup,             "SETUP",                    "設定")                                       \
    X(Np_TagNotice,            "NOTICE",                   "お知らせ")                                  \
    X(Np_TagPlay,              "PLAY",                     "対戦")                                       \
    X(Np_TitleOnlineMenu,      "Online Menu",              "オンラインメニュー")                        \
    X(Np_TitleWatch,           "Watch a Match",            "試合を観戦")                                 \
    X(Np_TitleConnSettings,    "Connection Settings",      "接続設定")                                   \
    X(Np_TitleConnNotice,      "Connection Notice",        "接続のお知らせ")                            \
    X(Np_TitlePlayOnline,      "Play Online",              "オンライン対戦")                            \
    X(Np_TitlePlayer,          "Player",                   "プレイヤー")                                \
    X(Np_TitleAppearance,      "Appearance",               "外観")                                       \
    X(Np_TitleNetwork,         "Network",                  "ネットワーク")                              \
    X(Np_TitleWatchSettings,   "Watch",                    "観戦")                                       \
    X(Np_TitleDiagnostics,     "Diagnostics",              "診断")                                       \
    X(Np_TitleGame,            "Game",                     "ゲーム")                                     \
    X(Np_StatusProbingHost,    "Checking whether the host is already in a match...", "ホストが対戦中か確認しています...") \
    X(Np_StatusReachingWatch,  "Reaching the watch server...", "観戦サーバーに接続しています...")         \
    X(Np_StatusWatchingLive,   "Watching a live match.",   "試合を観戦中です。")                        \
    X(Np_StatusWaitingMatch,   "Waiting for a match to start.", "試合開始を待っています。")             \
    X(Np_StatusOpeningRoom,    "Opening your room...",     "ルームを開いています...")                   \
    X(Np_StatusConnectingHost, "Connecting to the host...", "ホストに接続しています...")                \
    X(Np_StatusReadyUp,        "Ready up to start the match.", "準備完了で試合を開始します。")           \
    X(Np_StatusBothConnected,  "Both players connected.",  "両プレイヤーが接続しました。")              \
    X(Np_StatusMatchComplete,  "Match complete.",          "試合終了。")                                 \
    X(Np_StatusInterrupted,    "Connection interrupted.",  "接続が中断されました。")                    \
    X(Np_Confirm,              "Confirm",                  "決定")                                       \
    X(Np_Option,               "Option",                   "オプション")                                \
    X(Np_RouteAutomatic,       "Automatic",                "自動")                                       \
    X(Np_RouteDirectOnly,      "Direct Only",              "直接のみ")                                   \
    X(Np_RouteRelayNA,         "Relay N/A",                "中継不可")                                   \
    X(Np_WatchInstead,         "watch instead?",           "観戦しますか?")                             \
    X(Np_Join,                 "Join",                     "参加")                                       \
    X(Np_Close,                "Close",                    "閉じる")                                     \
    X(Np_HintOpenRoom,         "Open a room",              "ルームを開く")                              \
    X(Np_HintConnectOrWatch,   "Connect or watch",         "接続または観戦")                            \
    X(Np_HintNameRouting,      "Name and routing",         "名前と経路")                                 \
    X(Np_HintReturnGame,       "Return to game",           "ゲームに戻る")                              \
    X(Np_HintConnectHost,      "Connect to a host",        "ホストに接続")                              \
    X(Np_HintOnlineMenu,       "Online menu",              "オンラインメニュー")                        \
    X(Np_StatusExtIp,          "Status [ext-IP:port]",     "状態 [外部IP:ポート]")                       \
    X(Np_StartHosting,         "Start Hosting",            "ホスト開始")                                 \
    X(Np_HintOpenTheRoom,      "Open the room",            "ルームを開く")                              \
    X(Np_RoomPort,             "Room Port",                "ルームポート")                              \
    X(Np_ShareThisRoom,        "Share This Room",          "このルームを共有")                          \
    X(Np_HintStartConnection,  "Start the connection",     "接続を開始")                                 \
    X(Np_HostAddress,          "Host Address",             "ホストアドレス")                            \
    X(Np_Connection,           "Connection",               "接続")                                       \
    X(Np_Connect,              "Connect",                  "接続")                                       \
    X(Np_ScanningLan,          "Scanning nearby rooms...", "近くのルームを検索中...")                   \
    X(Np_FoundRooms,           "Found %u rooms",           "%u件のルームが見つかりました")              \
    X(Np_FoundOneRoom,         "Found 1 room",             "1件のルームが見つかりました")               \
    X(Np_HintSearchNearby,     "Search nearby rooms",      "近くのルームを検索")                        \
    X(Np_ScanLocalLan,         "Scan Local LAN",           "LAN内を検索")                               \
    X(Np_WatchAddress,         "Watch Address",            "観戦アドレス")                              \
    X(Np_Details,              "Details",                  "詳細")                                       \
    X(Np_LanSearch,            "LAN Search",               "LAN検索")                                    \
    X(Np_Server,               "Server",                   "サーバー")                                  \
    X(Np_Punch,                "Punch",                    "パンチ")                                     \
    X(Np_Palettes,             "Palettes",                 "パレット")                                   \
    X(Np_HintNameGameplay,     "Name and gameplay",        "名前とゲーム設定")                          \
    X(Np_HintHudColors,        "HUD colors and trails",    "HUDの色と残像")                             \
    X(Np_HintRoutingServers,   "Routing and servers",      "経路とサーバー")                            \
    X(Np_HintSpectatorPalettes, "Spectator and palettes",  "観戦とパレット")                            \
    X(Np_HintDebugLogs,        "Debug logs",               "デバッグログ")                              \
    X(Np_DisplayName,          "Display Name",             "表示名")                                     \
    X(Np_InputDelay,           "Input delay",              "入力遅延")                                   \
    X(Np_MaxRollback,          "Max rollback",             "最大ロールバック")                          \
    X(Np_ColorBlue,            "Blue",                     "青")                                         \
    X(Np_HintNameBar,          "name bar",                 "名前バー")                                   \
    X(Np_ColorWhite,           "White",                    "白")                                         \
    X(Np_HintNicknameText,     "nickname text",            "ニックネーム文字")                          \
    X(Np_ColorGold,            "Gold",                     "金")                                         \
    X(Np_HintP2Score,          "P2 score",                 "2Pスコア")                                   \
    X(Np_HintPxOutward,        "px outward",               "px 外側へ")                                  \
    X(Np_MenuSafe,             "Menu-safe",                "メニュー回避")                              \
    X(Np_HintNameRow,          "name row",                 "名前の行")                                   \
    X(Np_Large,                "Large",                    "大")                                         \
    X(Np_Overlay,              "Overlay",                  "オーバーレイ")                              \
    X(Np_HintNicknameDraw,     "nickname draw",            "ニックネーム描画")                          \
    X(Np_NoteColorsSent,       "Colors sent to opponent",  "色は対戦相手に送信されます")                \
    X(Np_NoteNamePosition,     "Name position applies to both sides", "名前の位置は両側に適用されます") \
    X(Np_NoteTopDrops,         "Top drops to menu-safe row", "上部はメニュー回避行に下がります")        \
    X(Np_NoteGameDraw,         "Game draw; stats stay overlay", "ゲーム描画。統計はオーバーレイのまま") \
    X(Np_Route,                "Route",                    "経路")                                       \
    X(Np_HintPortMapping,      "< Off > port mapping",     "< オフ > ポートマッピング")                 \
    X(Np_HintNatTraversal,     "< Off > NAT traversal",    "< オフ > NAT越え")                          \
    X(Np_HintDirectConnect,    "< Off > direct connect",   "< オフ > 直接接続")                          \
    X(Np_HintDualStack,        "< Off > dual-stack",       "< オフ > デュアルスタック")                 \
    X(Np_CustomRelay,          "Custom relay",             "カスタム中継")                              \
    X(Np_DefaultRelay,         "Default relay",            "既定の中継")                                 \
    X(Np_PunchRelay,           "Punch Relay",              "パンチ中継")                                 \
    X(Np_StunServer,           "STUN Server",              "STUNサーバー")                               \
    X(Np_HintAllowSpectation,  "< Off > allow spectation", "< オフ > 観戦を許可")                        \
    X(Np_HintShareColors,      "< Off > share colors",     "< オフ > 色を共有")                          \
    X(Np_HintSeeOpponent,      "< Off > see opponent",     "< オフ > 相手の色を表示")                    \
    X(Np_HintKeyEventsOnly,    "< Off > key events only",  "< オフ > 主要イベントのみ")                 \
    X(Np_StopConnecting,       "Stop connecting",          "接続を中止")                                 \
    X(Np_GameN,                "Game %u",                  "第%u試合")                                   \
    X(Np_CurrentGame,          "Current Game",             "現在の試合")                                 \
    X(Np_ReturnToMenu,         "Return to menu",           "メニューに戻る")                            \
    X(Np_Match,                "Match",                    "試合")                                       \
    X(Np_PortViewers,          "Port %u  Viewers %u",      "ポート %u  視聴者 %u")                        \
    X(Np_Rebroadcast,          "Rebroadcast",              "再配信")                                     \
    X(Np_Playback,             "Playback",                 "再生")                                       \
    X(Np_ReadyUp,              "Ready Up",                 "準備完了")                                   \
    X(Np_HintConfirmSettings,  "Confirm settings",         "設定を確定")                                 \
    X(Np_Disconnect,           "Disconnect",               "切断")                                       \
    X(Np_Guest,                "Guest",                    "ゲスト")                                     \
    X(Np_YouWaiting,           "You: Waiting",             "自分: 待機中")                               \
    X(Np_OppWaiting,           "Opp: Waiting",             "相手: 待機中")                               \
    X(Np_Ready,                "Ready",                    "準備完了")                                   \
    X(Np_Ping,                 "Ping",                     "Ping")                                       \
    X(Np_Rounds,               "Rounds",                   "ラウンド")                                   \
    X(Np_Stun,                 "STUN",                     "STUN")                                       \
    X(Np_StartNextGame,        "Start next game",          "次の試合を開始")                            \
    X(Np_StayConnected,        "Stay connected",           "接続を維持")                                 \
    X(Np_CharacterSelect,      "Character select",         "キャラクター選択")                          \
    X(Np_LeaveRoom,            "Leave the room",           "ルームを退出")                              \
    X(Np_ClearMessage,         "Clear message",            "メッセージを消去")                          \
    /* --- netplay status messages ------------------------------------------------ */                \
    X(Np_MsgMainWatchNoReply,  "The main watch address did not respond. Trying the relay watch address...", "メインの観戦アドレスが応答しません。中継の観戦アドレスを試しています...") \
    X(Np_MsgEnterHostAddress,  "Enter the host address as host:port or [ipv6]:port.", "ホストアドレスを host:port または [ipv6]:port の形式で入力してください。") \
    X(Np_MsgConnSettingsInvalid, "Connection settings are invalid.", "接続設定が無効です。")             \
    X(Np_MsgPunchRelayInvalid, "The punch relay address is invalid.", "パンチ中継アドレスが無効です。")  \
    X(Np_MsgJoinFailed,        "Couldn't start joining the room.", "ルームへの参加を開始できませんでした。") \
    X(Np_MsgConnectingToHost,  "Connecting to host...",    "ホストに接続中...")                          \
    X(Np_MsgReturnedToJoin,    "Returned to Join a Match.", "試合参加画面に戻りました。")                \
    X(Np_MsgHostMayBePlaying,  "The host may already be playing. Checking whether a live watch feed is available...", "ホストは対戦中かもしれません。観戦配信があるか確認しています...") \
    X(Np_MsgStunInvalid,       "The STUN server address is invalid.", "STUNサーバーアドレスが無効です。") \
    X(Np_MsgTurnInvalid,       "The TURN server address is invalid.", "TURNサーバーアドレスが無効です。") \
    X(Np_MsgOpeningOnlineMenu, "Opening the online menu.", "オンラインメニューを開きます。")            \
    X(Np_MsgChooseOption,      "Choose an online option.", "オンラインの項目を選択してください。")      \
    X(Np_MsgClosingOnlineMenu, "Closing the online menu.", "オンラインメニューを閉じます。")            \
    X(Np_MsgSessionConnected,  "Session connected.",       "セッションが接続されました。")              \
    X(Np_MsgWaitingSelection,  "Waiting for network menu selection.", "ネットワークメニューの選択を待っています。") \
    X(Np_MsgDisplayName,       "Display name: %s",         "表示名: %s")                                 \
    X(Np_MsgRoomPort,          "Room port: %u",            "ルームポート: %u")                           \
    X(Np_MsgRoomPortRange,     "Enter a valid room port from 1 to 65535.", "1〜65535の有効なルームポートを入力してください。") \
    X(Np_MsgHostAddress,       "Host address: %s",         "ホストアドレス: %s")                         \
    X(Np_MsgEnterAddress,      "Enter an address as host:port or [ipv6]:port.", "アドレスを host:port または [ipv6]:port の形式で入力してください。") \
    X(Np_MsgWatchAddress,      "Watch address: %s",        "観戦アドレス: %s")                           \
    X(Np_MsgWatchAddressInvalid, "Enter a valid watch address.", "有効な観戦アドレスを入力してください。") \
    X(Np_MsgWatchPort,         "Watch port: %u",           "観戦ポート: %u")                             \
    X(Np_MsgWatchPortRange,    "Enter a valid watch port from 1 to 65535.", "1〜65535の有効な観戦ポートを入力してください。") \
    X(Np_MsgPunchRelayReset,   "Punch relay reset to the default server.", "パンチ中継を既定のサーバーに戻しました。") \
    X(Np_MsgPunchRelayCustom,  "Punch relay set to custom server.", "パンチ中継をカスタムサーバーに設定しました。") \
    X(Np_MsgPunchRelayEnter,   "Enter a valid punch relay address.", "有効なパンチ中継アドレスを入力してください。") \
    X(Np_MsgStunReset,         "STUN server reset to the default address.", "STUNサーバーを既定のアドレスに戻しました。") \
    X(Np_MsgStunServer,        "STUN server: %s",          "STUNサーバー: %s")                           \
    X(Np_MsgStunEnter,         "Enter a valid STUN server address.", "有効なSTUNサーバーアドレスを入力してください。") \
    X(Np_MsgLanRoomMatch,      "LAN room %u/%u: %s (%s vs %s)", "LANルーム %u/%u: %s (%s vs %s)")        \
    X(Np_MsgLanRoomWaiting,    "LAN room %u/%u: %s (waiting for a match)", "LANルーム %u/%u: %s (試合待ち)") \
    X(Np_MsgWatchRedirected,   "This watch address redirected you to %s.", "この観戦アドレスから %s へ転送されました。") \
    X(Np_MsgMatchStartedWatching, "A match just started. You're now watching.", "試合が始まりました。観戦中です。") \
    X(Np_MsgCouldNotReachHost, "Could not reach the host.", "ホストに接続できませんでした。")           \
    X(Np_MsgNowWatching,       "Now watching the live match.", "試合を観戦中です。")                     \
    X(Np_MsgNoRoomAdvertised,  "This watch server did not advertise a room address to join.", "この観戦サーバーは参加用ルームアドレスを公開していません。") \
    X(Np_MsgWaitingHostStart,  "Waiting for the host to start a match.", "ホストの試合開始を待っています。") \
    X(Np_MsgStabilityBias,     "Stability bias: %d",       "安定性バイアス: %d")                         \
    /* --- replay browser --------------------------------------------------------- */                \
    X(Rp_Name,                 "Name",                     "名前")                                       \
    X(Rp_Length,               "Length",                   "時間")                                       \
    X(Rp_Folder,               "Folder",                   "フォルダ")                                   \
    X(Rp_GoUpOneFolder,        "Go up one folder",         "上のフォルダへ")                            \
    X(Rp_NoReplayDir,          "No replay directory found at replay/.", "replay/ にリプレイフォルダがありません。") \
    X(Rp_NoReplays,            "No replay folders or files found under replay/.", "replay/ 以下にリプレイが見つかりません。") \
    X(Rp_FolderEmpty,          "Folder is empty. Press Esc or Backspace to return.", "フォルダは空です。EscまたはBackspaceで戻ります。") \
    X(Rp_NoReplaysHere,        "No replay folders or files are available here.", "ここには利用できるリプレイがありません。") \
    X(Rp_LongestMatch,         "Longest Match %s",         "最長試合 %s")                                \
    X(Rp_TotalTime,            "Total time %s",            "合計時間 %s")                                \
    X(Rp_MostPlayed,           "Most played characters:   P1: %s   P2: %s", "最多使用キャラ:   1P: %s   2P: %s") \
    X(Rp_ExitedPlayback,       "Exited replay playback.",  "リプレイ再生を終了しました。")              \
    /* --- netplay HUD ------------------------------------------------------------ */                \
    X(Hud_NamePosition,        "Name position",            "名前の位置")                                 \
    X(Hud_OpponentStopped,     "Opponent's game stopped responding (%us)", "相手のゲームが応答していません (%u秒)") \
    /* --- ImGui mod menu --------------------------------------------------------- */                \
    X(Mm_File,                 "File",                     "ファイル")                                   \
    X(Mm_SaveConfig,           "Save Config",              "設定を保存")                                 \
    X(Mm_LoadConfig,           "Load Config",              "設定を読込")                                 \
    X(Mm_HideMenu,             "Hide Menu",                "メニューを隠す")                            \
    X(Mm_View,                 "View",                     "表示")                                       \
    X(Mm_AdvancedMode,         "Advanced Mode",            "詳細モード")

enum class Str : uint16_t {
#define AS2_UI_STR_ENUM(id, en, ja) id,
    AS2_UI_STRINGS(AS2_UI_STR_ENUM)
#undef AS2_UI_STR_ENUM
    Count,
};

/// The string in the current language. Never null.
const char* S(Str id);

/// The string in a specific language, regardless of the current one.
const char* SIn(Str id, Lang lang);

void Strings_SetLanguage(Lang lang);
Lang Strings_Language();

/// Two-letter code for the ini ("en", "ja"); parses either back.
const char* Strings_LanguageCode(Lang lang);
bool Strings_ParseLanguageCode(const char* code, Lang* out);

/// Every Japanese string concatenated, for the font atlas to collect glyphs
/// from. The proxy bakes exactly these codepoints into the large menu face
/// instead of the full CJK block, which would not fit at that size.
const char* Strings_JapaneseGlyphText();

} // namespace Ui

using Ui::S;
using Ui::Str;
