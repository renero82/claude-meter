#!/usr/bin/env python3
"""Claude usage display - server per la macchina dove gira Claude Code.

Ogni POLL_S secondi legge l'utilizzo del piano Claude (limite 5 ore e
settimanale) e lo espone in LAN come JSON su http://<vm>:8790/usage.json
per la scheda ESP32 (WT32-SC01 Plus); su / la stessa grafica come pagina web
(per kiosk e browser).

Fonte dei limiti: endpoint NON documentato https://api.anthropic.com/api/oauth/usage
(lo stesso che usano /usage di Claude Code e la scheda "utilizzo" dell'app).
Autenticazione: token OAuth di Claude Code in ~/.claude/.credentials.json.
Il token non viene mai rinnovato da qui: se sta per scadere si lancia
`claude auth status`, che lo rinnova con la logica di Claude Code.
Il token non esce mai dalla VM: nel JSON servito ci sono solo percentuali e orari.

In piu': token consumati oggi da Claude Code su questa VM, sommati dai
transcript locali ~/.claude/projects/**/*.jsonl (campo message.usage, formato
osservato e non documentato).
"""
import glob
import json
import os
import subprocess
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from zoneinfo import ZoneInfo

PORT = int(os.environ.get("CUD_PORT", "8790"))
POLL_S = 300            # 5 minuti: l'endpoint risponde 429 se interrogato troppo spesso
BACKOFF_S = 900         # dopo un errore 429
CRED = os.path.expanduser("~/.claude/.credentials.json")
CLAUDE = os.path.expanduser("~/.local/bin/claude")
PROJECTS = os.path.expanduser("~/.claude/projects")
PAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "page.html")
TZ = ZoneInfo("Europe/Rome")
GIORNI = ["Lun", "Mar", "Mer", "Gio", "Ven", "Sab", "Dom"]

state = {"ok": False, "error": "avvio in corso", "fetched_at": None}
lock = threading.Lock()


def log(msg):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), msg, flush=True)


def access_token():
    with open(CRED) as f:
        o = json.load(f)["claudeAiOauth"]
    if o.get("expiresAt", 0) / 1000 - time.time() < 1800:
        log("token in scadenza: claude auth status")
        subprocess.run([CLAUDE, "auth", "status"], capture_output=True, timeout=60)
        with open(CRED) as f:
            o = json.load(f)["claudeAiOauth"]
    return o["accessToken"], o.get("subscriptionType", "")


def fetch_usage():
    tok, plan = access_token()
    req = urllib.request.Request(
        "https://api.anthropic.com/api/oauth/usage",
        headers={"Authorization": "Bearer " + tok,
                 "anthropic-beta": "oauth-2025-04-20",
                 "Accept": "application/json",
                 "User-Agent": "claude-usage-display/1.0"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.load(r), plan


def fmt_reset(iso):
    if not iso:
        return None, None
    dt = datetime.fromisoformat(iso).astimezone(TZ)
    now = datetime.now(TZ)
    secs = int((dt - now).total_seconds())
    dt = (dt + timedelta(seconds=30)).replace(second=0, microsecond=0)  # 06:59:59 -> 07:00
    if dt.date() == now.date():
        label = dt.strftime("%H:%M")
    else:
        label = f"{GIORNI[dt.weekday()]} {dt.strftime('%H:%M')}"
    return label, max(secs, 0)


def window(raw):
    if not raw or raw.get("utilization") is None:
        return None
    used = round(float(raw["utilization"]))
    label, secs = fmt_reset(raw.get("resets_at"))
    return {"used": used, "left": max(0, 100 - used),
            "resets_at": raw.get("resets_at"), "reset_label": label,
            "reset_in_s": secs}


def tokens_today():
    """Token di Claude Code su questa VM, oggi (ora italiana), dedup per message.id."""
    start = datetime.now(TZ).replace(hour=0, minute=0, second=0, microsecond=0)
    start_ts = start.timestamp()
    seen = {}
    for path in glob.glob(PROJECTS + "/**/*.jsonl", recursive=True):
        try:
            if os.path.getmtime(path) < start_ts:
                continue
            with open(path, errors="replace") as f:
                for line in f:
                    if '"usage"' not in line:
                        continue
                    try:
                        e = json.loads(line)
                    except ValueError:
                        continue
                    m = e.get("message") or {}
                    u = m.get("usage")
                    ts = e.get("timestamp")
                    if not u or not ts:
                        continue
                    if datetime.fromisoformat(ts.replace("Z", "+00:00")).timestamp() < start_ts:
                        continue
                    key = (m.get("id"), e.get("requestId")) if m.get("id") else (path, ts)
                    seen[key] = u  # l'ultima riga di un messaggio ha i totali finali
        except OSError:
            continue
    t = {"input": 0, "output": 0, "cache_write": 0, "cache_read": 0, "messages": len(seen)}
    for u in seen.values():
        t["input"] += u.get("input_tokens") or 0
        t["output"] += u.get("output_tokens") or 0
        t["cache_write"] += u.get("cache_creation_input_tokens") or 0
        t["cache_read"] += u.get("cache_read_input_tokens") or 0
    return t


def poll_loop():
    global state
    while True:
        delay = POLL_S
        try:
            raw, plan = fetch_usage()
            new = {
                "ok": True, "error": None,
                "plan": (plan or "").capitalize(),
                "five_hour": window(raw.get("five_hour")),
                "seven_day": window(raw.get("seven_day")),
                "extra_usage_enabled": bool((raw.get("extra_usage") or {}).get("is_enabled")),
                "source": "api.anthropic.com/api/oauth/usage (non documentato)",
            }
            log(f"ok 5h={new['five_hour'] and new['five_hour']['used']}% "
                f"7g={new['seven_day'] and new['seven_day']['used']}%")
        except urllib.error.HTTPError as e:
            new = dict(state, ok=False, error=f"HTTP {e.code}")
            if e.code == 429:
                delay = BACKOFF_S
            log(f"errore HTTP {e.code}")
        except Exception as e:  # rete, file credenziali, ecc.
            new = dict(state, ok=False, error=type(e).__name__)
            log(f"errore {e!r}")
        try:
            new["tokens_today"] = tokens_today()
        except Exception as e:
            log(f"errore transcript {e!r}")
        now = datetime.now(TZ)
        if new.get("ok"):
            new["fetched_at"] = now.isoformat(timespec="seconds")
            new["fetched_label"] = now.strftime("%H:%M")
        with lock:
            state = new
        time.sleep(delay)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/", "/index.html"):
            try:
                with open(PAGE, "rb") as f:
                    page = f.read()
            except OSError:
                self.send_error(500)
                return
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(page)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(page)
            return
        if path != "/usage.json":
            self.send_error(404)
            return
        with lock:
            body = dict(state)
        # ricalcola i secondi al reset a ogni richiesta
        for k in ("five_hour", "seven_day"):
            w = body.get(k)
            if w and w.get("resets_at"):
                w = dict(w)
                w["reset_label"], w["reset_in_s"] = fmt_reset(w["resets_at"])
                body[k] = w
        body["now"] = datetime.now(TZ).strftime("%H:%M")
        if body.get("fetched_at"):
            age = time.time() - datetime.fromisoformat(body["fetched_at"]).timestamp()
            body["age_s"] = int(age)
        data = json.dumps(body, ensure_ascii=True).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        pass


if __name__ == "__main__":
    threading.Thread(target=poll_loop, daemon=True).start()
    log(f"in ascolto su :{PORT}")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
