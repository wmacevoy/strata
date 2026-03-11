# Stage 1: Build
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config \
    libsqlite3-dev libzmq3-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Build strata
WORKDIR /build
COPY . .
RUN mkdir -p build && cd build && \
    cmake .. && \
    make -j$(nproc)

# Stage 2: Runtime
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libsqlite3-0 libzmq5 libssl3 tini \
    && rm -rf /var/lib/apt/lists/*

# Strata binaries
COPY --from=builder /build/build/strata-homestead /usr/local/bin/
COPY --from=builder /build/build/strata           /usr/local/bin/
COPY --from=builder /build/build/strata-den       /usr/local/bin/

# Data volume
RUN mkdir -p /var/strata
VOLUME /var/strata

EXPOSE 6000

ENV STRATA_DB_PATH=/var/strata/strata.db
ENV STRATA_STORE_ENDPOINT=tcp://127.0.0.1:5560
ENV STRATA_VILLAGE_ENDPOINT=tcp://0.0.0.0:6000

ENTRYPOINT ["tini", "--", "strata-homestead"]
