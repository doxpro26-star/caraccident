"""
ResQCar — ESP32 Crash Response Network
Professional Flask server:
- Hospitals register with geo-location
- ESP32 car robot POSTs tilt / impact / SOS to /api/alert
- Server notifies nearby hospitals (haversine radius)
"""
import sqlite3
import math
import json
import os
import secrets
from datetime import datetime
from zoneinfo import ZoneInfo
from flask import Flask, request, jsonify, redirect, url_for, render_template, flash, session
from werkzeug.security import generate_password_hash, check_password_hash

IST = ZoneInfo("Asia/Kolkata")

def now_ist(fmt="%Y-%m-%d %H:%M:%S"):
    return datetime.now(IST).strftime(fmt)

app = Flask(__name__)
app.secret_key = os.environ.get("SECRET_KEY", secrets.token_hex(16))
DB = os.path.join(os.path.dirname(__file__), "hospital.db")
RADIUS_KM = float(os.environ.get("ALERT_RADIUS_KM", "5"))  # accidents within 5 km flag the hospital

# ---------- DB ----------
def db():
    c = sqlite3.connect(DB)
    c.row_factory = sqlite3.Row
    return c

def init_db():
    c = db()
    c.execute("""CREATE TABLE IF NOT EXISTS hospitals(
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT NOT NULL, phone TEXT DEFAULT '', address TEXT DEFAULT '',
        lat REAL NOT NULL, lon REAL NOT NULL, created TEXT NOT NULL,
        password_hash TEXT DEFAULT '')""")
    # migration for DBs created before login existed
    try:
        c.execute("ALTER TABLE hospitals ADD COLUMN password_hash TEXT DEFAULT ''")
    except sqlite3.OperationalError:
        pass
    c.execute("""CREATE TABLE IF NOT EXISTS alerts(
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        device_id TEXT NOT NULL, reason TEXT NOT NULL,
        lat REAL NOT NULL, lon REAL NOT NULL,
        roll REAL DEFAULT 0, pitch REAL DEFAULT 0,
        time TEXT NOT NULL, nearby TEXT DEFAULT '[]')""")
    c.commit(); c.close()

init_db()

