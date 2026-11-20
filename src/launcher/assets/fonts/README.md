# Fonts of the launcher

| Files | Family | Licence |
|---|---|---|
| `Onest-Regular.ttf`, `-Medium`, `-SemiBold`, `-Bold` | Onest, (c) 2021 The Onest Project Authors | SIL Open Font License 1.1, `OFL-Onest.txt` |
| `JetBrainsMono-Medium.ttf`, `-Bold` | JetBrains Mono, (c) 2020 The JetBrains Mono Project Authors | SIL Open Font License 1.1, `OFL-JetBrainsMono.txt` |

These are **modified versions**: made by `tools/launcher_fonts.py` from the files Google Fonts serves (variable
fonts, one file per range of characters). For each weight the Latin and the Cyrillic file were pinned at that
weight and merged into one static file; the layout tables (kerning, alternates) were left out. Neither family
declares a Reserved Font Name. The fonts are compiled into `tmnt2net-launcher.exe` as resources.

The headings' font of the design is Dela Gothic One (OFL). When `DelaGothicOne-Regular.ttf` (with its
`OFL-DelaGothicOne.txt`) is in this folder, the build puts it into the exe; without it the launcher draws the
headings with Arial Black of Windows.
