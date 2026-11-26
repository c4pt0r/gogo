# Editor support for gogo

gogo source files end in `.gg` and are Go syntax. The files here give
editors a `gogo` file type: Go highlighting and indentation, plus gogo's
own `//gogo:` directives and `#!/usr/bin/env -S gogo run` scripts. Each
editor also gets **gogo mono**, the black and white color scheme of the
gogo site: highlighting by contrast and weight instead of hue (keywords
bold, types underlined, comments faint and italic).

| Editor | Directory | What |
|---|---|---|
| VS Code | `vscode/` | the `gogo` language, its grammar (VS Code's Go grammar plus directives), and the themes *gogo mono light* and *gogo mono dark* |
| Vim / Neovim | `vim/` | file type detection, syntax (Vim's Go syntax plus directives), indent and ftplugin; the color scheme `gogo-mono` |
| Emacs | `emacs/` | `gogo-mode`, derived from `go-mode` |

This is syntax only. `gopls` and `go vet` do not apply: the Go toolchain
does not read `.gg` files, and gogo's packages (`gogo/actor`, ...) exist
only inside the gogo compiler. `gogo build` is the checker.

GitHub highlights `.gg` files as Go through the repository's
`.gitattributes`.

## VS Code

Link the extension into VS Code's extension directory and restart VS Code:

```sh
ln -s "$PWD/editors/vscode" ~/.vscode/extensions/gogo-lang
```

`.gg` files then open as *gogo*. The color themes are under
*Preferences: Color Theme*. To use them only for gogo files, keep your
theme and leave these out; the grammar works with any theme.

## Vim and Neovim

Add the directory to the runtime path, in `vimrc` or `init.vim`:

```vim
set runtimepath^=/path/to/gogo/editors/vim
filetype plugin indent on
syntax on
" optional: the black and white scheme (true color)
set termguicolors
colorscheme gogo-mono   " follows 'background': light or dark
```

or in Lua for Neovim:

```lua
vim.opt.runtimepath:prepend("/path/to/gogo/editors/vim")
vim.cmd.colorscheme("gogo-mono")
```

With a plugin manager, point it at `editors/vim` as a local plugin.

## Emacs

Install `go-mode`, then:

```elisp
(add-to-list 'load-path "/path/to/gogo/editors/emacs")
(require 'gogo-mode)
```

`.gg` files and scripts whose `#!` line runs `gogo` open in `gogo-mode`.

## Any other editor

Map the `.gg` extension to Go. That gives everything but the directive
highlighting.
