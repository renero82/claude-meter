#!/bin/sh
# Avviato da cron ogni minuto: flock garantisce una sola istanza (riparte dopo crash/riavvio).
mkdir -p "$HOME/.cache/claude-usage-display"
exec flock -n "$HOME/.cache/claude-usage-display/server.lock" \
  python3 "$HOME/claude-usage-display/usage_server.py" >> "$HOME/.cache/claude-usage-display/server.log" 2>&1
