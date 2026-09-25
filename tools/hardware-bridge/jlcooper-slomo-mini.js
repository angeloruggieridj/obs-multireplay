// jlcooper-slomo-mini.js
//
// Bridge: JLCooper SloMo Mini (raw mode over USB, FTDI) -> obs-multireplay,
// through obs-websocket. See README.md for the setup and the key map.
//
//   npm install
//   node jlcooper-slomo-mini.js

const { usb } = require('usb');
const { connectObs, disconnectObs, triggerHotkey, callVendor } = require('./obs-link');

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------
const VENDOR_ID = 0x0760;
const PRODUCT_ID = 0x0005;
const IN_ENDPOINT = 1;
const OUT_ENDPOINT = 2;

const BAUD_38400 = 0xc04e; // FTDI divisor code for 38400 baud
const DISPLAY_REFRESH_MS = 200; // measured not to disturb input polling
const KEEP_ALIVE_MS = 50;

// Frame rate used to turn jog ticks into seconds and to draw the timecode.
// Set it to your project's rate.
const FPS = 25;

// ---------------------------------------------------------------------------
// Key table, taken from USB captures of the controller's original software.
// Press byte -> name; the release byte is always the press byte - 0x40.
// ---------------------------------------------------------------------------
const BUTTON_NAMES = {
  0x40: 'replay',
  0x45: 'shift',
  0x46: 'cycle-player',
  0x4c: 'reverse-play',
  0x4d: 'stop',
  0x4e: 'play',
  0x4f: 'f-fwd',
  0x50: 'cam-a',
  0x51: 'cam-b',
  0x52: 'cam-c',
  0x53: 'cam-d',
  0x54: 'mark-in',
  0x55: 'w1',
  0x56: 'frame-minus1',
  0x58: 'frame-plus1',
  0x59: 'w2',
  0x5a: 'mark-out',
  0x5c: 'store-cue',
  0x5d: 'last-cue',
  0x5e: 'next-cue',
  0x5f: 'digit-0',
  0x60: 'digit-1',
  0x61: 'digit-2',
  0x62: 'digit-3',
  0x63: 'digit-4',
  0x64: 'digit-5',
  0x65: 'digit-6',
  0x66: 'digit-7',
  0x67: 'digit-8',
  0x68: 'digit-9',
  0x69: 'clr',
  0x6a: 'enter',
};

// ---------------------------------------------------------------------------
// State the device does not report
// ---------------------------------------------------------------------------
let shiftHeld = false;
let lastSpeedPct = null;
// The last get_playback_status answer, refreshed with the display. REC toggles
// against it rather than against a flag of our own, which would drift the first
// time recording is started from the panel.
let lastStatus = null;

function selectCamera(baseNumber) {
  const cam = shiftHeld ? baseNumber + 4 : baseNumber;
  triggerHotkey(`ReplayACamera${cam}`);
}

function toggleRecording() {
  const recording = lastStatus ? lastStatus.recording : false;
  triggerHotkey(recording ? 'MultiReplayStopRecording' : 'MultiReplayStartRecording');
}

function stepList(delta) {
  callVendor('step_list_selection', { delta: shiftHeld ? -delta : delta });
}

// Keys that act on PRESS.
const BUTTON_ACTIONS = {
  'mark-in': () => triggerHotkey('ReplayMarkIn'),
  'mark-out': () => triggerHotkey('ReplayMarkOut'),
  w1: () => triggerHotkey('ReplayMarkInOut5'),
  w2: () => triggerHotkey('ReplayMarkInOut10'),
  play: () => triggerHotkey('ReplayPlayLastEventToOutput'),
  'f-fwd': () => triggerHotkey('ReplayJumpToNow'),
  'frame-minus1': () => triggerHotkey('ReplayStepBackward'),
  'frame-plus1': () => triggerHotkey('ReplayStepForward'),
  'cam-a': () => selectCamera(1),
  'cam-b': () => selectCamera(2),
  'cam-c': () => selectCamera(3),
  'cam-d': () => selectCamera(4),
  replay: () => toggleRecording(),
  stop: () => triggerHotkey('ReplayStopEvents'),
  'reverse-play': () => triggerHotkey('ReplayPlayReverse'),
  'cycle-player': () => callVendor('toggle_active_channel'),
  'store-cue': () => stepList(1),
  'last-cue': () => callVendor('step_event_selection', { delta: -1 }),
  'next-cue': () => callVendor('step_event_selection', { delta: 1 }),
};

// Three-digit entry: digits fill the buffer, CLR empties it, ENTER jumps to the
// event with that id.
let digitBuffer = '';

