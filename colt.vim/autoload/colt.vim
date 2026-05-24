" autoload/colt.vim - interactive alignment engine for colt
"
" Entry points:
"   colt#align(fl, ll, visualmode, expr)   called by plugin/colt.vim
"   colt#run(fl, ll, flags)                run colt binary directly

let s:save_cpo = &cpo
set cpo&vim


function! s:bin()
  if exists('g:colt_bin')
    return g:colt_bin
  endif
  let l:local = expand('<sfile>:p:h:h') . '/bin/colt'
  if executable(l:local)
    return l:local
  endif
  return 'colt'
endfunction

let s:KEYS = {
      \  '=': '-d=',
      \  ':': '-d: -s -l0 -r1',
      \  ',': '-d, -s -l0 -r1',
      \  '|': "-d'|'",
      \  '.': '-P dot',
      \  ';': '-P semi',
      \  '&': '-P amp',
      \  '>': '-P arrow',
      \  '/': '-P comment',
      \  '\': '-P backslash',
      \  '[': '-P bracket',
      \  '(': '-P paren',
      \  ' ': '-d" " -l0 -r0',
      \ }

let s:MODE_LABEL = {'l': '[L]', 'r': '[R]', 'c': '[C]'}
let s:ALIGN_CYCLE = ['l', 'r', 'c']

function! s:hud(st)
  let l:toks = []

  call add(l:toks, ['Function',   ':Colt'])
  call add(l:toks, ['ModeMsg',    ' ' . get(s:MODE_LABEL, a:st.align, '?')])

  if a:st.nth ==# '**'
    call add(l:toks, ['Repeat', ' **'])
  elseif a:st.nth =~# '^\*'
    call add(l:toks, ['Repeat',   ' ' . a:st.nth])
  elseif a:st.nth =~# '^-'
    call add(l:toks, ['Special',  ' ' . a:st.nth])
  else
    call add(l:toks, ['Number',   ' ' . a:st.nth])
  endif

  if !empty(a:st.delim)
    if a:st.regex
      call add(l:toks, ['Delimiter', ' /'])
      call add(l:toks, ['String',    a:st.delim])
      call add(l:toks, ['Delimiter', '/'])
    else
      let l:dk = a:st.delim ==# ' ' ? '<sp>' : a:st.delim
      call add(l:toks, ['Identifier', ' ' . l:dk])
    endif
  else
    call add(l:toks, ['Comment', '  (key?)'])
  endif

  if a:st.stick
    call add(l:toks, ['Statement', ' <'])
  endif
  if a:st.live
    call add(l:toks, ['WarningMsg', ' ~live~'])
  endif
  if !empty(a:st.warn)
    call add(l:toks, ['WarningMsg', '  ' . a:st.warn])
  endif

  call s:echo_toks(l:toks)
endfunction

function! s:echo_toks(toks)
  let l:rs = &ruler | let l:sc = &showcmd
  try
    set noruler noshowcmd
    let l:w   = winwidth(0) - 2
    let l:all = join(map(copy(a:toks), 'v:val[1]'), '')
    let l:ell = len(l:all) > l:w ? '..' : ''
    let l:used = 0
    echon "\r"
    for [l:hl, l:txt] in a:toks
      if empty(l:txt)
        continue
      endif
      execute 'echohl ' . l:hl
      let l:room = l:w - len(l:ell) - l:used
      if len(l:txt) > l:room
        echon l:txt[: l:room - 1] . l:ell
        break
      endif
      echon l:txt
      let l:used += len(l:txt)
    endfor
  finally
    echohl None
    let &ruler   = l:rs
    let &showcmd = l:sc
  endtry
endfunction

function! s:clear_hud()
  echon "\r\r"
endfunction

function! s:to_flags(st)
  let l:f = ''

  " delimiter / preset
  if a:st.regex
    let l:f .= '-e ' . shellescape(a:st.delim)
  elseif has_key(s:KEYS, a:st.delim)
    let l:f .= s:KEYS[a:st.delim]
  else
    let l:f .= '-d ' . shellescape(a:st.delim)
  endif

  if a:st.nth ==# '**'
    let l:f .= " -n'**'"
  elseif a:st.nth ==# '*'
    let l:f .= " -n'*'"
  elseif a:st.nth !=# '1'
    let l:f .= ' -n' . a:st.nth
  endif

  " alignment
  if a:st.align ==# 'r'
    let l:f .= ' -ar'
  elseif a:st.align ==# 'c'
    let l:f .= ' -ac'
  endif

  " margins
  if a:st.lm >= 0
    let l:f .= ' -l' . a:st.lm
  endif
  if a:st.rm >= 0
    let l:f .= ' -r' . a:st.rm
  endif

  " stick
  if a:st.stick
    let l:f .= ' -s'
  endif

  return l:f
