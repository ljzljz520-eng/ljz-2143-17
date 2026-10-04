FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential \
       make \
       libsdl2-dev \
       libsdl2-image-dev \
       xvfb \
       x11vnc \
       openbox \
       novnc \
       websockify \
       x11-utils \
       ca-certificates \
       python3 \
       curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY . /app
RUN chmod +x /app/docker/entrypoint.sh && make && make replay_tool
RUN cd /app && make test

EXPOSE 6080 8080

CMD ["/app/docker/entrypoint.sh"]
