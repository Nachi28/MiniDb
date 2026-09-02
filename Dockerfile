FROM gcc:14 AS build
RUN apt-get update && apt-get install -y --no-install-recommends cmake ninja-build \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMINIDB_WARNINGS_AS_ERRORS=ON \
    && cmake --build build \
    && ctest --test-dir build --output-on-failure

FROM debian:bookworm-slim
COPY --from=build /src/build/minidb /usr/local/bin/minidb
WORKDIR /data
EXPOSE 5433
ENTRYPOINT ["minidb"]
CMD ["--db", "/data/minidb.db", "--serve", "5433"]
