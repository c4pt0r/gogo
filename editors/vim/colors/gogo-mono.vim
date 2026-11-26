" gogo mono: black and white, highlighting by contrast and weight, as on
" the gogo site. Follows 'background' (set background=dark for white on
" black). Needs a true-color terminal (set termguicolors) or the GUI.
highlight clear
if exists('syntax_on')
  syntax reset
endif
let g:colors_name = 'gogo-mono'

if &background ==# 'dark'
  let s:bg = '#000000' | let s:fg = '#9a9a9a' | let s:ink = '#ffffff'
  let s:text = '#e8e8e8' | let s:faint = '#5e5e5e' | let s:line = '#333333'
else
  let s:bg = '#ffffff' | let s:fg = '#6b6b6b' | let s:ink = '#000000'
  let s:text = '#1a1a1a' | let s:faint = '#a3a3a3' | let s:line = '#e6e6e6'
endif

function! s:hi(group, fg, bg, attr) abort
  execute 'highlight' a:group 'guifg=' . a:fg 'guibg=' . a:bg 'gui=' . a:attr 'cterm=' . a:attr
endfunction

call s:hi('Normal',       s:fg,    s:bg,   'NONE')
call s:hi('Comment',      s:faint, 'NONE', 'italic')
call s:hi('Statement',    s:ink,   'NONE', 'bold')
call s:hi('Keyword',      s:ink,   'NONE', 'bold')
call s:hi('Conditional',  s:ink,   'NONE', 'bold')
call s:hi('Repeat',       s:ink,   'NONE', 'bold')
call s:hi('PreProc',      s:ink,   'NONE', 'bold')
call s:hi('Function',     s:ink,   'NONE', 'NONE')
call s:hi('Type',         s:text,  'NONE', 'underline')
call s:hi('String',       s:text,  'NONE', 'NONE')
call s:hi('Character',    s:text,  'NONE', 'NONE')
call s:hi('Number',       s:ink,   'NONE', 'NONE')
call s:hi('Float',        s:ink,   'NONE', 'NONE')
call s:hi('Boolean',      s:ink,   'NONE', 'NONE')
call s:hi('Constant',     s:ink,   'NONE', 'NONE')
call s:hi('Identifier',   s:fg,    'NONE', 'NONE')
call s:hi('Operator',     s:fg,    'NONE', 'NONE')
call s:hi('Special',      s:text,  'NONE', 'NONE')
call s:hi('Todo',         s:ink,   'NONE', 'bold')
call s:hi('LineNr',       s:faint, s:bg,   'NONE')
call s:hi('CursorLineNr', s:ink,   s:bg,   'NONE')
call s:hi('CursorLine',   'NONE',  s:bg,   'underline')
call s:hi('Visual',       'NONE',  s:line, 'NONE')
call s:hi('StatusLine',   s:ink,   s:bg,   'reverse')
call s:hi('StatusLineNC', s:faint, s:bg,   'reverse')
call s:hi('VertSplit',    s:line,  s:bg,   'NONE')
call s:hi('Pmenu',        s:fg,    s:line, 'NONE')
call s:hi('PmenuSel',     s:bg,    s:ink,  'NONE')
call s:hi('Search',       s:bg,    s:ink,  'NONE')
call s:hi('MatchParen',   s:ink,   'NONE', 'bold,underline')
