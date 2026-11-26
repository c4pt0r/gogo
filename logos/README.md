# gogo brand assets

The mascot and the two lockups, as supplied by the designer, plus the white
variants for dark surfaces. The rules are in
[gogo-design-guide.pdf](gogo-design-guide.pdf) (five pages); this file is a
short summary of them and an index of the files.

## Files

| File | Use |
|---|---|
| `gogo-octopus.svg` / `.png` | The mascot alone, monochrome (the default). Avatar, icon, device screen, any square placement. |
| `gogo-octopus-color.svg` / `.png` | The mascot in deep teal and pale mint. |
| `gogo-octopus-white.svg` / `.png` | The mascot for dark surfaces (see below). |
| `gogo-lang.svg` / `.png`, `-color`, `-white` | The "Gogo Lang" lockup: the language, the compiler and the documentation (the book). |
| `gogo-vm.svg` / `.png`, `-color`, `-white` | The "Gogo VM" lockup: the runtime, the firmware and embedding. |
| `gogo-logo-preview.png` | The designer's overview of the supplied marks. |
| `gogo-design-guide.pdf` | The design guide, v1.0. |
| `gogo-social-preview.png` | 1280×640 image for the GitHub repository's social preview (Settings, General, Social preview). |

SVG is the master; the PNGs (1000 px wide, transparent background) are for
places that need a raster. The `-white` PNGs were rendered from their SVGs
with headless Chrome. The site and the book use the mascot as a favicon:
`docs/book/theme/favicon.svg` is `gogo-octopus-color.svg` with a square
viewBox around the artwork, and `favicon.png` a 192 px rendering of it.

## Naming

`gogo-octopus`, `gogo-lang`, `gogo-vm`, each `.svg` and `.png`. No suffix is
the monochrome artwork, `-color` the teal and mint variant, `-white` the
reverse for dark backgrounds. In code and on the command line the name is
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

## The white variants

The guide asks for a white reverse mark on dark surfaces that keeps the
eye highlights and the face legible. The `-white` files are the `-color`
files with deep teal (`#005550`) replaced by white, and nothing else
changed: the outline, the arms and the wordmark are white, the face keeps
its pale mint fill, and the eyes and smile stay ink with their highlights.

Three versions were rendered on `#0d1117` (GitHub's dark background) and on
`#000000`, at 400, 32 and 24 px:

1. **The literal reverse of the monochrome mark** (every black fill made
   white). The silhouette is intact, but the eyes become white discs with
   a dark dot where the highlight was. The highlight is gone, and the dot
   reads as a pupil looking up and to the right: the expression changes,
   which the guide forbids.
2. **All white, eyes ink**: the `-color` artwork with both teal and mint
   made white, so the eyes sit on a white face as they do on paper. The
   face is right, but the mint fill also covers the webbing between the
   arms; in white it merges the arms with the body, leaves thin dark
   slivers between them at large sizes, and loses the outline that makes
   the mascot.
3. **White outline and arms, pale mint face, ink eyes** (chosen). The face
   is exactly the original, highlights included; the arms and the outline
   stay separate, because the mint face and the webbing read as a surface
   behind the white line work, as they do in the `-color` mark; and it
   holds up at 24 px. Pale
   mint is a brand color, and here it is a large surface on a dark
   background, never small text.

The monochrome artwork itself is unchanged, so on a 1-bit display use
`gogo-octopus.svg`, inverted by the display if it must be.
