# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Pass the pinned gtk= value from Scripts/linux-images.txt as BASE_IMAGE.
ARG BASE_IMAGE
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive
ENV UBQ_BUILD_ROOT=/tmp/ubq-sync-transport-dependencies
ENV UBQ_BUILD_JOBS=4 GOMAXPROCS=4
RUN apt-get update -qq && apt-get install -y -qq \
        cmake ninja-build g++ pkg-config git golang-go perl python3 \
        openssl openssh-client openssh-server libwebp-dev \
    && rm -rf /var/lib/apt/lists/*

# This account exists only in the image, never in the developer's host OS.
# /bin/sh has no per-user startup file for an OpenSSH forced command.
RUN useradd --create-home --home-dir /home/ubq-transport --shell /bin/sh \
        --password '*' ubq-transport \
    && mkdir -p /run/sshd \
    && printf 'sync-transport-v1\n' > /etc/ubiquitty-transport-fixture \
    && chmod 0644 /etc/ubiquitty-transport-fixture

WORKDIR /src
CMD ["/bin/sh", "-ec", "cmake -S /src -B /tmp/ubq-sync-transport-build -G Ninja -DUBIQUITTY_FRONTEND=Host -DUBIQUITTY_QUIC=ON -DCMAKE_BUILD_TYPE=Debug -DUBIQUITTY_BUILD_SYNC_TRANSPORT_ACCEPTANCE=ON -DUBIQUITTY_BUILD_TESTS=OFF -DUBIQUITTY_BUILD_SSH_PACKAGE=OFF -DCMAKE_COMPILE_WARNING_AS_ERROR=ON && cmake --build /tmp/ubq-sync-transport-build --parallel 4 --target sync_transport_acceptance && python3 Scripts/check-sync-transport.py --binary /tmp/ubq-sync-transport-build/sync_transport_acceptance"]
