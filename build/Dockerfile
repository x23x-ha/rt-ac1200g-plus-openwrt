# Build env for the OpenWrt 25.12.6 SDK (x86_64 binaries -> run with --platform linux/amd64)
FROM debian:12
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential gawk gettext git libncurses-dev libssl-dev zlib1g-dev \
      python3 python3-setuptools rsync unzip file wget curl ca-certificates \
      zstd xz-utils bzip2 patch diffutils flex bison swig quilt time \
    && rm -rf /var/lib/apt/lists/*
RUN useradd -m -u 1000 build && mkdir /work && chown build:build /work
USER build
WORKDIR /work
