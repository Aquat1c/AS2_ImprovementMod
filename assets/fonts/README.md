# Embedded overlay font

`NotoSansCJKjp-Regular.ttf` is compiled into `d3d9.dll` as an RCDATA resource
(`IDR_OVERLAY_FONT`) and loaded by `ConfigureOverlayFonts`. It gives the overlay
Latin, Japanese and Cyrillic coverage without depending on which fonts the host
machine happens to have installed, so nicknames render the same everywhere.

Noto Sans CJK JP is licensed under the SIL Open Font License, Version 1.1, which
permits redistribution (including bundled inside a binary) provided the license
travels with it. The full text is at:

    https://scripts.sil.org/OFL

Reserved Font Name: none claimed by this project. The font is redistributed
unmodified.

If the resource is missing or fails to load, the loader falls back to the
system fonts (MS Gothic, Yu Gothic, Meiryo) and finally to the ImGui default.

## Menu font and Japanese

`ShipporiMincho-Bold.ttf` (`IDR_MENU_FONT`) is the face the mod's own menus use,
baked twice: 19 px with the full Japanese + Cyrillic ranges, and 49 px for the
pause-menu rows. The full CJK block does not fit at 49 px, so the large bake
carries Latin, merged Cyrillic, and **exactly the codepoints the mod's string
table uses**: the mod hands its Japanese strings to `AS2Proxy_SetMenuGlyphText`
(from `ui/strings.h`) and the proxy adds those glyphs to the large face,
rebuilding the atlas between frames if the table arrives after the device did.

Draw and measure both route a string to the large face only when it has every
glyph the string needs (`FontCoversText`), otherwise to the 19 px one - so a
Japanese nickname from another player still renders even though it is not in
the table.
