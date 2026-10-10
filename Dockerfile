# Builds the XF16Cam XR872 firmware image the same way as
# .github/workflows/build-xf16cam.yml
FROM ubuntu:22.04

ARG ARM_GCC_VERSION=8-2019q3
ARG ARM_GCC_URL=https://armkeil.blob.core.windows.net/developer/Files/downloads/gnu-rm/8-2019q3/RC1.1/gcc-arm-none-eabi-8-2019-q3-update-linux.tar.bz2
ARG BUILD_VARIANT=ptz

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    python3 \
    curl \
    ca-certificates \
    xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Install the Arm GNU Toolchain (matches carlosperate/arm-none-eabi-gcc-action@v1, release 8-2019-q3)
RUN mkdir -p /opt/arm-gnu-toolchain \
    && curl -fsSL "$ARM_GCC_URL" -o /tmp/arm-gcc.tar.bz2 \
    && tar -xjf /tmp/arm-gcc.tar.bz2 -C /opt/arm-gnu-toolchain --strip-components=1 \
    && rm -f /tmp/arm-gcc.tar.bz2

ENV PATH="/opt/arm-gnu-toolchain/bin:${PATH}"

WORKDIR /workspace
COPY --exclude=./dist --exclude=./.git . .

# Test RTSP request parser
RUN cc -std=c11 -Wall -Wextra -Werror \
    -Iinclude -Iproject/example/xf16cam \
    tests/xf16cam/test_rtsp_parser.c \
    project/example/xf16cam/xf16cam_rtsp_parser.c \
    -o /tmp/xf16cam-rtsp-parser-test \
    && /tmp/xf16cam-rtsp-parser-test

# Configure XR872 build
RUN printf '%s\n' \
    '__CONFIG_CHIP_TYPE ?= xr872' \
    '__CONFIG_HOSC_TYPE ?= 40' > .config \
    && chmod +x tools/mkimage

# Apply os_thread.c.patch
RUN patch -p1 < patches/os_thread.c.patch

# Build flash and OTA images. Pass BUILD_VARIANT=no_ptz for the fixed-camera
# board; the default PTZ build leaves NO_PTZ undefined. Optional suffixes, in
# this order: _talk adds XF16CAM_TALK, the RTSP audio backchannel to the
# speaker; _netlog adds XF16CAM_NETLOG, the UDP console mirror (see
# xf16cam_log.c). So ptz, ptz_talk, ptz_netlog, ptz_talk_netlog and the
# no_ptz equivalents.
RUN variant="$BUILD_VARIANT"; symbols=""; \
    case "$variant" in \
    *_netlog) symbols="-DXF16CAM_NETLOG"; variant="${variant%_netlog}" ;; \
    esac; \
    case "$variant" in \
    *_talk) symbols="-DXF16CAM_TALK $symbols"; variant="${variant%_talk}" ;; \
    esac; \
    case "$variant" in \
    ptz) ;; \
    no_ptz) symbols="-DNO_PTZ $symbols" ;; \
    *) echo "Invalid BUILD_VARIANT: $BUILD_VARIANT (use ptz or no_ptz, optionally with _talk and/or _netlog)" >&2; exit 1 ;; \
    esac \
    && make -C project/example/xf16cam/gcc \
    CC_DIR="$(dirname "$(command -v arm-none-eabi-gcc)")" \
    PRJ_EXTRA_SYMBOLS="$symbols" image \
    && make -C project/example/xf16cam/gcc \
    CC_DIR="$(dirname "$(command -v arm-none-eabi-gcc)")" \
    PRJ_EXTRA_SYMBOLS="$symbols" image_xz

# Check critical code placement. The absent sentinels catch an SDK update
# silently linking lwIP DNS/IGMP back in (see xf16cam_lwip_stubs.c). PM must
# stay compiled in on both variants: hal_flashctrl.c keys its SBUS re-init
# workaround (FLASHC_TEMP_FIXED) to CONFIG_PM, and without it the first flash
# write -- an OTA piece or a settings save -- hangs the device.
RUN python3 tools/xf16cam/check_symbol_placement.py \
    --elf project/example/xf16cam/gcc/xf16cam.axf \
    --require-sram xf16cam_http_flash_info \
    --require-sram xf16cam_http_ota \
    --require-sram flashc_suspend \
    --require-xip xf16cam_http_start \
    --require-absent dns_table \
    --require-absent igmp_group_list

# Check 1 MiB flash budget
RUN mkdir -p dist \
    && python3 tools/xf16cam/check_image_budget.py \
    --config project/example/xf16cam/image/xr872/image_auto_cal.cfg \
    --image-dir project/example/xf16cam/image/xr872 \
    | tee dist/image-budget.txt

# Package flash image
RUN version="$(sed -n 's/^#define XF16CAM_VERSION "\([^"]*\)"/\1/p' \
    project/example/xf16cam/xf16cam_version.h | tr -d '\r')" \
    && test -n "$version" \
    && mkdir -p dist \
    && cp project/example/xf16cam/image/xr872/xr_system.img \
    "dist/xf16cam-${BUILD_VARIANT}-xr872-v${version}.img" \
    && cp project/example/xf16cam/image/xr872/xr_system_img_xz.img \
    "dist/xf16cam-${BUILD_VARIANT}-xr872-v${version}-ota.img" \
    && arm-none-eabi-size project/example/xf16cam/gcc/xf16cam.axf \
    | tee dist/size.txt \
    && arm-none-eabi-size -A project/example/xf16cam/gcc/xf16cam.axf \
    > dist/sections.txt \
    && arm-none-eabi-nm -S --size-sort --radix=d \
    project/example/xf16cam/gcc/xf16cam.axf > dist/symbols.txt \
    && gzip -9 -c project/example/xf16cam/gcc/xf16cam.map \
    > dist/xf16cam.map.gz \
    && sha256sum dist/*.img > dist/SHA256SUMS

# Copy the resulting artifacts out with:
#   docker build --build-arg BUILD_VARIANT=ptz -t xf16cam-build . && \
#   docker create --name xf16cam-extract xf16cam-build && \
#   docker cp xf16cam-extract:/workspace/dist ./dist && \
#   docker rm xf16cam-extract