# ---------- geo ----------
def haversine(lat1, lon1, lat2, lon2):
    R = 6371.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    d = math.radians(lat2 - lat1)
    l = math.radians(lon2 - lon1)
    a = math.sin(d / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(l / 2) ** 2
    return 2 * R * math.asin(math.sqrt(a))

def nearby_hospitals(lat, lon, radius=RADIUS_KM):
    c = db()
    rows = c.execute("SELECT * FROM hospitals").fetchall()
    c.close()
    out = []
    for h in rows:
        d = haversine(lat, lon, h["lat"], h["lon"])
        if d <= radius:
            h = dict(h)
            h["distance_km"] = round(d, 2)
            out.append(h)
    return sorted(out, key=lambda x: x["distance_km"])

def get_stats():
    c = db()
    nh = c.execute("SELECT COUNT(*) n FROM hospitals").fetchone()["n"]
    na = c.execute("SELECT COUNT(*) n FROM alerts").fetchone()["n"]
    today = now_ist("%Y-%m-%d")
    nt = c.execute("SELECT COUNT(*) n FROM alerts WHERE time LIKE ?", (today + "%",)).fetchone()["n"]
    last = c.execute("SELECT * FROM alerts ORDER BY id DESC LIMIT 1").fetchone()
    c.close()
    return {"hospitals": nh, "alerts": na, "today": nt,
            "last": dict(last) if last else None, "radius": RADIUS_KM}

@app.context_processor
def inject_globals():
    ch = None
    hid = session.get("hospital_id")
    if hid:
        c = db()
        row = c.execute("SELECT id, name, phone FROM hospitals WHERE id=?", (hid,)).fetchone()
        c.close()
        if row:
            ch = dict(row)
        else:
            session.pop("hospital_id", None)
    return {"RADIUS": RADIUS_KM, "now": datetime.now(IST), "current_hospital": ch}

def current_hospital_full():
    hid = session.get("hospital_id")
    if not hid:
        return None
    c = db()
    row = c.execute("SELECT * FROM hospitals WHERE id=?", (hid,)).fetchone()
    c.close()
    return dict(row) if row else None

def prioritize(alerts):
    """GPS-fix alerts first (newest first), then location-unknown ones."""
    return sorted(alerts, key=lambda a: (a["lat"] == 0 and a["lon"] == 0, -a["id"]))

def alerts_near(hospital, limit=50):
    c = db()
    rows = [dict(a) for a in c.execute("SELECT * FROM alerts ORDER BY id DESC LIMIT ?", (limit,)).fetchall()]
    c.close()
    mine = []
    for a in rows:
        if a["lat"] == 0 and a["lon"] == 0:
            a["dist"] = None  # broadcast: no GPS fix, location unknown
        else:
            d = haversine(a["lat"], a["lon"], hospital["lat"], hospital["lon"])
            if d > RADIUS_KM:
                continue
            a["dist"] = round(d, 2)
        try:
            a["nearby_list"] = json.loads(a["nearby"] or "[]")
        except json.JSONDecodeError:
            a["nearby_list"] = []
        mine.append(a)
    return prioritize(mine)

# ---------- pages ----------
@app.route("/")
def index():
    c = db()
    alerts = [dict(a) for a in c.execute("SELECT * FROM alerts ORDER BY id DESC LIMIT 30").fetchall()]
    hospitals = [dict(h) for h in c.execute("SELECT * FROM hospitals ORDER BY id DESC LIMIT 10").fetchall()]
    c.close()
    for a in alerts:
        try:
            a["nearby_list"] = json.loads(a["nearby"] or "[]")
        except json.JSONDecodeError:
            a["nearby_list"] = []
    return render_template("index.html", alerts=prioritize(alerts), hospitals=hospitals, stats=get_stats())

@app.route("/register", methods=["GET", "POST"])
def register():
    if request.method == "POST":
        name = request.form.get("name", "").strip()
        phone = request.form.get("phone", "").strip()
        address = request.form.get("address", "").strip()
        email = request.form.get("email", "").strip()
        pw = request.form.get("password", "")
        pw2 = request.form.get("password2", "")
        try:
            lat = float(request.form.get("lat", ""))
            lon = float(request.form.get("lon", ""))
        except (ValueError, TypeError):
            flash("Invalid latitude / longitude.", "error")
            return render_template("register.html"), 400
        if not (-90 <= lat <= 90 and -180 <= lon <= 180):
            flash("Coordinates out of range.", "error")
            return render_template("register.html"), 400
        if not name:
            flash("Hospital name is required.", "error")
            return render_template("register.html"), 400
        if not phone:
            flash("Phone is required — it is your login ID.", "error")
            return render_template("register.html"), 400
        if len(pw) < 4:
            flash("Password must be at least 4 characters.", "error")
            return render_template("register.html"), 400
        if pw != pw2:
            flash("Passwords do not match.", "error")
            return render_template("register.html"), 400
        c = db()
        if c.execute("SELECT id FROM hospitals WHERE phone=?", (phone,)).fetchone():
            c.close()
            flash("This phone is already registered. Please log in.", "error")
            return redirect(url_for("login"))
        cur = c.execute(
            "INSERT INTO hospitals(name,phone,address,lat,lon,created,password_hash) VALUES(?,?,?,?,?,?,?)",
            (name, phone, f"{address} {email}".strip(),
             lat, lon, now_ist(),
             generate_password_hash(pw)))
        hid = cur.lastrowid
        c.commit(); c.close()
        session["hospital_id"] = hid
        flash(f"{name} registered — welcome to your dashboard.", "success")
        return redirect(url_for("dashboard"))
    return render_template("register.html")

@app.route("/login", methods=["GET", "POST"])
def login():
    if session.get("hospital_id"):
        return redirect(url_for("dashboard"))
    if request.method == "POST":
        phone = request.form.get("phone", "").strip()
        pw = request.form.get("password", "")
        c = db()
        row = c.execute("SELECT * FROM hospitals WHERE phone=?", (phone,)).fetchone()
        c.close()
        if row and row["password_hash"] and check_password_hash(row["password_hash"], pw):
            session["hospital_id"] = row["id"]
            flash(f"Welcome back, {row['name']}.", "success")
            return redirect(url_for("dashboard"))
        flash("Wrong phone number or password.", "error")
        return render_template("login.html"), 401
    return render_template("login.html")

@app.route("/logout")
def logout():
    session.pop("hospital_id", None)
    flash("Logged out.", "success")
    return redirect(url_for("index"))

@app.route("/dashboard")
def dashboard():
    h = current_hospital_full()
    if not h:
        flash("Please log in to open your dashboard.", "error")
        return redirect(url_for("login"))
    near = alerts_near(h)
    return render_template("dashboard.html", hospital=h, alerts=near,
                           stats=get_stats(), nearby_count=len(near))

@app.route("/hospitals")
def hospitals():
    c = db()
    rows = [dict(r) for r in c.execute(
        "SELECT id, name, phone, address, lat, lon, created FROM hospitals ORDER BY id DESC").fetchall()]
    c.close()
    if request.args.get("format") == "json":
        return jsonify(rows)
    return render_template("hospitals.html", hospitals=rows, stats=get_stats())

@app.route("/hospital/<int:hid>")
def hospital_detail(hid):
    c = db()
    h = c.execute("SELECT id, name, phone, address, lat, lon, created FROM hospitals WHERE id=?", (hid,)).fetchone()
    c.close()
    if not h:
        return render_template("404.html"), 404
    h = dict(h)
    return render_template("hospital_detail.html", hospital=h, alerts=alerts_near(h))

# ---------- API (ESP32 calls this — do not rename) ----------
@app.route("/api/alert", methods=["POST"])
def api_alert():
    data = request.get_json(force=True, silent=True) or {}
    device_id = str(data.get("device_id", "UNKNOWN"))
    reason = str(data.get("reason", "EMERGENCY"))
    try:
        lat = float(data.get("lat", 0)); lon = float(data.get("lon", 0))
    except (ValueError, TypeError):
        return jsonify({"ok": False, "error": "invalid lat/lon"}), 400
    roll = float(data.get("roll", 0) or 0)
    pitch = float(data.get("pitch", 0) or 0)
    if lat == 0 and lon == 0:
        # No GPS fix: broadcast to ALL hospitals, location marked unknown
        c = db()
        rows = c.execute(
            "SELECT id, name, phone, address, lat, lon, created FROM hospitals").fetchall()
        c.close()
        near = []
        for h in rows:
            h = dict(h)
            h["distance_km"] = None
            near.append(h)
        gps_note = "no GPS fix — broadcast to all hospitals, location unknown"
        amap = None
    else:
        near = nearby_hospitals(lat, lon)
        gps_note = "gps fix ok"
        amap = f"https://maps.google.com/?q={lat},{lon}"
    c = db()
    c.execute("INSERT INTO alerts(device_id,reason,lat,lon,roll,pitch,time,nearby) VALUES(?,?,?,?,?,?,?,?)",
              (device_id, reason, lat, lon, roll, pitch,
               now_ist(), json.dumps(near)))
    c.commit(); c.close()
    print(f"[ALERT] {reason} from {device_id} @ {lat},{lon} -> {len(near)} hospitals ({gps_note})")
    return jsonify({"ok": True, "notified": len(near), "hospitals": near,
                    "gps": gps_note, "map": amap})

@app.route("/api/alerts")
def api_alerts():
    c = db()
    rows = [dict(r) for r in c.execute("SELECT * FROM alerts ORDER BY id DESC LIMIT 30").fetchall()]
    c.close()
    for r in rows:
        try:
            r["nearby_list"] = json.loads(r["nearby"] or "[]")
        except json.JSONDecodeError:
            r["nearby_list"] = []
    return jsonify(prioritize(rows))

@app.route("/health")
def health():
    return jsonify({"ok": True, "radius_km": RADIUS_KM})

@app.errorhandler(404)
def not_found(_e):
    if request.path.startswith("/api/"):
        return jsonify({"ok": False, "error": "not found"}), 404
    return render_template("404.html"), 404

if __name__ == "__main__":
    port = int(os.environ.get("PORT", "5000"))
    app.run(host="0.0.0.0", port=port, debug=True)
