# kbus-manager: web management for the kbus-gateway (port 8080 in the container, optional login).
# On the controller: published on 127.0.0.1:8090 and reached at http(s)://<pfc-ip>/kbus
# through the controller's lighttpd (tools/deploy-docker.sh --manager).
#
#   docker buildx build --platform linux/arm64 -t kbus-manager:arm64 --load docker/manager
#
#   docker run -d --name kbus-manager --restart unless-stopped -p 8080:8080 \
#       -v /home/kbus-manager:/data -v /run/kbus-gateway:/run/kbus-gateway kbus-manager:arm64
#
# Login: off by default; switched on with user + password on the Settings tab,
# stored in /data/settings.json (mount /data to keep it). Optional: MGMT_READONLY=1.
FROM python:3.12-alpine
WORKDIR /app
COPY manager.py manager.html ./
ENV PYTHONUNBUFFERED=1 MGMT_PORT=8080
EXPOSE 8080
CMD ["python", "manager.py"]
