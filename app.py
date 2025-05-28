from datetime import datetime
import csv
import os
from collections import deque
import msgpack
from flask import Flask, request, jsonify, send_file, send_from_directory
from flask_socketio import SocketIO
import pandas as pd
import math
from uuid import uuid4

session_log_file = None  # aktuális session logfájl elérési útja

#import ace_tools as tools; tools.display_dataframe_to_user(name="Lekerekített szenzoradatok", dataframe=pd.DataFrame([sensor_data]))
# Mintaszintű adatok és struktúra a kerekítéshez

app = Flask(__name__)
socketio = SocketIO(app, cors_allowed_origins="*")

API_KEY = os.getenv("SENSOR_API_KEY", "alvashozTitkosToken123")
eco_mode = False
logging_enabled = True
latest_data = {}
history = deque(maxlen=20000)
log_dir = os.path.abspath("logs")
os.makedirs(log_dir, exist_ok=True)

EXPECTED_FIELDS = ['timestamp', 'temp', 'humidity', 'lux', 'r', 'g', 'b', 'eco2', 'tvoc',
                   'ens160Status', 'noise', 'eco_mode']

@app.route('/')
def serve_dashboard():
    return send_from_directory('.', 'dashboard.html')

@app.route('/data', methods=['POST'])
def receive_data():
    auth = request.headers.get("Authorization")
    if auth != f"Bearer {API_KEY}":
        return jsonify({'error': 'Unauthorized'}), 401

    if request.headers.get('Content-Type') != 'application/msgpack':
        return jsonify({'error': 'Unsupported Content-Type'}), 415

    try:
        data = msgpack.unpackb(request.get_data(), raw=False)
    except Exception as e:
        return jsonify({'error': f'Failed to parse MsgPack: {e}'}), 400

    if not isinstance(data, dict):
        return jsonify({'error': 'Invalid data format'}), 400

    data['timestamp'] = datetime.now().isoformat()
    data['eco_mode'] = eco_mode

    global latest_data
    latest_data = data
    history.append(data)

    if logging_enabled:
        write_to_log(data)

    socketio.emit('new_data', data)
    return jsonify({'status': 'OK'})

@app.route('/sensor.json')
def serve_sensor():
    return jsonify({k: latest_data.get(k, '') for k in EXPECTED_FIELDS})

@app.route('/history.json')
def serve_history():
    return jsonify(list(history))

@app.route('/eco_status')
def eco_status():
    return jsonify({'eco_mode': eco_mode})

@app.route('/set_mode')
def set_mode():
    global eco_mode
    val = request.args.get("eco")
    if val == "1":
        eco_mode = True
    elif val == "0":
        eco_mode = False
    else:
        return "Hibás paraméter", 400
    return f"eco_mode = {'ON' if eco_mode else 'OFF'}"

log_session_id = None

@app.route('/toggle_logging', methods=['POST'])
def toggle_logging():
    global logging_enabled, session_log_file

    logging_enabled = not logging_enabled

    if logging_enabled:
        session_id = uuid4().hex[:8]
        timestamp = datetime.now().strftime("%Y-%m-%d_%H%M%S")
        session_log_file = os.path.join(log_dir, f"session_log_{timestamp}_{session_id}.csv")
    else:
        session_log_file = None

    return jsonify({
        'logging': logging_enabled,
        'session_log': os.path.basename(session_log_file) if session_log_file else None
    })


@app.route('/log_status')
def log_status():
    return jsonify({'logging': logging_enabled})

@app.route('/config')
def get_config():
    return jsonify({
        "temp_interval": 1800000,
        "gas_interval": 60000,
        "light_interval": 60000,
        "color_interval": 60000,
        "lux_threshold": 50,
        "color_threshold": 10,
        "eco2_threshold": 100,
        "tvoc_threshold": 100
    })

@app.route('/download_log')
def download_log():
    date = request.args.get("date", datetime.now().strftime("%Y-%m-%d"))
    log_file = os.path.join(log_dir, f"data_log_{date}.csv")
    if os.path.exists(log_file):
        return send_file(log_file, as_attachment=True, mimetype='text/csv')
    return f"Log file for {date} not found.", 404

@app.route('/download_plots')
def download_plots():
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import pandas as pd
    import io
    import zipfile
    import numpy as np

    if not session_log_file or not os.path.exists(session_log_file):
        return "❌ Nincs aktív session log fájl vagy még nem jött adat!", 404

    try:
        df = pd.read_csv(session_log_file, on_bad_lines='skip')
        df['timestamp'] = pd.to_datetime(df['timestamp'], errors='coerce')
    except Exception as e:
        return f"❌ Log fájl olvasási hiba: {e}", 500

    def rgb_to_cct(r, g, b):
        X = 0.4124*r + 0.3576*g + 0.1805*b
        Y = 0.2126*r + 0.7152*g + 0.0722*b
        Z = 0.0193*r + 0.1192*g + 0.9505*b
        denom = X + Y + Z
        if denom == 0: return np.nan
        x = X / denom
        y = Y / denom
        n = (x - 0.3320) / (0.1858 - y) if (0.1858 - y) != 0 else np.nan
        cct = 449 * n**3 + 3525 * n**2 + 6823.3 * n + 5520.33
        return cct if 1000 < cct < 40000 else np.nan

    df['cct'] = df.apply(lambda row: rgb_to_cct(row['r'], row['g'], row['b']), axis=1)
    columns_to_plot = ['temp', 'humidity', 'lux', 'eco2', 'tvoc', 'noise', 'cct']

    zip_buffer = io.BytesIO()
    with zipfile.ZipFile(zip_buffer, mode="w") as zf:
        for col in columns_to_plot:
            if col not in df.columns:
                continue
            plt.figure(figsize=(10, 4))
            plt.plot(df['timestamp'], df[col], label=col)
            plt.xlabel('Time')
            plt.ylabel(col)
            plt.title(f'{col} over time')
            plt.grid(True)
            plt.tight_layout()
            img_buf = io.BytesIO()
            plt.savefig(img_buf, format='png')
            plt.close()
            img_buf.seek(0)
            zf.writestr(f'{col}.png', img_buf.read())

    zip_buffer.seek(0)
    return send_file(zip_buffer, mimetype='application/zip',
                     as_attachment=True, download_name='session_plots.zip')

def write_to_log(data):
    if not session_log_file:
        print("⚠️ Nincs aktív session logfájl!")
        return

    try:
        file_exists = os.path.isfile(session_log_file)
        with open(session_log_file, 'a', newline='') as csvfile:
            writer = csv.DictWriter(csvfile, fieldnames=EXPECTED_FIELDS)
            if not file_exists:
                writer.writeheader()
            writer.writerow({k: data.get(k, '') for k in EXPECTED_FIELDS})
            print(f"📝 Session loggolva: {session_log_file}")
    except Exception as e:
        print(f"❌ Session log írási hiba: {e}")


if __name__ == '__main__':
    socketio.run(app, host='0.0.0.0', port=5000)