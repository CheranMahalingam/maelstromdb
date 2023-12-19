FROM ubuntu:22.04 AS builder

# Prevent interactive tool from blocking package installations
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y build-essential cmake python3-pip && \
    rm -rf /var/lib/apt/lists/*

RUN pip3 install conan && conan profile detect

WORKDIR /usr/src
COPY . /usr/src

RUN conan build .

WORKDIR /usr/src/build/Release
