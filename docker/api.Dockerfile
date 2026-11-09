FROM debian:trixie-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ make libpq-dev ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /source
COPY Makefile ./
COPY include/ include/
COPY lib/ lib/
COPY src/ src/
# The Makefile inventories these trees even when building just the API.
RUN mkdir -p test/exsim test/framework test/lob test/benchmark && make -j1 WITH_POSTGRES=1 EXTRA_CXXFLAGS=-Werror build/release/exsim_apid build/release/exsim_exchanged build/release/exsim_traderd

FROM debian:trixie-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends libpq5 ca-certificates curl && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 exsim && useradd --uid 10001 --gid 10001 --no-create-home exsim \
    && mkdir -p /var/lib/exsim && chown exsim:exsim /var/lib/exsim
COPY --from=build /source/build/release/exsim_apid /source/build/release/exsim_exchanged /source/build/release/exsim_traderd /usr/local/bin/
COPY --chmod=755 docker/api-entrypoint.sh /usr/local/bin/api-entrypoint
USER exsim
WORKDIR /var/lib/exsim
EXPOSE 7600
ENTRYPOINT ["api-entrypoint"]
CMD ["--bind", "0.0.0.0", "--port", "7600", "--data-dir", "/var/lib/exsim"]
