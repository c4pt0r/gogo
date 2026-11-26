" gogo source files (.gg), and #! scripts run by gogo.
autocmd BufRead,BufNewFile *.gg setfiletype gogo
autocmd BufRead,BufNewFile * if getline(1) =~# '^#!.*\<gogo\>' | setfiletype gogo | endif
