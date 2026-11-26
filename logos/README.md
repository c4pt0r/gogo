# gogo brand assets

The mascot and the two lockups, exactly as supplied by the designer. The rules are in
[gogo-design-guide.pdf](gogo-design-guide.pdf) (five pages); this file is a
short summary of them and an index of the files.

## Files

| File | Use |
|---|---|
| `gogo-octopus.svg` / `.png` | The mascot alone, monochrome (the default). Avatar, icon, device screen, any square placement. |
| `gogo-octopus-color.svg` / `.png` | The mascot in deep teal and pale mint. |
| `gogo-lang.svg` / `.png`, `-color` | The "Gogo Lang" lockup: the language, the compiler and the documentation (the book). |
| `gogo-vm.svg` / `.png`, `-color` | The "Gogo VM" lockup: the runtime, the firmware and embedding. |
| `gogo-logo-preview.png` | The designer's overview of the supplied marks. |
| `gogo-design-guide.pdf` | The design guide, v1.0. |
| `gogo-social-preview.png` | 1280×640 image for the GitHub repository's social preview (Settings, General, Social preview). |

SVG is the master; the PNGs (1000 px wide, transparent background) are for
places that need a raster. The site and the book use the mascot as a favicon:
`docs/book/theme/favicon.svg` is `gogo-octopus-color.svg` with a square
viewBox around the artwork, and `favicon.png` a 192 px rendering of it.

## Naming

`gogo-octopus`, `gogo-lang`, `gogo-vm`, each `.svg` and `.png`. No suffix is
the monochrome artwork, `-color` the teal and mint variant. In code and on the command line the name is
`gogo`, lowercase; in visual assets use the supplied lockups.

## Rules in short

- Use the supplied SVGs. Never re-typeset the "Gogo Lang" or "Gogo VM"
  wordmarks; they are outlined paths.
- Never stretch, rotate or crop the marks, add gradients, shadows or
  effects, change the expression or the arms, or place a mark on busy
  imagery.
- Clear space: X on every side, X = 1/5 of the mascot's visible height.
- Minimum sizes: the mascot 24 px wide, a lockup 140 px wide. Below that,
  use the mascot alone.
- Color: ink `#000000`, deep teal `#005550`, pale mint `#DFFAF7`, white
  `#FFFFFF`. Monochrome is the default and teal the optional accent. Text on
  white or mint is teal or black; pale mint is never used for small text on
  white.
- Type: Inter / Inter Display for text, a monospace font for code.
- Embedded displays: rasterize the monochrome SVG at the display's native
  resolution and check it on the real (often 1-bit) display.

## Colors are the designer's

The artwork is used exactly as supplied: no recolored or derived variants.
The README, the website and the book show `gogo-octopus-color.svg` and
`gogo-lang-color.svg` on light and dark backgrounds alike. A reverse mark
for dark surfaces, which the guide mentions, is the designer's to make.