endfunction

function! colt#run(fl, ll, flags)
  let l:bin = s:bin()
  if !executable(l:bin)
    call s:echo_toks([['ErrorMsg',
          \  'colt: binary not found - set g:colt_bin or add colt to $PATH']])
    return 0
  endif

  let l:input  = join(getline(a:fl, a:ll), "\n") . "\n"
  let l:result = systemlist(l:bin . ' ' . a:flags, l:input)

  if v:shell_error
    call s:echo_toks([['ErrorMsg', 'colt: ' . join(l:result, ' ')]])
    return 0
  endif

  let l:changed = 0
  let l:i = 0
  while l:i < len(l:result)
    let l:lnum = a:fl + l:i
    if l:lnum <= a:ll && l:result[l:i] !=# getline(l:lnum)
      call setline(l:lnum, l:result[l:i])
      let l:changed += 1
    endif
    let l:i += 1
  endwhile
  return l:changed
endfunction

function! s:interactive(fl, ll, vis)
  " State
  let l:st = {
        \  'align': 'l',
        \  'nth':   '1',
        \  'delim': '',
        \  'regex': 0,
        \  'stick': 0,
        \  'lm':    -1,
        \  'rm':    -1,
        \  'live':  0,
        \  'warn':  '',
        \ }

  let l:undo_done = 0

  if a:vis
    execute "normal! \<Esc>"
  endif

  while 1

    if l:st.live && !empty(l:st.delim)
      if l:undo_done
        silent! undo
        let l:undo_done = 0
      endif
      let l:flags = s:to_flags(l:st)
      let l:n = colt#run(a:fl, a:ll, l:flags)
      if l:n
        let &undolevels = &undolevels
      endif
      let l:undo_done = l:n > 0
      if a:vis
        normal! gv
      endif
      redraw
      if a:vis
        execute "normal! \<Esc>"
      endif
    else
      redraw
    endif

    call s:hud(l:st)
    let l:st.warn = ''

    try
      let l:c = getchar()
    catch /^Vim:Interrupt$/
      let l:c = 27
    endtry

    if type(l:c) == 0
      let l:ch = nr2char(l:c)
    else
      let l:ch = l:c
    endif

    if l:c == 27 || l:c == 3
      if l:undo_done
        silent! undo
      endif
      call s:clear_hud()
      throw 'colt:cancel'
    endif

    if l:c == 0x08 || l:c == 0x7f
      if !empty(l:st.delim)
        let l:st.delim = l:st.delim[: -2]
        if empty(l:st.delim)
          let l:st.regex = 0
        endif
      elseif l:st.nth !=# '1'
        if len(l:st.nth) > 1
          let l:st.nth = l:st.nth[: -2]
        else
          let l:st.nth = '1'
        endif
      endif
      continue
    endif

    if l:c == 13
      let l:idx = index(s:ALIGN_CYCLE, l:st.align)
      let l:st.align = s:ALIGN_CYCLE[(l:idx + 1) % 3]
      continue
    endif

    if l:ch ==# "\<Left>"
      let l:st.stick = 1
      let l:st.lm    = 0
      continue
    endif

    if l:ch ==# "\<Right>"
      let l:st.stick = 0
      let l:st.lm    = 1
      continue
    endif

    if l:ch ==# "\<Up>"
      let l:st.stick = 0
      let l:st.lm    = -1
      let l:st.rm    = -1
      continue
    endif

    if l:ch ==# "\<Down>"
      let l:st.lm = 0
      let l:st.rm = 0
      continue
    endif

    if l:ch ==# "\<C-P>"
      let l:st.live = !l:st.live
      if !l:st.live && l:undo_done
        silent! undo
        let l:undo_done = 0
      endif
      continue
    endif

    if l:ch ==# "\<C-X>"
      if l:st.regex && !empty(l:st.delim)
        " second <C-X> confirms
        break
      endif
      call s:clear_hud()
      let l:pat = input('regex: ')
      redraw
      if empty(l:pat)
        let l:st.warn = 'empty regex'
      else
        let l:st.delim = l:pat
        let l:st.regex = 1
        if !l:st.live
          break
        endif
      endif
      continue
    endif

    if empty(l:st.delim)

      if l:ch ==# '-'
        if l:st.nth =~# '^[*]'
          let l:st.nth = '1'
        elseif l:st.nth ==# '1'
          let l:st.nth = '-1'
        elseif l:st.nth =~# '^-'
          let l:st.nth = '-' . (str2nr(l:st.nth[1:]) + 1)
        else
          let l:st.nth = '-1'
        endif
        continue
      endif

      if l:ch ==# '*'
        if l:st.nth ==# '1' || l:st.nth =~# '^-'
          let l:st.nth = '*'
        elseif l:st.nth ==# '*'
          let l:st.nth = '**'
        else
          let l:st.nth = '1'
        endif
        continue
      endif

      " digits: set positive nth directly
      if l:ch =~# '^[1-9]$'
        if l:st.nth ==# '1' || l:st.nth =~# '^[*-]'
          let l:st.nth = l:ch
        else
          let l:st.nth .= l:ch
        endif
        continue
      endif

    endif

    if l:ch =~# '[[:print:]]'
      if empty(l:st.delim)
        let l:st.delim = l:ch
        if !l:st.live
          " non-live: single keypress confirms immediately
          break
        endif
      else
        " second press of the same char confirms
        if l:ch ==# l:st.delim && !l:st.regex
          break
        else
          let l:st.warn = "press '" . l:st.delim . "' again to confirm"
        endif
      endif
      continue
    endif

    let l:st.warn = 'unknown key'

  endwhile

  return l:st
