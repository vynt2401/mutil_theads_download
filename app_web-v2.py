import fcntl
import os
import json
from flask import Flask, render_template_string, request, jsonify

app = Flask(__name__)
REQUEST_FILE = "requests.txt"
PROGRESS_FILE = "progress.json"

HTML_TEMPLATE = """
<!DOCTYPE html>
<html lang="vi">
<head>
    <meta charset="UTF-8">
    <title>Linux Downloader UI</title>
    <style>
        body { font-family: sans-serif; background: #1e1e2e; color: #cdd6f4; padding: 20px; }
        .box { max-width: 750px; margin: 0 auto; background: #313244; padding: 20px; border-radius: 8px; }
        input[type="url"] { width: 70%; padding: 10px; border-radius: 4px; border: 1px solid #45475a; }
        button { padding: 10px 20px; background: #89b4fa; border: none; border-radius: 4px; font-weight: bold; cursor: pointer; }
        .bar-bg { width: 100%; height: 20px; background: #45475a; border-radius: 10px; overflow: hidden; margin: 10px 0; }
        .bar-fill { height: 100%; width: 0%; background: #a6e3a1; transition: width 0.3s; }
    </style>
</head>
<body>
    <div class="box">
        <h2>🚀 High-Speed Multi-Thread Downloader</h2>
        <form method="POST" action="/download">
            <input type="url" name="url" placeholder="Nhap URL can tai vao day..." required>
            <button type="submit">Tai ve</button>
        </form>
        {% if message %}<p style="color: #a6e3a1;">{{ message }}</p>{% endif %}

        <div style="margin-top: 20px; background: #181825; padding: 15px; border-radius: 6px;">
            <div style="display:flex; justify-content:space-between; font-weight:bold;">
                <span id="lbl-url">Dang cho yeu cau...</span>
                <span id="lbl-pct">0.0%</span>
            </div>
            <div class="bar-bg"><div id="main-bar" class="bar-fill"></div></div>
        </div>
    </div>

    <script>
        function update() {
            fetch('/progress')
                .then(r => r.json())
                .then(d => {
                    if (!d.active && d.pct === 0) return;
                    document.getElementById('lbl-url').innerText = d.url;
                    document.getElementById('lbl-pct').innerText = d.pct.toFixed(1) + "%";
                    document.getElementById('main-bar').style.width = d.pct + "%";
                });
        }
        setInterval(update, 500);
    </script>
</body>
</html>
"""

@app.route("/", methods=["GET"])
def index():
    return render_template_string(HTML_TEMPLATE)

@app.route("/download", methods=["POST"])
def download():
    url = request.form.get("url", "").strip()
    if url:
        with open(REQUEST_FILE, "a") as f:
            fcntl.flock(f.fileno(), fcntl.LOCK_EX)
            f.write(url + "\n")
            fcntl.flock(f.fileno(), fcntl.LOCK_UN)
        return render_template_string(HTML_TEMPLATE, message="Da them URL vao hang cho tai!")
    return render_template_string(HTML_TEMPLATE, message="URL khong hop le!")

@app.route("/progress", methods=["GET"])
def get_progress():
    if os.path.exists(PROGRESS_FILE):
        try:
            with open(PROGRESS_FILE, "r") as f:
                return jsonify(json.load(f))
        except Exception:
            pass
    return jsonify({"active": False, "url": "", "file_size": 0, "downloaded_bytes": 0, "pct": 0.0, "threads": []})

if __name__ == "__main__":
    app.run(host="127.0.0.1", port=5000)
