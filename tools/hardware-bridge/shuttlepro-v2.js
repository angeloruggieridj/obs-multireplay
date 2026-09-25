// shuttlepro-v2.js
//
// Bridge: Contour Design ShuttlePRO v2 (standard USB HID) -> obs-multireplay,
// through obs-websocket. See README.md for the setup.
//
// Unlike the SloMo Mini this is an ordinary HID device: no FTDI chip, no raw
// mode handshake, no baud rate. node-hid reads the reports directly.
//
//   npm install
//   node shuttlepro-v2.js
//
// Contour's own configuration software claims the device exclusively: quit it
// before starting this bridge.

const HID = require('node-hid');
const { connectObs, triggerHotkey, callVendor } = require('./obs-link');

const VENDOR_ID = 0x0b33;
const PRODUCT_ID = 0x0030;

// Frame rate used to turn jog ticks into seconds. Set it to your project's rate.
const FPS = 25;

// ---------------------------------------------------------------------------
// Keys. The ShuttlePRO v2 has 15 unlabelled keys: bits 0-7 of byte 3 are keys
// 1-8, bits 0-6 of byte 4 are keys 9-15. Rename them after your own overlay.
// ---------------------------------------------------------------------------
const BUTTON_NAMES = Array.from({ length: 15 }, (_, i) => `button-${i + 1}`);

// Keys that act on PRESS. Map the rest the way jlcooper-slomo-mini.js does.
const BUTTON_ACTIONS = {
  // 'button-1': () => triggerHotkey('ReplayMarkIn'),
  // 'button-2': () => triggerHotkey('ReplayMarkOut'),
  'button-14': () => callVendor('step_event_selection', { delta: -1 }), // previous event
  'button-15': () => callVendor('step_event_selection', { delta: 1 }), // next event
};

function handleButton(name, pressed) {
  const ts = new Date().toISOString().split('T')[1].replace('Z', '');
  console.log(`[${ts}] ${name}: ${pressed ? 'press' : 'release'}`);
  if (!pressed) return;
  const action = BUTTON_ACTIONS[name];
  if (action) action();
  else console.log(`  -> nothing mapped to "${name}"`);
}

// ---------------------------------------------------------------------------
// Shuttle ring (byte 0, signed -7..+7, spring-centred) -> speed rocker:
//   centre 0 -> 100%   right +7 -> 200%   left -7 -> 5%
// set_speed is forward-only; shuttling backwards would need a request with a
// direction, which the plugin does not have yet.
// ---------------------------------------------------------------------------
let lastSpeedPct = null;

function shuttleToPercent(value) {
  if (value >= 0) return Math.round(100 + (value / 7) * 100); // 100..200
  return Math.round(100 + (value / 7) * 95); // 100..5
}

function handleShuttle(value) {
  const pct = shuttleToPercent(value);
  if (pct === lastSpeedPct) return;
  lastSpeedPct = pct;
  console.log(`Shuttle ${value} -> ${pct}%`);
  callVendor('set_speed', { percent: pct });
}

// ---------------------------------------------------------------------------
// Jog wheel (byte 1). An ABSOLUTE 8-bit position that wraps, so the delta is
// taken the short way round the ring (255 -> 0 is +1, not -255). The first
// report only sets the baseline, which costs the very first tick.
//
// From there, the same window as the SloMo Mini: deltas summed for 50 ms and
// sent as one scrub_seconds jump (see jlcooper-slomo-mini.js for why).
// ---------------------------------------------------------------------------
const JOG_FLUSH_MS = 50;
let prevJog = null;
let jogAccumulator = 0;
let jogFlushTimer = null;

function flushJog() {
  jogFlushTimer = null;
  if (jogAccumulator === 0) return;
  const seconds = jogAccumulator / FPS;
  jogAccumulator = 0;
  callVendor('scrub_seconds', { seconds });
}

function handleJog(value) {
  if (prevJog === null) {
    prevJog = value;
    return;
  }
  let delta = value - prevJog;
  if (delta > 128) delta -= 256;
  else if (delta < -128) delta += 256;
  prevJog = value;
  if (delta === 0) return;
  jogAccumulator += delta;
  if (!jogFlushTimer) jogFlushTimer = setTimeout(flushJog, JOG_FLUSH_MS);
}

// ---------------------------------------------------------------------------
// Report, 5 bytes:
//   [0] shuttle (int8, -7..+7)   [1] jog (uint8, wrapping)   [2] unused
//   [3] keys 1-8                 [4] keys 9-15
// ---------------------------------------------------------------------------
let prevKeysLow = 0;
let prevKeysHigh = 0;

function processKeyByte(current, previous, firstIndex) {
  const changed = current ^ previous;
  for (let bit = 0; bit < 8; bit++) {
    const index = firstIndex + bit;
    if (!((changed >> bit) & 1) || index >= BUTTON_NAMES.length) continue;
    handleButton(BUTTON_NAMES[index], !!((current >> bit) & 1));
  }
}

function processReport(data) {
  if (data.length < 5) return;
  handleShuttle(data[0] > 127 ? data[0] - 256 : data[0]);
  handleJog(data[1]);
  processKeyByte(data[3], prevKeysLow, 0);
  processKeyByte(data[4], prevKeysHigh, 8);
  prevKeysLow = data[3];
  prevKeysHigh = data[4];
}

async function main() {
  await connectObs();

  console.log('Looking for the ShuttlePRO v2...');
  let device;
  try {
    device = new HID.HID(VENDOR_ID, PRODUCT_ID);
  } catch (err) {
    console.error('Device not found, or held by another process (Contour\'s own software?):',
      err.message);
    process.exit(1);
  }
  device.on('data', processReport);
  device.on('error', (err) => console.error('HID error:', err.message));
  console.log('\nReady.\n');
}

main().catch((err) => {
  console.error('Unexpected error:', err);
  process.exit(1);
});