endfunction

function! colt#align(fl, ll, visualmode, expr)
  " Resolve line range and vis flag
  if a:visualmode ==# 'command'
    let l:fl  = a:fl
    let l:ll  = a:ll
    let l:vis = (l:fl == line("'<") && l:ll == line("'>"))
  elseif empty(a:visualmode)
    let l:fl  = a:fl
    let l:ll  = a:ll
    let l:vis = 0
  else
    let l:fl  = line("'<")
    let l:ll  = line("'>")
    let l:vis = 1
  endif

  if !&modifiable
    if l:vis
      normal! gv
    endif
    return
  endif

  if !empty(a:expr)
    let l:changed = colt#run(l:fl, l:ll, a:expr)
    let g:colt_last_command = 'Colt ' . a:expr
    if l:changed
      redraw
      call s:echo_toks([
            \  ['Function', ':Colt '],
            \  ['Normal',   a:expr],
            \  ['Comment',  '  (' . l:changed . ' line'
            \              . (l:changed == 1 ? '' : 's') . ' changed)'],
            \ ])
    endif
    return
  endif

  try
    let l:st = s:interactive(l:fl, l:ll, l:vis)
  catch /^colt:cancel$/
    if l:vis
      normal! gv
    endif
    return
  endtry

  let l:flags = s:to_flags(l:st)

  if l:st.live
    silent! undo
  endif

  let l:changed = colt#run(l:fl, l:ll, l:flags)

  let g:colt_last_command = 'Colt ' . l:flags

  " Final status line
  redraw
  let l:dk = ''
  if !empty(l:st.delim)
    if l:st.regex
      let l:dk = '/' . l:st.delim . '/'
    elseif l:st.delim ==# ' '
      let l:dk = '<sp>'
    else
      let l:dk = l:st.delim
    endif
  endif

  let l:summary = l:dk
  if l:st.nth !=# '1'
    let l:summary .= ' n=' . l:st.nth
    if l:st.nth ==# '**'
      let l:summary .= ' (alt L/R)'
    elseif l:st.nth ==# '*'
      let l:summary .= ' (all)'
    elseif l:st.nth =~# '^-'
      let l:summary .= ' (from end)'
    endif
  endif
  if l:st.align !=# 'l'
    let l:summary .= ' ' . get(s:MODE_LABEL, l:st.align, '')
  endif
  if l:st.stick
    let l:summary .= ' <'
  endif

  call s:echo_toks([
        \  ['Function', ':Colt'],
        \  ['ModeMsg',  ' ' . get(s:MODE_LABEL, l:st.align, '')],
        \  ['Normal',   ' ' . l:summary],
        \  ['Comment',  '  (' . l:changed . ' line'
        \              . (l:changed == 1 ? '' : 's') . ')'],
        \ ])

  if l:vis
    normal! gv
  endif
endfunction

let &cpo = s:save_cpo
unlet s:save_cpo

" vim: set et sw=2 ts=2 :
