// obs-link.js
//
// The OBS side shared by every controller bridge: one obs-websocket
// connection, OBS hotkeys by name, and the "multireplay" vendor requests the
// plugin registers (src/remote-control.cpp).
//
// Where OBS is comes from the environment, never from the source:
//   OBS_WS_URL       default ws://127.0.0.1:4455
//   OBS_WS_PASSWORD  default empty (Tools > WebSocket Server Settings in OBS)

const OBSWebSocket = require('obs-websocket-js').default;

const OBS_WS_URL = process.env.OBS_WS_URL || 'ws://127.0.0.1:4455';
const OBS_WS_PASSWORD = process.env.OBS_WS_PASSWORD || '';
const VENDOR = 'multireplay';

const obs = new OBSWebSocket();

async function connectObs() {
  console.log(`Connecting to OBS at ${OBS_WS_URL}...`);
  await obs.connect(OBS_WS_URL, OBS_WS_PASSWORD || undefined);
  console.log('Connected to OBS.');
}

// A hotkey registered by the plugin (Settings > Hotkeys in OBS lists them).
async function triggerHotkey(name) {
  try {
    await obs.call('TriggerHotkeyByName', { hotkeyName: name });
    console.log(`  -> hotkey ${name}`);
  } catch (err) {
    console.error(`  -> hotkey ${name} failed:`, err.message);
  }
}

// A "multireplay" vendor request. Resolves to the plugin's answer — always an
// object with "success", and "error" when success is false — or null when the
// request never reached the plugin (obs-websocket down, plugin not loaded).
async function callVendor(requestType, requestData = {}) {
  try {
    const res = await obs.call('CallVendorRequest', {
      vendorName: VENDOR,
      requestType,
      requestData,
    });
    const answer = res.responseData || {};
    if (answer.success === false) {
      console.error(`  -> ${requestType} refused: ${answer.error}`);
    }
    return answer;
  } catch (err) {
    console.error(`  -> ${requestType} failed:`, err.message);
    return null;
  }
}

async function disconnectObs() {
  await obs.disconnect();
}

module.exports = { connectObs, disconnectObs, triggerHotkey, callVendor };
