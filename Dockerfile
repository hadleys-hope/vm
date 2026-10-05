# hope-runtime image: build with the full toolchain, ship only the binaries.
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake make && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src/ src/
COPY tests/ tests/
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ctest --test-dir build --output-on-failure

FROM debian:bookworm-slim
COPY --from=build /src/build/hope-runtime /src/build/hopevm /src/build/bus-bench /usr/local/bin/
ENTRYPOINT ["hope-runtime"]
