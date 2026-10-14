# Agora rating submission API

This dependency-free Python service implements only Agora's public rating submission endpoint:

```text
POST /api/rating
OPTIONS /api/rating
```

It stores accepted submissions in SQLite. It contains no read, listing, aggregation, dashboard, or administration endpoint. Every GET request returns `405 Method Not Allowed`.

## Installation

```sh
sudo useradd --system --home /nonexistent --shell /usr/sbin/nologin agora-rating
sudo install -d -o agora-rating -g agora-rating -m 0750 /var/lib/agora-rating
sudo install -d -o root -g root -m 0755 /opt/agora-rating
sudo install -o root -g root -m 0755 agora-rating-api.py /opt/agora-rating/
sudo install -o root -g root -m 0644 agora-rating.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now agora-rating
```

Copy `nginx-public.location` into the public TLS virtual host, validate the Nginx configuration, then reload Nginx.

## Configuration

| Variable | Default |
| --- | --- |
| `AGORA_RATING_DB` | `/var/lib/agora-rating/ratings.db` |
| `AGORA_RATING_HOST` | `127.0.0.1` |
| `AGORA_RATING_PORT` | `8091` |

No database, submitted record, host identity, domain, certificate, token, or credential is included.
