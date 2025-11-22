from flask import Flask, request, jsonify, abort, render_template_string, redirect, url_for
from datetime import datetime
from typing import Dict, Any, List
import json
from pathlib import Path
import threading

app = Flask(__name__)

# ---------------------------------------------------------
# JSON "database" files
# ---------------------------------------------------------
BASE_DIR = Path(__file__).parent
MEDS_FILE = BASE_DIR / "devices_meds.json"
PENDING_FILE = BASE_DIR / "devices_commands_pending.json"
HISTORY_FILE = BASE_DIR / "devices_commands_history.json"
META_FILE = BASE_DIR / "devices_meta.json"

# In-memory mirrors (loaded from JSON at startup)
devices_meds: Dict[str, Dict[str, Any]] = {}
devices_commands_pending: Dict[str, List[Dict[str, Any]]] = {}
devices_commands_history: Dict[str, List[Dict[str, Any]]] = {}
devices_meta: Dict[str, Dict[str, Any]] = {}

lock = threading.Lock()


# ---------------------------------------------------------
# Time helpers
# ---------------------------------------------------------
def now_iso():
    """Return current time in UTC ISO-8601 format."""
    return datetime.utcnow().isoformat()


def human(ts: str):
    """
    Convert ISO-8601 timestamp string to a human readable form.
    Returns '-' for None/empty, or original string if parse fails.
    """
    if not ts:
        return "-"
    try:
        dt = datetime.fromisoformat(ts)
        # Example: "18 Nov 2025 • 09:28:49 PM"
        return dt.strftime("%d %b %Y • %I:%M:%S %p")
    except Exception:
        # If something weird is stored, show raw
        return ts


# Make human() available inside templates
app.jinja_env.globals.update(human=human)


# ---------------------------------------------------------
# JSON helpers
# ---------------------------------------------------------
def load_json_file(path: Path, default):
    if not path.exists():
        return default
    try:
        with path.open("r", encoding="utf-8") as f:
            return json.load(f)
    except Exception as e:
        print(f"Error loading {path.name}: {e}")
        return default


