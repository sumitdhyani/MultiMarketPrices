# ── Multi-stage production image for BinanceMD ──────────────────────────────
#
# Build context must be the project root:
#   docker build -f deploy/BinanceMD/Dockerfile \
#                --build-arg VERSION=$(git describe --tags --match "binance-md/*" --dirty --always) \
#                -t ghcr.io/<owner>/multimarketprices/binance-md:<tag> .
#
# Runtime configuration:
#   Bind-mount config files into /app/config/ and set ENV to select the variant.
#
#   docker run --rm \
#     -v /host/path/to/config:/app/config:ro \
#     -e ENV=prod \
#     ghcr.io/<owner>/multimarketprices/binance-md:v1.0.0  BinanceMD_1
#
# Config variant convention:
#   ENV=prod  → /app/config/config.prod.json   (default)
#   ENV=dev   → /app/config/config.dev.json
#   ENV=test  → /app/config/config.test.json
#
# The entrypoint creates a symlink config/config.json → config/config.<ENV>.json
# so ConfigLib always finds ./config/config.json.

# ── Stage 1: builder ─────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS builder

ARG BOOST_VERSION=1.90.0
ARG RDKAFKA_VERSION=2.10.0

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git wget ca-certificates \
        libssl-dev libsasl2-dev liblz4-dev libzstd-dev libxxhash-dev \
        zlib1g-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Build librdkafka from source so we control the exact version.
RUN wget -q "https://github.com/confluentinc/librdkafka/archive/refs/tags/v${RDKAFKA_VERSION}.tar.gz" \
        -O librdkafka.tar.gz \
    && tar xzf librdkafka.tar.gz \
    && cmake -S librdkafka-${RDKAFKA_VERSION} -B librdkafka-build \
             -DCMAKE_BUILD_TYPE=Release \
             -DRDKAFKA_BUILD_TESTS=OFF \
             -DRDKAFKA_BUILD_EXAMPLES=OFF \
    && cmake --build librdkafka-build --target install -j$(nproc) \
    && ldconfig \
    && rm -rf librdkafka.tar.gz librdkafka-${RDKAFKA_VERSION} librdkafka-build

# Build only the Boost.Json library from Boost source (much faster than all of Boost).
RUN wget -q "https://archives.boost.io/release/${BOOST_VERSION}/source/boost_$(echo ${BOOST_VERSION} | tr . _).tar.gz" \
        -O boost.tar.gz \
    && tar xzf boost.tar.gz \
    && cd boost_$(echo ${BOOST_VERSION} | tr . _) \
    && ./bootstrap.sh --with-libraries=json --prefix=/usr/local \
    && ./b2 -j$(nproc) link=shared install \
    && ldconfig \
    && cd / && rm -rf boost.tar.gz boost_$(echo ${BOOST_VERSION} | tr . _)

# Copy source and build BinanceMD.
# The VERSION build-arg overrides the cmake git-describe fallback.
ARG VERSION=unversioned
WORKDIR /src
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
          -DCOMPONENT_VERSION=${VERSION} \
    && cmake --build build --target BinanceMD -j$(nproc)

# ── Stage 2: runtime ─────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3 libsasl2-2 liblz4-1 libzstd1 libxxhash0 zlib1g libsasl2-modules \
    && rm -rf /var/lib/apt/lists/*

# Copy non-system shared libraries from the builder stage.
COPY --from=builder /usr/local/lib/libboost_json.so.* /usr/local/lib/
COPY --from=builder /usr/local/lib/librdkafka.so.1    /usr/local/lib/
RUN ldconfig

# Copy the binary.
COPY --from=builder /src/build/MDGateways/Binance/BinanceMD /app/BinanceMD

# Copy the entrypoint and prod config.
COPY deploy/BinanceMD/entrypoint.sh /entrypoint.sh
COPY deploy/BinanceMD/config/       /app/config/
RUN chmod +x /entrypoint.sh

WORKDIR /app

ENTRYPOINT ["/entrypoint.sh"]
# Default appId – override on docker run if needed.
CMD ["BinanceMD_1"]
