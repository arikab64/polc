" Buffer-local settings for the gc policy DSL.
if exists("b:did_ftplugin") | finish | endif
let b:did_ftplugin = 1

" Comments: scanner accepts both '#' (shell-style) and '//' (C-style).
" commentstring is what 'gcc' / vim-commentary and friends use for toggling.
setlocal commentstring=#\ %s
setlocal comments=:#,://

" Four-space indent, matching the hand-written sample file.
setlocal expandtab
setlocal shiftwidth=4
setlocal softtabstop=4

" Treat hyphens as part of identifiers so OVERRIDE-ALLOW, app:web-front,
" and asset names like web-prod-1 are a single word for 'w', '*', etc.
setlocal iskeyword+=-
setlocal iskeyword+=:
