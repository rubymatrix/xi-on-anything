# A Linux build and test box for the POSIX host: clang, SDL3 (built from its release, with Vulkan,
# X11, Wayland, PipeWire/PulseAudio/ALSA), and Mesa's lavapipe for offscreen Vulkan tests.
#   docker build -t xi-linux tools/linux
#   docker run --rm -v "$PWD":/src -w /src xi-linux python3 tools/build_posix.py gfxtest
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl clang lld make cmake ninja-build pkg-config python3 python3-pip git \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev \
    libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev \
    libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev libibus-1.0-dev \
    libvulkan-dev mesa-vulkan-drivers vulkan-tools vulkan-validationlayers glslang-tools glslang-dev spirv-tools \
    libsecret-1-dev xvfb \
 && rm -rf /var/lib/apt/lists/*
RUN pip3 install --break-system-packages capstone pefile
ARG SDL=3.4.16
RUN curl -fsSL https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL3-$SDL.tar.gz | tar xz -C /tmp \
 && cmake -S /tmp/SDL3-$SDL -B /tmp/sdlb -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
      -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_VULKAN=ON \
 && cmake --build /tmp/sdlb && cmake --install /tmp/sdlb && rm -rf /tmp/SDL3-$SDL /tmp/sdlb && ldconfig