def save_json_file(path: Path, data):
    try:
        with path.open("w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
    except Exception as e:
        print(f"Error saving {path.name}: {e}")


def load_state():
    global devices_meds, devices_commands_pending, devices_commands_history, devices_meta
    with lock:
        devices_meds = load_json_file(MEDS_FILE, {})
        devices_commands_pending = load_json_file(PENDING_FILE, {})
        devices_commands_history = load_json_file(HISTORY_FILE, {})
        devices_meta = load_json_file(META_FILE, {})
    print("Loaded JSON DB:")
    print(f"  devices_meds: {len(devices_meds)} devices")
    print(f"  devices_commands_pending: {len(devices_commands_pending)} devices")
    print(f"  devices_commands_history: {len(devices_commands_history)} devices")
    print(f"  devices_meta: {len(devices_meta)} devices")


def save_all_state():
    with lock:
        save_json_file(MEDS_FILE, devices_meds)
        save_json_file(PENDING_FILE, devices_commands_pending)
        save_json_file(HISTORY_FILE, devices_commands_history)
        save_json_file(META_FILE, devices_meta)


def ensure_device_meta(device_id: str):
    """Create meta entry if it doesn't exist."""
    with lock:
        if device_id not in devices_meta:
            devices_meta[device_id] = {
                "deviceId": device_id,
                "friendly_name": device_id,
                "created_at": now_iso(),
                "last_seen_upload": None,
                "last_seen_changes": None,
            }
            save_json_file(META_FILE, devices_meta)


def update_last_seen_upload(device_id: str):
    ensure_device_meta(device_id)
    with lock:
        devices_meta[device_id]["last_seen_upload"] = now_iso()
        save_json_file(META_FILE, devices_meta)


def update_last_seen_changes(device_id: str):
    ensure_device_meta(device_id)
    with lock:
        devices_meta[device_id]["last_seen_changes"] = now_iso()
        save_json_file(META_FILE, devices_meta)


def enqueue_command(device_id: str, cmd: Dict[str, Any]):
    """Add a command to the per-device pending queue and persist."""
    ensure_device_meta(device_id)
    with lock:
        if device_id not in devices_commands_pending:
            devices_commands_pending[device_id] = []
        devices_commands_pending[device_id].append(cmd)
        save_json_file(PENDING_FILE, devices_commands_pending)


# ---------------------------------------------------------
# ESP32 API ENDPOINTS
# ---------------------------------------------------------

# 1) POST /medbox/upload  (ESP32 -> Cloud)
@app.post("/medbox/upload")
def upload_medbox():
    data = request.get_json()
    if not data:
        return jsonify({"error": "invalid json"}), 400

    device_id = data.get("deviceId")
    count = data.get("count")
    meds = data.get("meds")

    if not device_id or meds is None or count is None:
        return jsonify({"error": "missing fields"}), 400

    snapshot = {
        "timestamp": now_iso(),
        "count": count,
        "meds": meds,
    }

    ensure_device_meta(device_id)
    update_last_seen_upload(device_id)

    with lock:
        devices_meds[device_id] = snapshot
        save_json_file(MEDS_FILE, devices_meds)

    print(f"[UPLOAD] from {device_id}: {count} meds")

    return jsonify({
        "status": "ok",
        "deviceId": device_id,
        "receivedCount": count,
        "storedAt": snapshot["timestamp"],
    })


# 2) GET /medbox/changes?deviceId=...  (ESP32 polls commands)
@app.get("/medbox/changes")
def get_changes():
    device_id = request.args.get("deviceId")
    if not device_id:
        return jsonify({"error": "deviceId required"}), 400

    ensure_device_meta(device_id)
    update_last_seen_changes(device_id)

    with lock:
        cmds = devices_commands_pending.get(device_id, [])
        # Move commands to history as "sent"
        if device_id not in devices_commands_history:
            devices_commands_history[device_id] = []

        sent_time = now_iso()
        for c in cmds:
            history_entry = dict(c)
            history_entry["status"] = "sent"
            history_entry["sent_at"] = sent_time
            devices_commands_history[device_id].append(history_entry)

        # Clear pending after sending
        devices_commands_pending[device_id] = []
        save_json_file(PENDING_FILE, devices_commands_pending)
        save_json_file(HISTORY_FILE, devices_commands_history)

    print(f"[CHANGES] for {device_id}: {len(cmds)} commands")

    return jsonify({"commands": cmds})


# Helper endpoints to enqueue commands (from dashboard / mobile app)

@app.post("/medbox/<device_id>/command/add")
def add_command(device_id):
    data = request.get_json()
    if not data:
        return jsonify({"error": "invalid json"}), 400

    required = ["name", "qty", "hour", "minute", "led"]
    if any(k not in data for k in required):
        return jsonify({"error": "missing fields"}), 400

    cmd = {
        "op": "add",
        "name": data["name"],
        "qty": int(data["qty"]),
        "hour": int(data["hour"]),
        "minute": int(data["minute"]),
        "led": int(data["led"]),
        "enabled": bool(data.get("enabled", True)),
    }
    enqueue_command(device_id, cmd)
    return jsonify({"status": "queued", "deviceId": device_id, "command": cmd})


@app.post("/medbox/<device_id>/command/edit")
def edit_command(device_id):
    data = request.get_json()
    if not data:
        return jsonify({"error": "invalid json"}), 400

    if "id" not in data:
        return jsonify({"error": "id required"}), 400

    cmd = {"op": "edit", "id": int(data["id"])}

    for field in ["name", "qty", "hour", "minute", "led", "enabled"]:
        if field in data:
            cmd[field] = data[field]

    enqueue_command(device_id, cmd)
    return jsonify({"status": "queued", "deviceId": device_id, "command": cmd})


@app.post("/medbox/<device_id>/command/delete")
def delete_command(device_id):
    data = request.get_json()
    if not data:
        return jsonify({"error": "invalid json"}), 400

    if "id" not in data:
        return jsonify({"error": "id required"}), 400

    cmd = {"op": "delete", "id": int(data["id"])}
    enqueue_command(device_id, cmd)
    return jsonify({"status": "queued", "deviceId": device_id, "command": cmd})


# Debug endpoints

@app.get("/medbox/<device_id>/snapshot")
def get_snapshot(device_id):
    with lock:
        snap = devices_meds.get(device_id)
    if not snap:
        abort(404, description="No data for this device yet")
    return jsonify(snap)


@app.get("/medbox/<device_id>/pending")
def get_pending(device_id):
    with lock:
        pending = devices_commands_pending.get(device_id, [])
    return jsonify({
        "deviceId": device_id,
        "pending": pending,
    })


# ---------------------------------------------------------
# DASHBOARD (HTML)
# ---------------------------------------------------------

INDEX_TEMPLATE = """
<!doctype html>
<html>
<head>
  <title>MedBox Dashboard</title>
  <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css" rel="stylesheet">
</head>
<body class="bg-light">
<div class="container mt-4">
  <div class="d-flex justify-content-between align-items-center">
    <h2> MedBox Dashboard</h2>
    <a class="btn btn-primary" href="{{ url_for('new_device') }}">+ New Device</a>
  </div>
  <hr>
  {% if devices %}
  <table class="table table-striped table-bordered align-middle">
    <thead class="table-dark">
      <tr>
        <th>#</th>
        <th>Friendly Name</th>
        <th>Device ID</th>
        <th>Last Upload</th>
        <th>Last Changes</th>
        <th>Meds Count</th>
        <th>Pending Requests</th>
        <th>Sent Requests</th>
        <th>Actions</th>
      </tr>
    </thead>
    <tbody>
    {% for d in devices %}
      <tr>
        <td>{{ loop.index }}</td>
        <td>{{ d.friendly_name }}</td>
        <td><code>{{ d.deviceId }}</code></td>
        <td>{{ human(d.last_seen_upload) }}</td>
        <td>{{ human(d.last_seen_changes) }}</td>
        <td>{{ d.meds_count }}</td>
        <td>{{ d.pending_count }}</td>
        <td>{{ d.sent_count }}</td>
        <td>
          <a class="btn btn-sm btn-outline-primary" href="{{ url_for('device_detail', device_id=d.deviceId) }}">View</a>
        </td>
      </tr>
    {% endfor %}
    </tbody>
  </table>
  {% else %}
    <div class="alert alert-info">
      No devices yet. Create one with "New Device" or let an ESP32 upload for the first time.
    </div>
  {% endif %}
</div>
</body>
</html>
"""


DEVICE_TEMPLATE = """
<!doctype html>
<html>
<head>
  <title>Device {{ meta.friendly_name }} - MedBox</title>
  <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css" rel="stylesheet">
</head>
<body class="bg-light">
<div class="container mt-4">
  <div class="d-flex justify-content-between align-items-center mb-3">
    <a href="{{ url_for('index') }}" class="btn btn-secondary">&laquo; Back</a>

    <form method="POST" action="{{ url_for('delete_device', device_id=meta.deviceId) }}"
          onsubmit="return confirm('Delete this device and all history? This cannot be undone.');">
      <button class="btn btn-danger">Delete Device</button>
    </form>
  </div>

  <h3>Device: {{ meta.friendly_name }}</h3>
  <p>
    <b>Device ID:</b> <code>{{ meta.deviceId }}</code><br>
    <b>Created at:</b> {{ human(meta.created_at) }}<br>
    <b>Last Upload:</b> {{ human(meta.last_seen_upload) }}<br>
    <b>Last Changes Poll:</b> {{ human(meta.last_seen_changes) }}<br>
  </p>

  <hr>
  <h4>Current Medicines</h4>
  {% if snapshot %}
    <p><b>Snapshot time:</b> {{ human(snapshot.timestamp) }} | <b>Count:</b> {{ snapshot.count }}</p>
    {% if snapshot.meds %}
      <table class="table table-sm table-striped table-bordered">
        <thead class="table-light">
          <tr>
            <th>ID</th><th>Name</th><th>Qty</th><th>Time (24h)</th><th>LED</th><th>Enabled</th>
          </tr>
        </thead>
        <tbody>
        {% for m in snapshot.meds %}
          <tr>
            <td>{{ m.id }}</td>
            <td>{{ m.name }}</td>
            <td>{{ m.qty }}</td>
            <td>{{ "%02d" % m.hour }}:{{ "%02d" % m.minute }}</td>
            <td>{{ m.led }}</td>
            <td>{{ "Yes" if m.enabled else "No" }}</td>
          </tr>
        {% endfor %}
        </tbody>
      </table>
    {% else %}
      <div class="alert alert-warning">No medicines in last snapshot.</div>
    {% endif %}
  {% else %}
    <div class="alert alert-info">No snapshot received yet from this device.</div>
  {% endif %}

  <hr>
  <h4>Pending Requests (not yet delivered to ESP32)</h4>
  {% if pending %}
    <table class="table table-sm table-striped table-bordered">
      <thead class="table-light">
        <tr><th>#</th><th>Op</th><th>Data</th><th>Actions</th></tr>
      </thead>
      <tbody>
      {% for c in pending %}
        <tr>
          <td>{{ loop.index }}</td>
          <td>{{ c.op }}</td>
          <td><pre style="margin:0;font-size:0.8rem;">{{ c|tojson(indent=2) }}</pre></td>
          <td>
            <form method="POST"
                  action="{{ url_for('delete_pending', device_id=meta.deviceId, idx=loop.index0) }}"
                  onsubmit="return confirm('Delete this pending command?');">
              <button class="btn btn-sm btn-outline-danger">Delete</button>
            </form>
          </td>
        </tr>
      {% endfor %}
      </tbody>
    </table>
  {% else %}
    <div class="alert alert-success">No pending requests.</div>
  {% endif %}

  <hr>
  <h4>Sent Requests (history)</h4>
  {% if history %}
    <table class="table table-sm table-striped table-bordered">
      <thead class="table-light">
        <tr><th>#</th><th>Op</th><th>Status</th><th>Sent at</th><th>Data</th><th>Actions</th></tr>
      </thead>
      <tbody>
      {% for c in history %}
        <tr>
          <td>{{ loop.index }}</td>
          <td>{{ c.op }}</td>
          <td>{{ c.status }}</td>
          <td>{{ human(c.sent_at) }}</td>
          <td><pre style="margin:0;font-size:0.8rem;">{{ c|tojson(indent=2) }}</pre></td>
          <td>
            <form method="POST"
                  action="{{ url_for('delete_history', device_id=meta.deviceId, idx=loop.index0) }}"
                  onsubmit="return confirm('Delete this history record?');">
              <button class="btn btn-sm btn-outline-danger">Delete</button>
            </form>
          </td>
        </tr>
      {% endfor %}
      </tbody>
    </table>
  {% else %}
    <div class="alert alert-info">No sent requests yet.</div>
  {% endif %}
</div>
</body>
</html>
"""


NEW_DEVICE_TEMPLATE = """
<!doctype html>
<html>
<head>
  <title>New Device - MedBox</title>
  <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css" rel="stylesheet">
</head>
<body class="bg-light">
<div class="container mt-4">
  <a href="{{ url_for('index') }}" class="btn btn-secondary mb-3">&laquo; Back</a>
  <h3>Create New Device</h3>
  <div class="card">
    <div class="card-body">
      <form method="POST">
        <div class="mb-3">
          <label class="form-label">Device ID (must match ESP32 deviceId)</label>
          <input type="text" class="form-control" name="deviceId" required placeholder="MEDBOX_XXXXXXXXXXXX">
        </div>
        <div class="mb-3">
          <label class="form-label">Friendly Name</label>
          <input type="text" class="form-control" name="friendly_name" placeholder="Grandpa MedBox">
        </div>
        <button class="btn btn-primary">Create</button>
      </form>
    </div>
  </div>
</div>
</body>
</html>
"""


# ---------------------------------------------------------
# Dashboard routes
# ---------------------------------------------------------

@app.get("/")
def index():
    with lock:
        device_ids = set(devices_meta.keys()) | set(devices_meds.keys()) | \
                     set(devices_commands_pending.keys()) | set(devices_commands_history.keys())

        rows = []
        for dev_id in sorted(device_ids):
            meta = devices_meta.get(dev_id, {
                "deviceId": dev_id,
                "friendly_name": dev_id,
                "created_at": None,
                "last_seen_upload": None,
                "last_seen_changes": None,
            })
            meds_snap = devices_meds.get(dev_id, None)
            pending = devices_commands_pending.get(dev_id, [])
            history = devices_commands_history.get(dev_id, [])
            rows.append(type("DevRow", (), {
                "deviceId": dev_id,
                "friendly_name": meta.get("friendly_name", dev_id),
                "last_seen_upload": meta.get("last_seen_upload"),
                "last_seen_changes": meta.get("last_seen_changes"),
                "meds_count": meds_snap["count"] if meds_snap else 0,
                "pending_count": len(pending),
                "sent_count": len(history),
            }))

    return render_template_string(INDEX_TEMPLATE, devices=rows)


@app.route("/devices/new", methods=["GET", "POST"])
def new_device():
    if request.method == "POST":
        device_id = request.form.get("deviceId", "").strip()
        friendly_name = request.form.get("friendly_name", "").strip()
        if not device_id:
            return "Device ID required", 400

        if not friendly_name:
            friendly_name = device_id

        ensure_device_meta(device_id)
        with lock:
            devices_meta[device_id]["friendly_name"] = friendly_name
            save_json_file(META_FILE, devices_meta)

        return redirect(url_for("device_detail", device_id=device_id))

    return render_template_string(NEW_DEVICE_TEMPLATE)


@app.get("/device/<device_id>")
def device_detail(device_id):
    ensure_device_meta(device_id)
    with lock:
        meta = devices_meta.get(device_id)
        snap = devices_meds.get(device_id)
        pending = devices_commands_pending.get(device_id, [])
        history = devices_commands_history.get(device_id, [])

    snapshot = None    #
    if snap:
        snapshot = type("Snap", (), {
            "timestamp": snap.get("timestamp"),
            "count": snap.get("count", 0),
            "meds": snap.get("meds", []),
        })

    return render_template_string(
        DEVICE_TEMPLATE,
        meta=meta,
        snapshot=snapshot,
        pending=pending,
        history=history
    )


# ---------------------------------------------------------
# Delete device (ALL data)
# ---------------------------------------------------------
@app.post("/device/<device_id>/delete")
def delete_device(device_id):
    """
    Delete everything related to a device:
    - meta
    - last meds snapshot
    - pending commands
    - history
    """
    with lock:
        devices_meta.pop(device_id, None)
        devices_meds.pop(device_id, None)
        devices_commands_pending.pop(device_id, None)
        devices_commands_history.pop(device_id, None)

        save_json_file(META_FILE, devices_meta)
        save_json_file(MEDS_FILE, devices_meds)
        save_json_file(PENDING_FILE, devices_commands_pending)
        save_json_file(HISTORY_FILE, devices_commands_history)

    print(f"[DELETE] Removed device {device_id} and all related data.")
    return redirect(url_for("index"))


# ---------------------------------------------------------
# Delete single pending command
# ---------------------------------------------------------
@app.post("/device/<device_id>/pending/<int:idx>/delete")
def delete_pending(device_id, idx):
    with lock:
        lst = devices_commands_pending.get(device_id, [])
        if 0 <= idx < len(lst):
            del lst[idx]
            devices_commands_pending[device_id] = lst
            save_json_file(PENDING_FILE, devices_commands_pending)
    print(f"[DELETE_PENDING] device={device_id}, idx={idx}")
    return redirect(url_for("device_detail", device_id=device_id))


# ---------------------------------------------------------
# Delete single history record
# ---------------------------------------------------------
@app.post("/device/<device_id>/history/<int:idx>/delete")
def delete_history(device_id, idx):
    with lock:
        lst = devices_commands_history.get(device_id, [])
        if 0 <= idx < len(lst):
            del lst[idx]
            devices_commands_history[device_id] = lst
            save_json_file(HISTORY_FILE, devices_commands_history)
    print(f"[DELETE_HISTORY] device={device_id}, idx={idx}")
    return redirect(url_for("device_detail", device_id=device_id))


# ---------------------------------------------------------
# Startup
# ---------------------------------------------------------

if __name__ == "__main__":
    load_state()
    # Run on Raspberry Pi / server so ESP32 can reach it over LAN
    app.run(host="0.0.0.0", port=8000, debug=True)
