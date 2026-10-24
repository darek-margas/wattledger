"""Constants for WattLedger."""

from datetime import timedelta

DOMAIN = "wattledger"

DEFAULT_PORT = 80
UPDATE_INTERVAL = timedelta(minutes=5)

# Rows per history API request (the device caps it at 60).
ROWS_PER_REQUEST = 60
# Requests per update, so a long backlog is worked off over a few updates.
MAX_REQUESTS_PER_UPDATE = 100
