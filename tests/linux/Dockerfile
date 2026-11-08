FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y -qq \
    python3 python3-venv python3-tk git iproute2 can-utils build-essential \
    ca-certificates xvfb xauth && rm -rf /var/lib/apt/lists/*
WORKDIR /tmp/can-demo
