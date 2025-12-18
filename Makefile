# PMP091 - A ZERO Player with Xvid MPEG-4 + libmad MP3 Support
# SF2000 Libretro Makefile
# Much simpler than FFmpeg version!

STATIC_LINKING := 1
AR             := ar

ifeq ($(platform),)
platform = unix
ifeq ($(shell uname -a),)
   platform = win
else ifneq ($(findstring MINGW,$(shell uname -a)),)
   platform = win
else ifneq ($(findstring Darwin,$(shell uname -a)),)
   platform = osx
endif
endif

TARGET_NAME := pmp
LIBM        := -lm

# =============================================================
# SF2000 Platform
# =============================================================
ifeq ($(platform), sf2000)
   TARGET := $(TARGET_NAME)_libretro_$(platform).a
   MIPS := /tmp/mips32-mti-elf/2019.09-03-2/bin/mips-mti-elf-
   CC = $(MIPS)gcc
   CXX = $(MIPS)g++
   AR = $(MIPS)ar

   # MIPS32 flags - soft-float, no FPU
   CFLAGS = -EL -march=mips32 -mtune=mips32 -msoft-float -ffast-math -fomit-frame-pointer
   CFLAGS += -G0 -mno-abicalls -fno-pic
   CFLAGS += -ffunction-sections -fdata-sections

   # Include paths
   CFLAGS += -I. -Ixvid -Ixvid/bitstream -Ixvid/dct -Ixvid/image
   CFLAGS += -Ixvid/motion -Ixvid/prediction -Ixvid/quant -Ixvid/utils
   CFLAGS += -Ilibmad

   # Defines for SF2000
   CFLAGS += -DSF2000
   # libmad: FPM_DEFAULT is hardcoded in froggyMP3's fixed.h

   STATIC_LINKING = 1

# =============================================================
# Unix (for testing compilation)
# =============================================================
else ifeq ($(platform), unix)
   TARGET := $(TARGET_NAME)_libretro.so
   fpic := -fPIC
   SHARED := -shared -Wl,--no-undefined
   CFLAGS += -I. -Ixvid -Ixvid/bitstream -Ixvid/dct -Ixvid/image
   CFLAGS += -Ixvid/motion -Ixvid/prediction -Ixvid/quant -Ixvid/utils

# =============================================================
# Windows
# =============================================================
else
   TARGET := $(TARGET_NAME)_libretro.dll
   CC = gcc
   SHARED := -shared -static-libgcc -s -Wl,--no-undefined
   CFLAGS += -I. -Ixvid
endif

# Optimization
ifeq ($(DEBUG), 1)
   CFLAGS += -O0 -g -DDEBUG
else
   CFLAGS += -O2
endif

CFLAGS += -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable

# =============================================================
# Xvid decoder source files (decoder only, no encoder!)
# =============================================================
OBJS_XVID = \
	xvid/xvid.o \
	xvid/decoder.o \
	xvid/bitstream/bitstream.o \
	xvid/bitstream/cbp.o \
	xvid/bitstream/mbcoding.o \
	xvid/dct/idct.o \
	xvid/dct/simple_idct.o \
	xvid/image/colorspace.o \
	xvid/image/image.o \
	xvid/image/interpolate8x8.o \
	xvid/image/postprocessing.o \
	xvid/image/qpel.o \
	xvid/image/reduced.o \
	xvid/image/font.o \
	xvid/motion/gmc.o \
	xvid/motion/motion_comp.o \
	xvid/motion/sad.o \
	xvid/prediction/mbprediction.o \
	xvid/quant/quant_h263.o \
	xvid/quant/quant_matrix.o \
	xvid/quant/quant_mpeg.o \
	xvid/utils/emms.o \
	xvid/utils/mem_align.o \
	xvid/utils/mem_transfer.o

# =============================================================
# TJpgDec for MJPEG
# =============================================================
OBJS_TJPGD = tjpgd.o

# =============================================================
# libmad MP3 decoder (fixed-point, MIPS optimized)
# =============================================================
OBJS_LIBMAD = \
	libmad/bit.o \
	libmad/fixed.o \
	libmad/frame.o \
	libmad/huffman.o \
	libmad/layer12.o \
	libmad/layer3.o \
	libmad/libmad.o \
	libmad/stream.o \
	libmad/synth.o \
	libmad/timer.o

# =============================================================
# Main libretro wrapper
# =============================================================
OBJS_MAIN = libretro-pmp.o

# =============================================================
# All objects
# =============================================================
OBJS = $(OBJS_MAIN) $(OBJS_TJPGD) $(OBJS_XVID) $(OBJS_LIBMAD)

# =============================================================
# Build rules
# =============================================================

all: $(TARGET)

$(TARGET): $(OBJS)
ifeq ($(STATIC_LINKING), 1)
	$(AR) rcs $@ $(OBJS)
else
	$(CC) $(fpic) $(SHARED) -o $@ $(OBJS) $(LDFLAGS) $(LIBM)
endif

%.o: %.c
	$(CC) $(CFLAGS) $(fpic) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
	find . -name "*.o" -type f -delete 2>/dev/null || true

.PHONY: clean all