function handleButton(name, pressed) {
  const ts = new Date().toISOString().split('T')[1].replace('Z', '');
  console.log(`[${ts}] ${name}: ${pressed ? 'press' : 'release'}`);

  if (name === 'shift') {
    shiftHeld = pressed;
    return;
  }
  if (!pressed) return;

  if (name.startsWith('digit-')) {
    digitBuffer = (digitBuffer + name.slice(-1)).slice(-3);
    console.log(`  -> digits: "${digitBuffer}"`);
    return;
  }
  if (name === 'clr') {
    digitBuffer = '';
    console.log('  -> digits cleared');
    return;
  }
  if (name === 'enter') {
    if (digitBuffer !== '') {
      const id = parseInt(digitBuffer, 10);
      console.log(`  -> event ${id}`);
      callVendor('select_event_by_id', { id });
      digitBuffer = '';
    }
    return;
  }

  const action = BUTTON_ACTIONS[name];
  if (action) action();
  else console.log(`  -> nothing mapped to "${name}"`);
}

// ---------------------------------------------------------------------------
// T-bar (tag 0x83, 0..63) -> replay speed, stepless: 0 -> 5%, 63 -> 100%.
// ---------------------------------------------------------------------------
function handleTBar(value) {
  const pct = Math.max(5, Math.round((value / 63) * 100));
  if (pct === lastSpeedPct) return;
  lastSpeedPct = pct;
  console.log(`T-bar ${value}/63 -> ${pct}%`);
  callVendor('set_speed', { percent: pct });
}

// ---------------------------------------------------------------------------
// Jog wheel (tag 0x81)
//
// Each value is a 7-bit two's-complement delta the device already scales with
// the turning speed: 0x01..0x3F forward, 0x40..0x7F backward.
//
// Deltas are summed over a short window and sent as ONE scrub_seconds jump.
// step_frames restarts playback on every step, which is right for the ±1 keys
// and hopeless for a wheel sending ~34 events a second: OBS falls so far behind
// that unrelated requests (a Mark In) queue up behind the backlog. The window
// also lets a stray +1 inside a run of -8 cancel out.
// ---------------------------------------------------------------------------
const JOG_FLUSH_MS = 50;
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
  if (value === 0x00) return;
  jogAccumulator += value < 0x40 ? value : value - 128;
  if (!jogFlushTimer) jogFlushTimer = setTimeout(flushJog, JOG_FLUSH_MS);
}

// ---------------------------------------------------------------------------
// USB / FTDI
// ---------------------------------------------------------------------------
let device = null;

async function ftdiControlOut(request, value, index = 0) {
  await device.controlTransferOut(
    { requestType: 'vendor', recipient: 'device', request, value, index },
    new Uint8Array(0)
  );
}

async function initFtdi() {
  const SIO_DATA_8O1 = 0x0008 | (0x1 << 8); // 8 data bits, odd parity, 1 stop bit
  await ftdiControlOut(0x00, 0x00); // reset
  await ftdiControlOut(0x00, 0x01); // purge RX
  await ftdiControlOut(0x00, 0x02); // purge TX
  await ftdiControlOut(0x04, SIO_DATA_8O1);
  await ftdiControlOut(0x02, 0x0000); // no flow control
  await ftdiControlOut(0x03, BAUD_38400);
  await ftdiControlOut(0x09, 1); // latency timer 1 ms
}

// EVERY write goes through this one queue. The keep-alive and the display run
// on independent timers, and two transferOut calls in flight at once fail with
// "endpoint not found".
let writeQueue = Promise.resolve();

function sendBytes(bytes) {
  const write = writeQueue.then(() => device.transferOut(OUT_ENDPOINT, new Uint8Array(bytes)));
  writeQueue = write.catch(() => {}); // one failed write must not stall the rest
  return write;
}

// The byte sequence the original software sends at start-up to put the device
// in raw mode (it also blanks both LCD lines).
async function enterRawMode() {
  const blank32 = new Array(32).fill(0x20);
  await sendBytes([0x8f, 0xff]);
  for (let i = 0; i < 3; i++) {
    for (const reg of [0x10, 0x11, 0x12, 0x13]) await sendBytes([0x80, reg]);
  }
  for (let i = 0; i < 2; i++) {
    await sendBytes([0x80, 0x15]);
    await sendBytes([0x80, 0x19]);
    await sendBytes([0x81, 0x00, ...blank32, 0xff]);
  }
  console.log('Raw mode on.');
}

