# Smart Living System - Realtime Database version

This version removes Firestore and Cloud Functions so you can stay on Firebase Spark.

## What changed
- `public/index.html` now uses Firebase Realtime Database
- `public/controller.html` is a browser-based automation controller
- `firebase.json` now deploys Hosting + Realtime Database rules only
- `database.rules.json` contains simple demo rules
- `sample-realtime-database.json` can be imported into Realtime Database as starter data

## Realtime Database structure
- `/home/state` -> summary for dashboard
- `/devices/<deviceId>/telemetry` -> sensor uploads
- `/devices/<deviceId>/desired` -> commands written by dashboard or automation
- `/devices/<deviceId>/state` -> actual state reported by actuator device
- `/automation/rules` -> automation rules read by `controller.html`
- `/system/controller` -> heartbeat/status of the local automation controller page

## How to use
1. Replace the Firebase config placeholders in:
   - `public/index.html`
   - `public/controller.html`
2. In Firebase Console, create Realtime Database.
3. Import `sample-realtime-database.json` into Realtime Database.
4. Deploy:
   ```bash
   firebase deploy --only hosting,database
   ```
5. Open your Hosting URL for the dashboard.
6. Open `/controller.html` on one laptop/browser tab and keep it running during the demo.

## How the flow works now
### Manual control
Dashboard -> `/devices/.../desired` -> actuator ESP32 listens -> physical action -> actuator updates `/devices/.../state`

### Automation
Sensor ESP32 -> `/devices/.../telemetry` -> `controller.html` evaluates rules -> writes `/devices/.../desired` and `/home/state`

## Notes
- This is designed for class demo use. Tighten database rules before public use.
- Without Cloud Functions, automation runs only while `controller.html` is open on a laptop, Raspberry Pi kiosk, or always-on browser.
