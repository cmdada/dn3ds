#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET			:=	dn3ds
APP_TITLE		:=	Duke Nukem 3D
APP_DESCRIPTION	:=	Duke Nukem 3D for the New 3DS
APP_AUTHOR		:=	cmdada
BUILD			:=	build
SOURCES			:=	source/engine source/game source/audiolib source/n3ds
DATA			:=	data
INCLUDES		:=	source/engine source/game source/audiolib source/n3ds
GRAPHICS		:=	gfx
GFXBUILD		:=	$(BUILD)
#ROMFS			:=	romfs

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

# -std=gnu89 is not nostalgia. This is 1996 C: K&R-style declarations, implicit
# int, tentative definitions, and identifiers that later became keywords. GCC 16
# defaults to gnu23, where implicit-int and implicit-function-declaration are
# hard errors, so the tree simply cannot be compiled in a modern dialect without
# rewriting code we specifically want to keep faithful to the 2003 release.
# -fcommon likewise: the original relies on tentative definitions being merged
# into common symbols, which stopped being the default in GCC 10.
CFLAGS	:=	-g -Wall -O2 -fomit-frame-pointer -mword-relocations \
			-ffunction-sections \
			-std=gnu89 -fcommon \
			$(ARCH)

# Warning noise from 1996 idiom, turned off so that real diagnostics stay
# visible. Each one is a style the original uses pervasively and correctly.
CFLAGS	+=	-Wno-unused-variable -Wno-unused-but-set-variable \
			-Wno-unused-function -Wno-parentheses -Wno-char-subscripts \
			-Wno-misleading-indentation -Wno-sign-compare \
			-Wno-implicit-fallthrough -Wno-maybe-uninitialized

# SDL 1.2's headers install to portlibs/include/SDL, and the tree includes them
# as "SDL.h" (not <SDL/SDL.h>), so that directory has to be on the path in its
# own right. This is what `sdl-config --cflags` adds.
CFLAGS	+=	$(INCLUDE) -I$(PORTLIBS)/include/SDL -D__3DS__ -DPLATFORM_UNIX=1

CXXFLAGS	:=	$(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:=	-lSDL -lcitro3d -lctru -lm -lz

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(CTRULIB)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
SHLISTFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.shlist)))
GFXFILES	:=	$(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

ifeq ($(GFXBUILD),$(BUILD))
export T3XFILES :=  $(GFXFILES:.t3s=.t3x)
else
export ROMFS_T3XFILES	:=	$(patsubst %.t3s, $(GFXBUILD)/%.t3x, $(GFXFILES))
export T3XHFILES		:=	$(patsubst %.t3s, $(BUILD)/%.h, $(GFXFILES))
endif

export OFILES_SOURCES 	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES)) \
			$(PICAFILES:.v.pica=.shbin.o) $(SHLISTFILES:.shlist=.shbin.o) \
			$(addsuffix .o,$(T3XFILES))

export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES	:=	$(PICAFILES:.v.pica=_shbin.h) $(SHLISTFILES:.shlist=_shbin.h) \
			$(addsuffix .h,$(subst .,_,$(BINFILES))) \
			$(GFXFILES:.t3s=.h)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

# Locally patched libSDL (stereo support, tools/patch_sdl.py) first, so it
# wins over the stock portlib.
export LIBPATHS	:=	-L$(CURDIR)/vendor/sdl12-3ds/lib \
			$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS	:=	$(if $(NO_SMDH),,$(OUTPUT).smdh)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else
		ifneq (,$(findstring icon.png,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.png
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

export BANNERTOOL  ?= bannertool
export MAKEROM     ?= makerom

export BANNER_IMAGE := $(TOPDIR)/banner.png
export BANNER_AUDIO := $(TOPDIR)/banner.wav
export BANNER       := $(TOPDIR)/banner.bnr
export APP_RSF      := $(TOPDIR)/app.rsf

.PHONY: all clean cia

#---------------------------------------------------------------------------------
all: $(BUILD) $(GFXBUILD) $(DEPSDIR) $(ROMFS_T3XFILES) $(T3XHFILES)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

cia: $(BUILD) $(GFXBUILD) $(DEPSDIR) $(ROMFS_T3XFILES) $(T3XHFILES)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile cia

$(BUILD):
	@mkdir -p $@

ifneq ($(GFXBUILD),$(BUILD))
$(GFXBUILD):
	@mkdir -p $@
endif

ifneq ($(DEPSDIR),$(BUILD))
$(DEPSDIR):
	@mkdir -p $@
endif

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf $(GFXBUILD) $(TARGET).cia $(BANNER)

#---------------------------------------------------------------------------------
$(GFXBUILD)/%.t3x	$(BUILD)/%.h	:	%.t3s
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@tex3ds -i $< -H $(BUILD)/$*.h -d $(DEPSDIR)/$*.d -o $(GFXBUILD)/$*.t3x

#---------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

$(BANNER): $(BANNER_IMAGE) $(BANNER_AUDIO)
	@echo "building banner ..."
	@$(BANNERTOOL) makebanner -i $(BANNER_IMAGE) -a $(BANNER_AUDIO) -o $@

$(OUTPUT).cia: $(OUTPUT).elf $(_3DSXDEPS) $(BANNER)
	@echo "building cia ..."
	@$(MAKEROM) -f cia -o $@ -target t -elf $(OUTPUT).elf -rsf $(APP_RSF) -icon $(OUTPUT).smdh -banner $(BANNER) -exefslogo

cia: $(OUTPUT).cia

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf	:	$(OFILES)

#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

.PRECIOUS	:	%.t3x %.shbin

%.t3x.o	%_t3x.h :	%.t3x
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

%.shbin.o %_shbin.h : %.shbin
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
