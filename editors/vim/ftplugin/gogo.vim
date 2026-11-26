" gogo files indent like Go: tabs, and Go's comment format.
if exists('b:did_ftplugin')
  finish
endif
runtime! ftplugin/go.vim
let b:did_ftplugin = 1
setlocal noexpandtab tabstop=4 shiftwidth=4
setlocal commentstring=//\ %s
