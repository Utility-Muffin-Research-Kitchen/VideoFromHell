# Video From Hell MLP1 cross-build. Runs inside the mlp1-toolchain container
# with VideoFromHell, Catastrophe, and Jawaka mounted as sibling repositories.
TC       := /opt/mlp1-toolchain
CC       := $(TC)/bin/aarch64-buildroot-linux-gnu-gcc
SYSROOT  := $(TC)/aarch64-buildroot-linux-gnu/sysroot
VFH      ?= /workspace/VideoFromHell
CATA     ?= /workspace/Catastrophe
CJSON    ?= /workspace/Jawaka/third_party/cjson
SRC      := $(VFH)/cmd/videofromhell
OBJDIR   := $(VFH)/ports/mlp1/build
OUT      := $(VFH)/ports/mlp1/pak/bin/videofromhell
FFMPEG   := $(VFH)/third_party/ffmpeg
MPP      := $(VFH)/third_party/mpp

INCLUDES := -I$(SRC) -I$(FFMPEG)/include -I$(MPP)/include -I$(CATA)/include -I$(CJSON) -I$(SYSROOT)/usr/include -I$(SYSROOT)/usr/include/SDL2
# -DPLATFORM_MLP1 is NOT optional: without it Catastrophe compiles its desktop
# path, which means the generic SDL GameController button mapping instead of the
# MLP1 raw evdev map (physical B arrives as CAT_BTN_A), dev_mode screen sizing,
# and no gamepad wake fd for the idle poll(). Every other MLP1 pak defines it.
PLATFORM_DEFINES := -DPLATFORM_MLP1
APP_CFLAGS := -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter $(PLATFORM_DEFINES) $(INCLUDES)
VENDOR_CFLAGS := -std=c11 -D_DEFAULT_SOURCE -O2 -w $(PLATFORM_DEFINES) $(INCLUDES)
FFMPEG_LIBS := -L$(FFMPEG)/stub -Wl,-rpath,/usr/lib \
	-Wl,-l:libavformat.so.58 -Wl,-l:libavcodec.so.58 -Wl,-l:libavutil.so.56 \
	-Wl,-l:libswresample.so.3 -Wl,-l:libswscale.so.5
PROBE_FFMPEG_LIBS := -L$(FFMPEG)/stub -Wl,-rpath,/usr/lib \
	-Wl,-l:libavformat.so.58 -Wl,-l:libavcodec.so.58 -Wl,-l:libavutil.so.56
MPP_LIBS := -L$(MPP)/stub -Wl,-rpath,/usr/lib -Wl,-l:librockchip_mpp.so.1
LDFLAGS  := -L$(SYSROOT)/usr/lib -lSDL2 -lSDL2_ttf -lSDL2_image -lasound -lm -lpthread $(FFMPEG_LIBS) $(MPP_LIBS)

# The actual decoder remains the vendor runtime: these SONAME stubs record the
# FFmpeg 4.4 ABI at link time and are never packaged into the pak.
OBJ := $(OBJDIR)/main.o $(OBJDIR)/vfh_sources.o $(OBJDIR)/vfh_media.o \
	$(OBJDIR)/vfh_thumb.o $(OBJDIR)/vfh_player.o $(OBJDIR)/vfh_mpp.o \
	$(OBJDIR)/vfh_inhibit.o $(OBJDIR)/vfh_resume.o $(OBJDIR)/vfh_srt.o \
	$(OBJDIR)/cJSON.o
PROBE_OUT := $(VFH)/build/mlp1/vfh-probe
PLAYER_SMOKE_OUT := $(VFH)/build/mlp1/vfh-player-smoke
PROBE_OBJ := $(OBJDIR)/vfh_probe.o
PLAYER_SMOKE_OBJ := $(OBJDIR)/vfh_player_smoke.o

.PHONY: all clean vfh-probe vfh-player-smoke
all: $(OUT)

vfh-probe: $(PROBE_OUT)
vfh-player-smoke: $(PLAYER_SMOKE_OUT)

$(OUT): $(OBJ)
	mkdir -p $(dir $(OUT))
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

$(PROBE_OUT): $(PROBE_OBJ)
	mkdir -p $(dir $(PROBE_OUT))
	$(CC) -o $@ $(PROBE_OBJ) -lm -lpthread $(PROBE_FFMPEG_LIBS) $(MPP_LIBS)

$(PLAYER_SMOKE_OUT): $(PLAYER_SMOKE_OBJ) $(OBJDIR)/vfh_player.o $(OBJDIR)/vfh_mpp.o
	mkdir -p $(dir $(PLAYER_SMOKE_OUT))
	$(CC) -o $@ $^ $(LDFLAGS)

$(OBJDIR)/main.o: $(SRC)/main.c | $(OBJDIR)
	$(CC) $(APP_CFLAGS) -c -o $@ $<

$(OBJDIR)/vfh_sources.o: $(SRC)/vfh_sources.c $(SRC)/vfh_sources.h | $(OBJDIR)
	$(CC) $(APP_CFLAGS) -c -o $@ $<

$(OBJDIR)/vfh_media.o: $(SRC)/vfh_media.c $(SRC)/vfh_media.h | $(OBJDIR)
	$(CC) $(APP_CFLAGS) -c -o $@ $<

$(OBJDIR)/vfh_thumb.o: $(SRC)/vfh_thumb.c $(SRC)/vfh_thumb.h | $(OBJDIR)
	$(CC) $(APP_CFLAGS) -c -o $@ $<

$(OBJDIR)/vfh_inhibit.o: $(SRC)/vfh_inhibit.c $(SRC)/vfh_inhibit.h | $(OBJDIR)
	$(CC) $(APP_CFLAGS) -c -o $@ $<

$(OBJDIR)/vfh_resume.o: $(SRC)/vfh_resume.c $(SRC)/vfh_resume.h | $(OBJDIR)
	$(CC) $(APP_CFLAGS) -c -o $@ $<

$(OBJDIR)/vfh_srt.o: $(SRC)/vfh_srt.c $(SRC)/vfh_srt.h | $(OBJDIR)
	$(CC) $(APP_CFLAGS) -c -o $@ $<

$(OBJDIR)/vfh_player.o: $(SRC)/vfh_player.c $(SRC)/vfh_player.h | $(OBJDIR)
	$(CC) $(APP_CFLAGS) -c -o $@ $<

$(OBJDIR)/vfh_mpp.o: $(SRC)/vfh_mpp.c $(SRC)/vfh_mpp.h $(MPP)/include/vfh_mpp_abi.h | $(OBJDIR)
	$(CC) $(APP_CFLAGS) -c -o $@ $<

$(OBJDIR)/vfh_probe.o: $(SRC)/vfh_probe.c $(MPP)/include/vfh_mpp_abi.h | $(OBJDIR)
	$(CC) $(APP_CFLAGS) -c -o $@ $<

$(OBJDIR)/vfh_player_smoke.o: $(SRC)/vfh_player_smoke.c $(SRC)/vfh_player.h | $(OBJDIR)
	$(CC) $(APP_CFLAGS) -c -o $@ $<

$(OBJDIR)/cJSON.o: $(CJSON)/cJSON.c | $(OBJDIR)
	$(CC) $(VENDOR_CFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR) $(OUT)