// The original software polls registers 0x0b..0x0f while running. Whether the
// device needs it to stay connected is unknown, so we do the same.
function startKeepAlive() {
  setInterval(async () => {
    try {
      for (const reg of [0x0b, 0x0c, 0x0d, 0x0e, 0x0f]) await sendBytes([0x80, reg]);
    } catch (err) {
      console.error('Keep-alive failed:', err.message);
    }
  }, KEEP_ALIVE_MS);
}

// ---------------------------------------------------------------------------
// Display: two 16-character lines in ONE write — 0x81 0x00 <32 ASCII> 0xff.
//   TC 00:01:23:14      position on the panel's seek bar
//   E042 L03 100%       event, list, speed
// ---------------------------------------------------------------------------
function formatTimecode(ms) {
  if (!(ms >= 0)) return '--:--:--:--';
  const totalSeconds = Math.floor(ms / 1000);
  const frames = Math.floor(((ms % 1000) / 1000) * FPS);
  const pad = (n) => String(n).padStart(2, '0');
  return `${pad(Math.floor(totalSeconds / 3600))}:${pad(Math.floor(totalSeconds / 60) % 60)}:` +
    `${pad(totalSeconds % 60)}:${pad(frames)}`;
}

function writeDisplay(line1, line2) {
  const pad16 = (s) => s.padEnd(16, ' ').slice(0, 16);
  const text = Array.from(Buffer.from(pad16(line1) + pad16(line2), 'ascii'));
  sendBytes([0x81, 0x00, ...text, 0xff]).catch((err) => {
    console.error('Display write failed:', err.message);
  });
}

function startDisplayLoop() {
  setInterval(async () => {
    const status = await callVendor('get_playback_status');
    if (!status || !status.success) return;
    lastStatus = status;
    const list = 'L' + String(status.activeList).padStart(2, '0');
    const event = 'E' + String(status.eventId).padStart(3, '0');
    const speed = String(status.speedPercent).padStart(3, ' ') + '%';
    writeDisplay(`TC ${formatTimecode(status.cursorMs)}`, `${event} ${list} ${speed}`);
  }, DISPLAY_REFRESH_MS);
}

// ---------------------------------------------------------------------------
// Incoming bytes: two-byte packets, tag then value. Returns how many bytes were
// consumed, so a packet split across two USB reads is finished by the next one.
// ---------------------------------------------------------------------------
function processBuffer(buffer) {
  let offset = 0;
  while (offset < buffer.length) {
    const tag = buffer[offset];
    if (tag !== 0x80 && tag !== 0x81 && tag !== 0x83) {
      offset += 1; // not a tag: drop one byte and resynchronise
      continue;
    }
    if (offset + 1 >= buffer.length) break; // value still to come
    const value = buffer[offset + 1];
    offset += 2;

    if (tag === 0x80) {
      if (BUTTON_NAMES[value]) handleButton(BUTTON_NAMES[value], true);
      else if (BUTTON_NAMES[value + 0x40]) handleButton(BUTTON_NAMES[value + 0x40], false);
      else console.log(`Unknown key code 0x${value.toString(16)}`);
    } else if (tag === 0x81) {
      handleJog(value);
    } else {
      handleTBar(value);
    }
  }
  return offset;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
async function main() {
  await connectObs();

  console.log('Looking for the SloMo Mini...');
  device = await usb.findDeviceByIds(VENDOR_ID, PRODUCT_ID);
  if (!device) {
    console.error('Device not found.');
    process.exit(1);
  }
  await device.open();
  if (!device.configuration && device.configurations.length > 0) {
    await device.selectConfiguration(device.configurations[0].configurationValue);
  }
  const iface = device.configuration.interfaces[0].interfaceNumber;
  await device.claimInterface(iface);

  await initFtdi();
  await enterRawMode();
  startKeepAlive();
  startDisplayLoop();
  console.log('\nReady.\n');

  let pending = Buffer.alloc(0);
  for (;;) {
    let result;
    try {
      result = await device.transferIn(IN_ENDPOINT, 64);
    } catch (err) {
      console.error('Read failed:', err.message);
      break;
    }
    if (!result || !result.data || result.data.byteLength <= 2) continue;
    // The first two bytes of every FTDI read are modem status, not data.
    const raw = new Uint8Array(result.data.buffer, result.data.byteOffset, result.data.byteLength);
    pending = Buffer.concat([pending, Buffer.from(raw.slice(2))]);
    pending = pending.subarray(processBuffer(pending));
  }

  await device.releaseInterface(iface);
  await device.close();
  await disconnectObs();
}

main().catch((err) => {
  console.error('Unexpected error:', err);
  process.exit(1);
});
