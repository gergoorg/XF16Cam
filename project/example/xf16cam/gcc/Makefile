#
# Rules for building application
#

# ----------------------------------------------------------------------------
# project local config
# ----------------------------------------------------------------------------
include localconfig.mk

# ----------------------------------------------------------------------------
# common rules
# ----------------------------------------------------------------------------
ROOT_PATH := ../../../..

include $(ROOT_PATH)/gcc.mk

# Git Bash reports itself as Msys, but GNU make for Windows can evaluate the
# tool selection before the requested shell is active. Keep local OTA builds
# on the native packer; Linux continues to use the extensionless binary.
ifeq ($(OS),Windows_NT)
override MKIMAGE := mkimage.exe
endif

# ----------------------------------------------------------------------------
# project name and project root path
# ----------------------------------------------------------------------------
PROJECT := $(notdir $(shell cd .. && pwd))
PRJ_ROOT_PATH := $(ROOT_PATH)/project/example/$(PROJECT)

# ----------------------------------------------------------------------------
# board path, including board_config.c and board_config.h
# ----------------------------------------------------------------------------
# XF16CAM_TALK builds (RTSP audio backchannel to the speaker) use the
# project's copy of the board config, whose only change is that the internal
# codec has no speaker-amplifier pin: the SDK board drives PA23 for that, and
# on the XF16 PA23 is the camera/SD power rail. Every other build keeps the
# SDK board file untouched.
ifeq ($(__CONFIG_CHIP_TYPE), xr872)
ifneq (,$(findstring XF16CAM_TALK,$(PRJ_EXTRA_SYMBOLS)))
PRJ_BOARD := $(PRJ_ROOT_PATH)/board
else
PRJ_BOARD := $(ROOT_PATH)/project/common/board/xr872_evb_ai
endif
else
PRJ_BOARD := null
endif

# ----------------------------------------------------------------------------
# objects
# ----------------------------------------------------------------------------
INCLUDE_PATHS += -I$(PRJ_ROOT_PATH)

DIRS_IGNORE := $(PRJ_ROOT_PATH)/gcc%
DIRS_IGNORE += $(PRJ_ROOT_PATH)/image%
DIRS_IGNORE += $(PRJ_ROOT_PATH)/board%
DIRS_IGNORE += $(ROOT_PATH)/project/common/board/%
DIRS_IGNORE += $(PRJ_BOARD)

DIRS_ALL := $(shell find $(PRJ_ROOT_PATH) $(ROOT_PATH)/project/common -type d)
DIRS := $(filter-out $(DIRS_IGNORE),$(DIRS_ALL))
DIRS += $(PRJ_BOARD)

SRCS := $(basename $(foreach dir,$(DIRS),$(wildcard $(dir)/*.[csS])))

# XF16Cam owns camera capture and sensor dispatch. Do not link the SDK camera
# console demo, its one-shot capture paths, or its native sensor dependency.
SRCS := $(filter-out $(ROOT_PATH)/project/common/cmd/cmd_camera,$(SRCS))

OBJS := $(addsuffix .o,$(SRCS))

# extra libraries searching path
# PRJ_EXTRA_LIBS_PATH :=

# extra libraries
# PRJ_EXTRA_LIBS :=

# extra header files searching path
# PRJ_EXTRA_INC_PATH :=

# extra symbols (macros)
# PRJ_EXTRA_SYMBOLS :=

# ----------------------------------------------------------------------------
# override project variables
# ----------------------------------------------------------------------------
# linker script file
#   - relative to "./"
#   - define your own "LINKER_SCRIPT" to override the default one
# LINKER_SCRIPT :=

# image config file
#   - relative to "../image/$(__CONFIG_CHIP_TYPE)/", eg. "../image/xr872/"
#   - define your own "IMAGE_CFG" to override the default one
# IMAGE_CFG :=
IMAGE_CFG := image_auto_cal.cfg
IMAGE_XZ_CFG := image_img_xz.cfg

# image name, default to xr_system
# IMAGE_NAME :=

# project make rules
include $(PRJ_MAKE_RULES)
