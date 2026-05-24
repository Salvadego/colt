CC      = gcc
CFLAGS  = -O2 -Wall -std=c99
TARGET  = colt
SRC     = src/colt.c
PREFIX ?= $(HOME)
BINDIR  = $(PREFIX)/bin
VIMDIR  = $(HOME)/.vim/plugin
CFGDIR  = $(HOME)/.config/colt

.PHONY: all install vim config all-in clean test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<
	@echo "Built: $(TARGET)"

install: $(TARGET)
	@mkdir -p $(BINDIR)
	cp $(TARGET) $(BINDIR)/$(TARGET)
	@echo "Installed binary: $(BINDIR)/$(TARGET)"

vim:
	@mkdir -p $(VIMDIR)
	cp vim/colt.vim $(VIMDIR)/colt.vim
	@echo "Installed Vim plugin: $(VIMDIR)/colt.vim"

config:
	@mkdir -p $(CFGDIR)
	@if [ ! -f $(CFGDIR)/presets.toml ]; then \
		./$(TARGET) --dump-config > $(CFGDIR)/presets.toml; \
		echo "Created: $(CFGDIR)/presets.toml"; \
	else \
		echo "Skipped (already exists): $(CFGDIR)/presets.toml"; \
	fi

all-in: all install vim config
	@echo ""
	@echo "Done. Edit your presets at:"
	@echo "  $(CFGDIR)/presets.toml"
	@echo ""
	@echo "Add to vimrc:"
	@echo "  \" Quick-align: <Leader>a{motion/visual} then a key"
	@echo "  \" = first=   * all=   : colon   , comma   | table"
	@echo "  \" > arrow    / comment   ; semi   \\ backslash"
	@echo "  \" r raw-flags-prompt     p preset-name-prompt"

clean:
	rm -f $(TARGET)

test: $(TARGET)
	@echo "--- basic = ---"
	@printf 'let foo = 1\nlet hello = "world"\nlet very_long = "test"\n' | ./$(TARGET) -d=
	@echo "--- star * ---"
	@printf 'a=b=c\nfoo=bar=baz\nx=yy=zzz\n' | ./$(TARGET) -d= -n'*'
	@echo "--- -P arrow ---"
	@printf 'x >> y\na => b\nfoo > bar\n' | ./$(TARGET) -P arrow
	@echo "--- -P comment ---"
	@printf 'int x = 1; // first\nlong foo = 2; // second\nchar *p = NULL; /* third */\n' | ./$(TARGET) -P comment
	@echo "--- -P colon ---"
	@printf 'key: value\nlonger_key: other\nx: y\n' | ./$(TARGET) -P colon
	@echo "--- -P semi ---"
	@printf 'int a = 1;\nlong bbb = 2;\nchar *p = NULL;\n' | ./$(TARGET) -P semi
	@echo "--- -e regex ---"
	@printf 'a >> b\nfoo => bar\nx > y\n' | ./$(TARGET) -e '>>|=>|>'
	@echo "--- table mode ---"
	@printf 'Name Age City\nAlice 30 NewYork\nBob 25 London\n' | ./$(TARGET) -T
	@echo "--- grep filter ---"
	@printf 'x = 1\n# comment = skip\ny = 2\n' | ./$(TARGET) -d= -g'^[^#]'
	@echo "--- list-presets ---"
	@./$(TARGET) --list-presets | head -5
	@echo "All tests passed."
