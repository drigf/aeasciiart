# ASCII Depth - command-line entry points (use from the VS Code terminal / tasks).
#
#   make            build the plug-in (Release, universal) and install it into After Effects
#   make debug      Debug build
#   make xcodeproj  regenerate AsciiDepth.xcodeproj after adding/removing source files
#   make harness    build the command-line test tool (no After Effects needed)
#   make samples    render sample images through the harness into Tools/Harness/out/
#   make clean

CONFIG    ?= Release
AE_SDK_ROOT ?=
DERIVED   := build
PRODUCT   := $(DERIVED)/Build/Products/$(CONFIG)/AsciiDepth.plugin
XCB_ARGS  := -project AsciiDepth.xcodeproj -scheme AsciiDepth -configuration $(CONFIG) -derivedDataPath $(DERIVED)
ifneq ($(AE_SDK_ROOT),)
XCB_ARGS  += AE_SDK_ROOT="$(AE_SDK_ROOT)"
endif

HARNESS   := $(DERIVED)/asciidepth-harness
SDK_FLAGS := -std=c++17 -O2 -fobjc-arc -ISource

.PHONY: all debug xcodeproj plugin install harness samples clean

all: xcodeproj plugin

debug:
	$(MAKE) CONFIG=Debug all

xcodeproj:
	python3 scripts/gen_xcodeproj.py

plugin:
	xcodebuild $(XCB_ARGS) build

install: plugin
	./scripts/install.sh "$(PRODUCT)"

$(HARNESS): Tools/Harness/main.mm Source/Core/AsciiCore.cpp Source/Core/GlyphAtlas.mm Source/Core/SubjectMask.mm Source/Core/AsciiCore.h
	@mkdir -p $(DERIVED)
	clang++ $(SDK_FLAGS) -mmacosx-version-min=13.0 \
	  Tools/Harness/main.mm Source/Core/AsciiCore.cpp Source/Core/GlyphAtlas.mm Source/Core/SubjectMask.mm \
	  -framework Foundation -framework CoreGraphics -framework CoreText -framework CoreVideo \
	  -framework ImageIO -framework UniformTypeIdentifiers -weak_framework Vision -o $@

harness: $(HARNESS)

samples: $(HARNESS)
	python3 Tools/Harness/make_samples.py
	@mkdir -p Tools/Harness/out
	$(HARNESS) Tools/Harness/samples/scene.png Tools/Harness/out/auto.png
	$(HARNESS) Tools/Harness/samples/scene.png Tools/Harness/out/layer.png depth=1 depthmap=Tools/Harness/samples/depth.png
	$(HARNESS) Tools/Harness/samples/scene.png Tools/Harness/out/subject.png depth=2
	$(HARNESS) Tools/Harness/samples/scene.png Tools/Harness/out/transparent.png depth=1 depthmap=Tools/Harness/samples/depth.png bg=1
	$(HARNESS) Tools/Harness/samples/scene.png Tools/Harness/out/deep16.png deep=1

clean:
	rm -rf $(DERIVED) Tools/Harness/out
