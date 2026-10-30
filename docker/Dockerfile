# The scda-nx builder: everything needed to turn your own copy of the game into the Switch NRO, and nothing of the
# game itself. Arch Linux with devkitPro's Switch toolchain, mesa-switch's Vulkan driver (NVK) built in, extract-xiso,
# the recompiler and this repository's sources. The game comes in only when the container runs, from a folder you
# mount, and the NRO is made there, on your computer (docker/entrypoint.sh, build-nro-docker.sh).
FROM archlinux:base-devel
# ties the published image to its repository on GitHub (ghcr.io lists it there and takes its description)
LABEL org.opencontainers.image.source=https://github.com/itouchdennis/scda-nx \
      org.opencontainers.image.description="Builds the scda-nx Switch NRO from your own copy of the game; holds no game data" \
      org.opencontainers.image.licenses=MIT

# host packages: the recompiler's and NVK's build tools
RUN pacman-key --init && pacman-key --populate archlinux && \
    pacman -Syu --noconfirm --needed git cmake ninja meson python python-mako python-yaml python-packaging python-ply \
        flex bison clang llvm libclc spirv-llvm-translator spirv-tools glslang rust rust-aarch64-gnu rust-bindgen \
        cbindgen sdl2-compat rsync && \
    pacman -Scc --noconfirm

# devkitPro's pacman repositories and the Switch toolchain (https://devkitpro.org/wiki/devkitPro_pacman)
RUN pacman-key --recv BC26F752D25B92CE272E0F44F7FD5492264BB9D0 --keyserver hkps://keyserver.ubuntu.com && \
    pacman-key --lsign BC26F752D25B92CE272E0F44F7FD5492264BB9D0 && \
    pacman -U --noconfirm https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.zst && \
    printf '\n[dkp-libs]\nServer = https://pkg.devkitpro.org/packages\n\n[dkp-linux]\nServer = https://pkg.devkitpro.org/packages/linux/$arch/\n' \
        >> /etc/pacman.conf && \
    pacman -Sy --noconfirm --needed switch-dev switch-sdl2 switch-libexpat switch-zlib switch-libzstd && \
    pacman -Scc --noconfirm
ENV DEVKITPRO=/opt/devkitpro

# extract-xiso, for Xbox disc images
RUN git clone --depth 1 https://github.com/XboxDev/extract-xiso.git /tmp/extract-xiso && \
    cmake -S /tmp/extract-xiso -B /tmp/extract-xiso/build -G Ninja -DCMAKE_BUILD_TYPE=Release && \
    ninja -C /tmp/extract-xiso/build && install -m755 /tmp/extract-xiso/build/extract-xiso /usr/local/bin/ && \
    rm -rf /tmp/extract-xiso

# mesa-switch's Vulkan driver at the commit the port was tested with, into /opt/nvk-sdk
ARG MESA_URL=https://github.com/danfromtico/mesa-switch.git
ARG MESA_COMMIT=1a8c1a66d6fd8d65f10107c4627ffc3606ba5631
COPY switch/nvk /opt/scda/switch/nvk
RUN git init -q /tmp/mesa-switch && git -C /tmp/mesa-switch fetch -q --depth 1 "$MESA_URL" "$MESA_COMMIT" && \
    git -C /tmp/mesa-switch checkout -q FETCH_HEAD && \
    MESA=/tmp/mesa-switch OUT=/opt/nvk-sdk /opt/scda/switch/nvk/build-nvk.sh && \
    rm -rf /tmp/mesa-switch /opt/scda/build && \
    test -f /opt/nvk-sdk/opt/devkitpro/portlibs/switch/lib/libvulkan.a

# the shader compilers the renderer builds in, checked out ahead so a build needs no network for them
RUN git clone -q --depth 1 --branch 15.4.0 https://github.com/KhronosGroup/glslang.git /opt/scda/third_party/glslang && \
    git clone -q --depth 1 --branch vulkan-sdk-1.4.321.0 https://github.com/KhronosGroup/SPIRV-Cross.git \
        /opt/scda/third_party/SPIRV-Cross

# this repository, and the recompiler built from it
COPY . /opt/scda
RUN cmake -S /opt/scda -B /opt/scda/build/host-tools -G Ninja -DCMAKE_BUILD_TYPE=Release -DSCDA_GPU=null >/dev/null && \
    ninja -C /opt/scda/build/host-tools xbe_recomp && \
    install -m755 /opt/scda/build/host-tools/xbe_recomp /usr/local/bin/ && rm -rf /opt/scda/build && \
    chmod -R a+rX /opt/scda

# run as your own user (build-nro-docker.sh passes --user): a home anyone can write to
ENV HOME=/tmp
WORKDIR /opt/scda
ENTRYPOINT ["/opt/scda/docker/entrypoint.sh"]
