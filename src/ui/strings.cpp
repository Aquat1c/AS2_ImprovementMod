#include "ui/strings.h"

#include <string.h>

namespace Ui {

namespace {

#define AS2_UI_STR_EN(id, en, ja) en,
#define AS2_UI_STR_JA(id, en, ja) ja,

const char* const kEnglish[] = { AS2_UI_STRINGS(AS2_UI_STR_EN) };
const char* const kJapanese[] = { AS2_UI_STRINGS(AS2_UI_STR_JA) };

#undef AS2_UI_STR_EN
#undef AS2_UI_STR_JA

static_assert(sizeof(kEnglish) / sizeof(kEnglish[0]) == (size_t)Str::Count,
              "English table drifted from the string list");
static_assert(sizeof(kJapanese) / sizeof(kJapanese[0]) == (size_t)Str::Count,
              "Japanese table drifted from the string list");

Lang g_lang = Lang::English;

// Built once: every Japanese string back to back, separated by spaces so the
// glyph builder sees each one. Sized generously; the assert below is the
// guard, not the estimate.
char g_japaneseGlyphText[24 * 1024];
bool g_japaneseGlyphTextBuilt = false;

} // namespace

const char* SIn(Str id, Lang lang) {
    const size_t index = (size_t)id;
    if (index >= (size_t)Str::Count) {
        return "?";
    }
    const char* text = (lang == Lang::Japanese) ? kJapanese[index] : kEnglish[index];
    // An empty Japanese cell falls back to English rather than to nothing.
    if (!text || !text[0]) {
        text = kEnglish[index];
    }
    return text ? text : "?";
}

const char* S(Str id) {
    return SIn(id, g_lang);
}

void Strings_SetLanguage(Lang lang) {
    g_lang = (lang < Lang::Count) ? lang : Lang::English;
}

Lang Strings_Language() {
    return g_lang;
}

const char* Strings_LanguageCode(Lang lang) {
    return lang == Lang::Japanese ? "ja" : "en";
}

bool Strings_ParseLanguageCode(const char* code, Lang* out) {
    if (!code || !out) {
        return false;
    }
    if (_stricmp(code, "ja") == 0 || _stricmp(code, "jp") == 0 || _stricmp(code, "japanese") == 0) {
        *out = Lang::Japanese;
        return true;
    }
    if (_stricmp(code, "en") == 0 || _stricmp(code, "english") == 0) {
        *out = Lang::English;
        return true;
    }
    return false;
}

const char* Strings_JapaneseGlyphText() {
    if (!g_japaneseGlyphTextBuilt) {
        size_t used = 0;
        for (size_t i = 0; i < (size_t)Str::Count; ++i) {
            const char* text = kJapanese[i];
            if (!text) {
                continue;
            }
            const size_t len = strlen(text);
            if (used + len + 2 >= sizeof(g_japaneseGlyphText)) {
                break;   // truncated coverage beats an overrun; the assert notices
            }
            memcpy(g_japaneseGlyphText + used, text, len);
            used += len;
            g_japaneseGlyphText[used++] = ' ';
        }
        g_japaneseGlyphText[used] = '\0';
        g_japaneseGlyphTextBuilt = true;
    }
    return g_japaneseGlyphText;
}

} // namespace Ui
