" Vim syntax file for gogo (.gg). gogo is Go syntax: load Vim's Go syntax,
" then mark gogo's own //gogo: directives and a #! first line.
if exists('b:current_syntax')
  finish
endif

runtime! syntax/go.vim
unlet! b:current_syntax

syntax match gogoDirective /^\s*\/\/gogo:\a\+.*$/ containedin=ALL
syntax match gogoShebang /\%1l^#!.*$/

highlight default link gogoDirective PreProc
highlight default link gogoShebang Comment

let b:current_syntax = 'gogo'
