FROM gcr.io/oss-fuzz-base/base-builder:v1
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake ninja-build \
    && rm -rf /var/lib/apt/lists/*
COPY . $SRC/shibadb-c
WORKDIR $SRC/shibadb-c
COPY .clusterfuzzlite/build.sh $SRC/
