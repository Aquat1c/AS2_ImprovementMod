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
