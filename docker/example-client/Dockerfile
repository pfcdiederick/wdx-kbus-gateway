# Example container that uses the kbus-gateway through its Unix socket.
# Build context: the docker/ folder (for the shared lib/wdx_client.py). From the project root:
#
#   docker buildx build --platform linux/arm64 -f docker/example-client/Dockerfile \
#       -t kbus-example-client:arm64 --load docker
#
#   docker run -d --name kbus-example-client --restart unless-stopped \
#       --network none -v /run/kbus-gateway:/run/kbus-gateway kbus-example-client:arm64
#
# No network is needed (--network none): the socket is the only connection.
FROM python:3.12-alpine
WORKDIR /app
COPY lib/wdx_client.py example-client/example.py ./
ENV PYTHONUNBUFFERED=1
CMD ["python", "example.py"]
