<<<<<<< HEAD
# ESP32-Sleep-Monitor
=======
# esp32-sleep-monitor
>>>>>>> 3a03bcc168693f285a4288b13378dfb3a2eab223
# ESP32 Sleep Monitor

## 🧠 Projekt leírása

Ez a projekt egy ESP32-alapú intelligens alvásmonitorozó rendszer, amely több környezeti paramétert figyel a pihentető alvás biztosítása érdekében. A rendszer különféle szenzorokat használ a fényviszonyok, a levegőminőség, a zajszint és a hőmérséklet-páratartalom mérésére, majd az adatokat WiFi-n keresztül továbbítja egy Flask-alapú szerverre, ahol megjelenítés, naplózás és értékelés történik.

## 📦 Funkciók

- 🌡️ Hőmérséklet és páratartalom mérése (AHTX0)
- 💡 Fényerő és színhőmérséklet mérése (BH1750, TCS34725)
- 🌬️ eCO₂ és TVOC levegőminőség mérés (ENS160)
- 🔊 Zajszint mérése és eseményalapú adatküldés (INMP441)
- 🌙 Eco mód: csökkentett fogyasztás, ritkább mérés és WiFi kikapcsolás
- 📊 Webes dashboard valós idejű kijelzéssel, alvásbarát értékeléssel
- 💾 Logging funkció: adatnaplózás CSV-be és grafikonok ZIP-ben
- 🔁 Automatikus szenzor-konfiguráció és WiFi kapcsolódás

## 📁 Mappastruktúra

esp32-sleep-monitor/
├── README.md
├── main.cpp # ESP32 firmware
├── app.py # Flask backend szerver
├── dashboard.html # Webes dashboard UI
├── /docs # Word + PowerPoint dokumentáció
│ ├── prezentacio.pptx
│ └── dokumentacio.docx
├── /figures # Diagramok, képernyőképek
│ ├── system_diagram.png
│ ├── sensor_table.png
│ └── sleep_quality_plot.png
├── /logs # Naplózott adatok (gitignore-olt)
│ └── .gitignore
└── requirements.txt # Python csomagok a szerverhez

## ⚙️ Rendszerkomponensek

- **ESP32**: adatgyűjtés és küldés WiFi-n keresztül
- **Szenzorok**:
  - `AHTX0`: hőmérséklet, páratartalom
  - `BH1750`: fényerő
  - `TCS34725`: RGB → színhőmérséklet
  - `ENS160`: eCO₂, TVOC
  - `INMP441`: zajszint
- **Flask szerver**: adatok fogadása, valós idejű kijelzés, loggolás, konfiguráció
- **Web dashboard**: Chart.js és Socket.IO segítségével

## 🚀 Telepítés és használat

### 1. Firmware feltöltése ESP32-re
Töltsd fel a `main.cpp` fájlt Arduino IDE vagy PlatformIO segítségével.

### 2. Flask szerver futtatása

```bash
git clone https://github.com/<felhasználónév>/esp32-sleep-monitor.git
cd esp32-sleep-monitor
pip install -r requirements.txt
python app.py

A szerver a http://<gép-ip>:5000 címen elérhető.

3. Webes dashboard használata
Valós idejű adatok megtekintése

Logging indítása/leállítása

Eco mód vezérlése

Grafikonok letöltése ZIP-ben

🔧 Követelmények
Hardver: ESP32 DevKit, szenzorok

Szoftver:

Python 3.9+

Flask, Flask-SocketIO

MsgPack, Pandas, Matplotlib

🧪 Példa használat
Beállított alacsony fény → alvásbarát értékelés ✅

Zajesemény → azonnali adatküldés + naplózás

Eco mód aktiválása → ritkább frissítések, kevesebb fogyasztás

📜 Licenc
MIT License © 2025 Viktor Mózer