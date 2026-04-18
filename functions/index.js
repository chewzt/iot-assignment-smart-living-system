const {setGlobalOptions} = require("firebase-functions/v2");
const {onDocumentCreated} = require("firebase-functions/v2/firestore");
const admin = require("firebase-admin");

admin.initializeApp();
const db = admin.firestore();
const FieldValue = admin.firestore.FieldValue;

setGlobalOptions({
  region: "asia-southeast1",
  maxInstances: 10,
});

const HOME_REF = db.doc("home_state/current");

const DEVICE_PATHS = {
  dht22: "devices/DHT22_001",
  ldr: "devices/LDR_001",
  mq2: "devices/MQ2_001",
  light: "devices/Light_001",
  fan: "devices/Fan_001",
  curtain: "devices/CurtainServo_001",
  nfcDoor: "devices/NFCDoor_001",
  doorMagnetic: "devices/DoorMagnetic_001",
  ultrasonic: "devices/Ultrasonic_001",
  alarm: "devices/Alarm_001",
};

/**
 * Normalizes a Firestore reference or string path into a plain path string.
 * @param {*} value A document reference-like object or string path.
 * @return {string} The normalized Firestore document path.
 */
function getRefPath(value) {
  if (!value) return "";
  if (typeof value.path === "string") return value.path;
  if (typeof value === "string") return value.replace(/^\//, "");
  return "";
}

/**
 * Converts a value to a number and falls back if conversion fails.
 * @param {*} value The value to convert.
 * @param {number} fallback The fallback number.
 * @return {number} The converted number or fallback.
 */
function getNumber(value, fallback) {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

/**
 * Returns the first non-null and non-undefined value.
 * @param {*} a The first candidate value.
 * @param {*} b The second candidate value.
 * @param {*} c The third candidate value.
 * @return {*} The first defined value, or undefined.
 */
function firstDefined(a, b, c) {
  if (a !== undefined && a !== null) return a;
  if (b !== undefined && b !== null) return b;
  if (c !== undefined && c !== null) return c;
  return undefined;
}

/**
 * Checks whether an automation rule condition matches the latest reading.
 * @param {string} condition The rule condition identifier.
 * @param {Object} readingData The sensor reading payload.
 * @param {Object} homeState The current home state document data.
 * @return {boolean} True when the condition matches.
 */
function matchesCondition(condition, readingData, homeState) {
  if (condition === "LIGHT_LT_300") {
    const lightValue = firstDefined(
        readingData.light_intensity,
        readingData.lightIntensity,
        readingData.value,
    );
    return getNumber(lightValue, 0) < 300;
  }

  if (condition === "SMOKE_DETECTED") {
    return readingData.smokeDetected === true;
  }

  if (condition === "ARMED_AND_MOTION") {
    return homeState.homeMode === "Armed" &&
      readingData.motionDetected === true;
  }

  if (condition === "ARMED_AND_DOOR_OPEN") {
    return homeState.homeMode === "Armed" &&
      readingData.doorClosed === false;
  }

  return false;
}

/**
 * Applies automation actions to device documents and the home state.
 * @param {Object} action The action definition to apply.
 * @return {Promise<void>} Resolves when the batch write completes.
 */
async function applyAction(action) {
  const batch = db.batch();
  const homeUpdates = {
    lastUpdated: FieldValue.serverTimestamp(),
  };

  if (typeof action.lightOn === "boolean") {
    batch.set(db.doc(DEVICE_PATHS.light), {
      status: action.lightOn,
      lastUpdated: FieldValue.serverTimestamp(),
    }, {merge: true});
    homeUpdates.lightOn = action.lightOn;
  }

  if (typeof action.fanOn === "boolean") {
    batch.set(db.doc(DEVICE_PATHS.fan), {
      status: action.fanOn,
      lastUpdated: FieldValue.serverTimestamp(),
    }, {merge: true});
    homeUpdates.fanOn = action.fanOn;
  }

  if (typeof action.curtainOn === "boolean") {
    batch.set(db.doc(DEVICE_PATHS.curtain), {
      status: action.curtainOn,
      lastUpdated: FieldValue.serverTimestamp(),
    }, {merge: true});
    homeUpdates.curtainOn = action.curtainOn;
  }

  if (typeof action.alarmOn === "boolean") {
    batch.set(db.doc(DEVICE_PATHS.alarm), {
      status: action.alarmOn,
      lastUpdated: FieldValue.serverTimestamp(),
    }, {merge: true});
    homeUpdates.alarmOn = action.alarmOn;
  }

  if (typeof action.doorClosed === "boolean") {
    batch.set(db.doc(DEVICE_PATHS.nfcDoor), {
      doorClosed: action.doorClosed,
      lastUpdated: FieldValue.serverTimestamp(),
    }, {merge: true});

    batch.set(db.doc(DEVICE_PATHS.doorMagnetic), {
      status: action.doorClosed,
      lastUpdated: FieldValue.serverTimestamp(),
    }, {merge: true});

    homeUpdates.doorClosed = action.doorClosed;
  }

  batch.set(HOME_REF, homeUpdates, {merge: true});
  await batch.commit();
}

exports.onSensorReadingCreated = onDocumentCreated(
    "sensor_readings/{readingId}",
    async (event) => {
      const snap = event.data;
      if (!snap) return;

      const readingData = snap.data();
      const devicePath = getRefPath(readingData.deviceID);

      const homeUpdates = {
        lastUpdated: FieldValue.serverTimestamp(),
      };

      if (devicePath === DEVICE_PATHS.dht22) {
        if (typeof readingData.temperature === "number") {
          homeUpdates.temperature = readingData.temperature;
        }
        if (typeof readingData.humidity === "number") {
          homeUpdates.humidity = readingData.humidity;
        }
      }

      if (devicePath === DEVICE_PATHS.ldr) {
        const ldrValue = firstDefined(
            readingData.light_intensity,
            readingData.lightIntensity,
            readingData.value,
        );
        homeUpdates.light_intensity = getNumber(ldrValue, 0);
      }

      if (devicePath === DEVICE_PATHS.mq2) {
        homeUpdates.smokeDetected = readingData.smokeDetected === true;
      }

      if (devicePath === DEVICE_PATHS.ultrasonic) {
        homeUpdates.motionDetected = readingData.motionDetected === true;
        if (typeof readingData.distanceCm === "number") {
          homeUpdates.distanceCm = readingData.distanceCm;
        }
      }

      if (devicePath === DEVICE_PATHS.doorMagnetic) {
        homeUpdates.doorClosed = readingData.doorClosed === true;
      }

      await HOME_REF.set(homeUpdates, {merge: true});

      const homeSnap = await HOME_REF.get();
      const homeState = homeSnap.exists ? homeSnap.data() : {};

      const rulesSnap = await db.collection("automation_rules")
          .where("enabled", "==", true)
          .where("triggeredDeviceID", "==", readingData.deviceID)
          .get();

      for (const ruleDoc of rulesSnap.docs) {
        const rule = ruleDoc.data();

        if (!matchesCondition(rule.condition, readingData, homeState)) {
          continue;
        }

        const actions = Array.isArray(rule.actions) ? rule.actions : [];
        for (const action of actions) {
          await applyAction(action);
        }
      }
    },
);
