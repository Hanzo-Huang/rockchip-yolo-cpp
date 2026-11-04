# Target userspace: Ubuntu 26.04 ARM64. Compiler and tools: native x86.
FROM --platform=linux/amd64 ubuntu:26.04

ENV DEBIAN_FRONTEND=noninteractive

ARG UBUNTU_MIRROR=http://archive.ubuntu.com/ubuntu
ARG UBUNTU_PORTS_MIRROR=http://ports.ubuntu.com/ubuntu-ports

RUN dpkg --add-architecture arm64 \
    && sed -i \
        '/^Types: deb/a Architectures: amd64' \
        /etc/apt/sources.list.d/ubuntu.sources \
    && sed -i \
        -e "s|http://archive.ubuntu.com/ubuntu/|${UBUNTU_MIRROR}/|g" \
        -e "s|http://security.ubuntu.com/ubuntu/|${UBUNTU_MIRROR}/|g" \
        /etc/apt/sources.list.d/ubuntu.sources \
    && printf 'Types: deb\nURIs: %s\nSuites: resolute resolute-updates resolute-security\nComponents: main universe restricted multiverse\nArchitectures: arm64\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n' \
        "$UBUNTU_PORTS_MIRROR" > /etc/apt/sources.list.d/arm64.sources \
    && apt-get -o Acquire::Retries=2 -o Acquire::http::Timeout=30 update \
    && apt-get install -y --no-install-recommends \
        g++-aarch64-linux-gnu \
        cmake \
        make \
        pkg-config \
        git \
        curl \
        ca-certificates \
        xz-utils \
        nodejs \
        npm \
        python3 \
        file \
        binutils-aarch64-linux-gnu \
        libavformat-dev:arm64 \
        libavcodec-dev:arm64 \
        libavutil-dev:arm64 \
        libswscale-dev:arm64 \
        libavdevice-dev:arm64 \
        libdrm-dev:arm64 \
        libjsoncpp-dev:arm64 \
        libyaml-cpp-dev:arm64 \
        libsqlite3-dev:arm64 \
        libarchive-dev:arm64 \
        zlib1g-dev:arm64 \
        uuid-dev:arm64 \
        nlohmann-json3-dev

WORKDIR /project
