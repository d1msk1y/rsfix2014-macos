# Builds rs_dyldfix.dylib (x86_64 only: Rocksmith 2014 for Mac is an Intel binary run via Rosetta).
CC      ?= clang
CFLAGS  ?= -O2 -Wall
ARCH     = -arch x86_64 -mmacosx-version-min=10.13
FRAMEWORKS = -framework CoreAudio -framework CoreFoundation
SRC      = src/dyldfix.c src/audiofix.c
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

rs_dyldfix.dylib: $(SRC)
	$(CC) $(ARCH) $(CFLAGS) -dynamiclib $(SRC) $(FRAMEWORKS) -o $@

# Shareable zip for people without Xcode tools: dist/RocksmithFix-<version>.zip
dist: rs_dyldfix.dylib
	rm -rf dist && mkdir -p dist/RocksmithFix
	cp launch rs_dyldfix.dylib README.md dist/RocksmithFix/
	cd dist && ditto -c -k --keepParent RocksmithFix RocksmithFix-$(VERSION).zip
	@echo "built dist/RocksmithFix-$(VERSION).zip"

clean:
	rm -rf rs_dyldfix.dylib dist dyldfix.log

.PHONY: dist clean
