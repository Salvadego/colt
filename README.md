# colt - fast column aligner

A tiny, fast C binary that aligns columns in text, plus a Vim plugin to
drive it. Think `column -t` but with full control over delimiters, nth
occurrence, alignment mode, indentation, and more.

## Install

```sh
make all-in            # build + ~/bin/colt + ~/.vim/plugin/colt.vim
make all-in PREFIX=/usr/local   # system-wide binary
```

## CLI usage

```
colt [OPTIONS] [FILE...]
```

 | Flag       | Meaning                                           | Default   |
 | ------     | ---------                                         | --------- |
 | `-d <str>` | Delimiter string                                  | `=`       |
 | `-n <n>`   | Occurrence: `1`,`2`,`-1`(last),`*`(all)           | `1`       |
 | `-l <n>`   | Left margin around delimiter                      | `1`       |
 | `-r <n>`   | Right margin around delimiter                     | `1`       |
 | `-s`       | Stick delimiter to left token                     | off       |
 | `-a <m>`   | Token alignment: `l` `r` `c`                      | `l`       |
 | `-D <m>`   | Delimiter alignment: `l` `r` `c`                  | `r`       |
 | `-i <m>`   | Indentation: `k`eep `s`hallowest `d`eepest `n`one | `k`       |
 | `-t <n>`   | Tab stop width                                    | `8`       |
 | `-T`       | Table mode: split on whitespace runs              | off       |
 | `-S <sep>` | Table mode output separator                       | `' '`     |
 | `-g <pat>` | Only align lines matching ERE pattern             |           |
 | `-v <pat>` | Skip lines matching ERE pattern                   |           |
 | `-x`       | Remove unmatched lines                            | off       |
 | `-j`       | JSON array input                                  | off       |
 | `-J`       | JSON array output                                 | off       |


### Examples

```sh
# Align first = in each line
colt -d=

# Align ALL = (like vim-easy-align *=)
colt -d= -n'*'

# YAML / dict style  key: value
colt -d: -s -l0 -r1

# CSV-style commas
colt -d, -s -l0 -r1

# Markdown / wiki table
colt -d'|' -n'*' -l1 -r1

# Whitespace table (like column -t)
colt -T

# Right-align the token before =
colt -d= -ar

# Align only non-comment lines
colt -d= -g '^[^#]'

# Shallow indentation normalisation
colt -d= -is
```

Input/output is plain text via stdin/stdout or files - compose freely
with `grep`, `sed`, `awk`, pipes.

## Vim plugin

### Commands

```vim
:[range]Colt [flags]      " align range with given flags
:[range]ColtPrompt        " interactive prompt (tab-completes presets)
:ColtStatus               " show binary path
```

### Default mappings

 | Mode   | Keys                 | Action                                            | 
 | ------ | ------               | --------                                          | 
 | Normal | `<Leader>a` + motion | align lines covered by motion (prompts for flags) | 
 | Normal | `<Leader>a=`         | align current paragraph on `=`                    | 
 | Visual | `<Leader>a`          | align selection (prompts for flags)               | 

Disable defaults and set your own:
```vim
let g:colt_no_mappings = 1

" Then your own:
vmap <silent> <Leader>= :Colt -d= -n'*'<CR>
vmap <silent> <Leader>: :Colt -d: -s -l0 -r1<CR>
vmap <silent> <Leader>, :Colt -d, -s -l0 -r1<CR>
vmap <silent> <Leader>\| :Colt -d'\|' -n'*'<CR>
vmap <silent> <Leader>t :Colt -T<CR>
nmap <silent> <Leader>= vip:Colt -d= -n'*'<CR>
```

### Configuration

```vim
let g:colt_bin           = '/usr/local/bin/colt'   " binary path
let g:colt_default_flags = '-d= -l1 -r1'           " default flags
let g:colt_map_prefix    = '<Leader>a'             " mapping prefix
let g:colt_no_mappings   = 0                       " 1 to disable defaults
```

## Speed

The binary starts in ~1ms and processes 10,000 lines in a few
milliseconds. The Vim plugin simply pipes the visual selection through it.

## Building manually

```sh
gcc -O2 -std=c99 -o colt src/colt.c
```

No dependencies beyond libc and POSIX regex (`<regex.h>`).
Works on Linux, macOS, and any POSIX system.
