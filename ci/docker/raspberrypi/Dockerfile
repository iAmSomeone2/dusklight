FROM debian:13
LABEL authors="Brenden Davidson <brenden@bdavidson.dev>"

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
          build-essential clang cmake curl git ca-certificates sccache \
          libasound2-dev libclang-dev libcurl4-openssl-dev libdbus-1-dev \
          libdecor-0-dev libegl1-mesa-dev libfreetype-dev libglu1-mesa-dev \
          libncurses5-dev libpng-dev libpulse-dev libpipewire-0.3-dev \
          libssl-dev libudev-dev libunwind-dev libusb-1.0-0-dev libvulkan-dev \
          libsdl3-dev libsdl3-image-dev libsdl3-ttf-dev \
          libwayland-dev libwayland-egl-backend-dev libxkbcommon-dev wayland-protocols \
          lld mold ninja-build python-is-python3 python3 python3-markupsafe zlib1g-dev && \
      rm -rf /var/lib/apt/lists/*

# Add and switch to the "builder" user
#RUN useradd --create-home builder
#USER builder

# Install Rust and ensure the arm64 toolchain is installed
WORKDIR /root
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- --profile minimal --default-toolchain stable -y
ENV PATH="/root/.cargo/bin:${PATH}"
RUN yes | rustup target add aarch64-unknown-linux-gnu

# Set up working directories
ENV RUSTC_WRAPPER=/usr/bin/sccache
ENV SCCACHE_DIR=/workspace/cache
RUN mkdir -p /workspace/{src,build,cache} && chmod -R 1777 /workspace && \
    mkdir $SCCACHE_DIR && chmod 1777 $SCCACHE_DIR

VOLUME $SCCACHE_DIR
VOLUME /workspace/build

COPY --chmod=777 build-with-tracy.sh /usr/local/bin/build-with-tracy.sh

WORKDIR /workspace

ENTRYPOINT ["build-with-tracy.sh"]