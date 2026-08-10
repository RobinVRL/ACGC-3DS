TARGET      := acgc_3ds
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    := include ../../include ../../src ../../pc/include
TOOLCHAIN_INCLUDES := $(DEVKITPRO)/libctru/include
LIBDIRS     := $(DEVKITPRO)/libctru/lib

APP_TITLE   := Animal Crossing GC
APP_AUTHOR  := ACGC-PC-Port contributors
APP_PRODUCT_CODE := CTR-P-ACGC
APP_UNIQUE_ID    := 0xAC6C0

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -g -Wall -O2 -mword-relocations -ffunction-sections \
          -fdata-sections $(ARCH) -DTARGET_3DS -DVERSION=0 \
          -DF3DEX_GBI_2 -DNDEBUG -D_LANGUAGE_C
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS := -g $(ARCH)
LDFLAGS = -specs=3dsx.specs -g $(ARCH) $(foreach dir,$(LIBDIRS),-L$(dir)) -Wl,-Map,$(notdir $*.map)
LIBS := -lcitro3d -lctru -lm

MAKEROM      ?= makerom
SMDHTOOL     ?= smdhtool
BANNERTOOL   ?= bannertool
CIA_RSF      := acgc_3ds_extended.rsf
CIA_ICON     := $(BUILD)/$(TARGET).smdh
CIA_BANNER   := $(BUILD)/$(TARGET).bnr
CIA_OUTPUT   := $(TARGET)_extended.cia
APP_ICON     := icon_48.png
APP_ICON_SMALL := icon_24.png
BANNER_IMAGE := banner.png
BANNER_AUDIO := banner.wav

ifneq ($(strip $(DEVKITPRO)),)
include $(DEVKITPRO)/devkitARM/3ds_rules
else
$(error DEVKITPRO is not set. Install devkitPro and run this from its shell)
endif

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
RUNTIME_ONLY_CFILES := acgc_3ds_pc_disc.c acgc_3ds_pc_compat.c acgc_3ds_gx.c
CFILES := $(filter-out $(RUNTIME_ONLY_CFILES),$(CFILES))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
OFILES := $(CFILES:.c=.o) $(CPPFILES:.cpp=.o) acgc_3ds_vshader.shbin.o
RUNTIME_OFILE := runtime-full-make/acgc-runtime-combined.o
RUNTIME_OBJECT := build/$(RUNTIME_OFILE)
OFILES += $(RUNTIME_OFILE)

export OUTPUT := $(CURDIR)/$(TARGET)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(dir)) \
                  $(foreach dir,$(TOOLCHAIN_INCLUDES),-I$(dir)) -I$(BUILD)

.PHONY: all cia clean runtime-smoke runtime-full FORCE

all: $(BUILD) $(TARGET).3dsx

# Homebrew Launcher fixes Old 3DS applications to the normal memory layout.
# The CIA exheader selects the 80 MiB (Dev2) layout before process launch.
cia: $(CIA_OUTPUT)

$(CIA_OUTPUT): $(BUILD)/$(TARGET).elf $(CIA_ICON) $(CIA_BANNER) $(CIA_RSF)
	$(MAKEROM) -f cia -o $@ -target t -elf $< -rsf $(CIA_RSF) \
		-icon $(CIA_ICON) -banner $(CIA_BANNER) -exefslogo

$(CIA_ICON): $(APP_ICON) $(APP_ICON_SMALL) | $(BUILD)
	$(SMDHTOOL) --create "$(APP_TITLE)" "Native GameCube port" \
		"$(APP_AUTHOR)" $(APP_ICON) $@ $(APP_ICON_SMALL)

$(CIA_BANNER): $(BANNER_IMAGE) $(BANNER_AUDIO) | $(BUILD)
	$(BANNERTOOL) makebanner -i $(BANNER_IMAGE) -a $(BANNER_AUDIO) -o $@

runtime-smoke:
	$(MAKE) -f Makefile.runtime runtime-smoke

runtime-full:
	cmake -S runtime -B build/runtime-full-make -G "Unix Makefiles" \
		-DCMAKE_TOOLCHAIN_FILE=$(DEVKITPRO)/cmake/3DS.cmake
	cmake --build build/runtime-full-make -j4
	sed -i 's|"$(CURDIR)/|"|g' build/runtime-full-make/runtime-objects.rsp
	$(DEVKITARM)/bin/arm-none-eabi-ld -r -o $(RUNTIME_OBJECT) \
		@build/runtime-full-make/runtime-objects.rsp

$(RUNTIME_OBJECT): FORCE
	$(MAKE) runtime-full

FORCE:

$(BUILD):
	@mkdir -p $@

$(TARGET).3dsx: $(BUILD)/$(TARGET).elf $(CIA_ICON)
	3dsxtool $< $@ --smdh=$(CIA_ICON)

$(BUILD)/$(TARGET).elf: $(addprefix $(BUILD)/,$(OFILES))
	$(CXX) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD)/%.o: $(SOURCES)/%.c
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

$(BUILD)/%.o: $(SOURCES)/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -o $@

$(BUILD)/acgc_3ds_video.o: $(BUILD)/acgc_3ds_vshader.shbin.o

$(BUILD)/acgc_3ds_vshader.shbin: $(SOURCES)/acgc_3ds_vshader.v.pica
	picasso -o $@ $<

$(BUILD)/acgc_3ds_vshader.shbin.o: $(BUILD)/acgc_3ds_vshader.shbin
	bin2s -a 4 -H $(BUILD)/acgc_3ds_vshader_shbin.h $< | \
		$(CC) -x assembler-with-cpp $(ASFLAGS) -c -o $@ -

clean:
	@rm -rf $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf $(CIA_OUTPUT)

-include $(BUILD)/*.d
