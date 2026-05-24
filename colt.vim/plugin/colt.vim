if exists('g:loaded_colt_plugin') | finish | endif
let g:loaded_colt_plugin = 1

let s:save_cpo = &cpo
set cpo&vim

command! -nargs=* -range -bang Colt
  \ <line1>,<line2>call colt#align(<bang>0, 0, 'command', <q-args>)

command! -nargs=0 -range ColtAlign
  \ <line1>,<line2>call colt#align(0, 0, 'command', '')

function! s:abs(v)
  return a:v >= 0 ? a:v : -a:v
endfunction

function! s:remember_visual(mode)
  let s:last_visual = [
  \   a:mode,
  \   s:abs(line("'>") - line("'<")),
  \   s:abs(col("'>")  - col("'<"))
  \ ]
endfunction

function! s:set_repeat()
  silent! call repeat#set("\<Plug>(ColtRepeat)")
endfunction

function! s:colt_op(type, ...)
  if !&modifiable | return | endif

  let sel_save   = &selection
  let &selection = 'inclusive'

  if a:0   " called from visual mapping
    let vmode   = a:type
    let [l1,l2] = [line("'<"), line("'>")]
    call s:remember_visual(vmode)
  else
    let vmode   = ''
    let [l1,l2] = [line("'["), line("']")]
    unlet! s:last_visual
  endif

  try
    if get(g:, 'colt_need_repeat', 0)
      execute l1.','.l2 . g:colt_last_command
    else
      call colt#align(0, 0, vmode, '')
    endif
    call s:set_repeat()
  finally
    let &selection = sel_save
  endtry
endfunction

function! s:colt_op_visual(type, ...)
  call s:colt_op(a:type, 1)
endfunction

function! s:repeat_visual()
  let [mode, ldiff, cdiff] = s:last_visual
  let cmd = 'normal! ' . mode
  if ldiff > 0 | let cmd .= ldiff . 'j' | endif

  let ve_save = &virtualedit
  try
    if mode ==# "\<C-V>"
      if cdiff > 0 | let cmd .= cdiff . 'l' | endif
      set virtualedit+=block
    endif
    execute cmd . ":\<C-r>=g:colt_last_command\<CR>\<CR>"
    call s:set_repeat()
  finally
    if ve_save !=# &virtualedit | let &virtualedit = ve_save | endif
  endtry
endfunction

function! s:colt_repeat()
  if exists('s:last_visual')
    call s:repeat_visual()
  else
    try
      let g:colt_need_repeat = 1
      normal! .
    finally
      unlet! g:colt_need_repeat
    endtry
  endif
endfunction

function! s:repeat_in_visual()
  if exists('g:colt_last_command')
    call s:remember_visual(visualmode())
    call s:repeat_visual()
  endif
endfunction

" Normal mode: operator (ga then motion)
nnoremap <silent> <Plug>(Colt)
  \ :set opfunc=<SID>colt_op<CR>g@

" Visual mode: ga on selection
xnoremap <silent> <Plug>(Colt)
  \ :<C-U>call <SID>colt_op(visualmode(), 1)<CR>

" Repeat
nnoremap <silent> <Plug>(ColtRepeat)
  \ :call <SID>colt_repeat()<CR>
xnoremap <silent> <Plug>(ColtRepeat)
  \ :<C-U>call <SID>repeat_in_visual()<CR>

let &cpo = s:save_cpo
unlet s:save_cpo

" vim: set et sw=2 ts=2 :
