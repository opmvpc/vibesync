# Serveur vibesync — multi-stage, image finale minimale non-root (ADR-002)
FROM golang:1.26-alpine AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/vibesync-server ./cmd/vibesync-server

FROM alpine:3.22
RUN adduser -D -H -u 10001 vibesync && apk add --no-cache wget
COPY --from=build /out/vibesync-server /usr/local/bin/vibesync-server
USER vibesync
ENV VIBESYNC_ADDR=:8080
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=3s --retries=3 \
  CMD wget -qO- http://127.0.0.1:8080/healthz || exit 1
ENTRYPOINT ["vibesync-server"]
