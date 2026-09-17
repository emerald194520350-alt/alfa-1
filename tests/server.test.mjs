// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import net from 'node:net';
import { spawn } from 'node:child_process';
import { mkdtemp, readFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';

const project = dirname(dirname(fileURLToPath(import.meta.url)));
const temp = await mkdtemp(join(tmpdir(), 'spore-coop-test-'));
const save = join(temp, 'session.json');
const hostToken = 'test-host-012345678901234567890123456789';
const guestToken = 'test-guest-012345678901234567890123456789';
const fingerprint = 'a'.repeat(64);
const sockets = new Set();
let server, port, logs = '';
let checks = 0;
const check = (condition, message) => { assert.ok(condition, message); checks++; };

async function start() {
  const allocator = net.createServer();
  await new Promise(ok => allocator.listen(0, '127.0.0.1', ok));
  port = allocator.address().port;
  await new Promise(ok => allocator.close(ok));
  server = spawn(process.env.SPORE_COOP_TEST_SERVER || join(project, 'SporeCoop.Server.exe'), [
    '--listen', '127.0.0.1', '--port', String(port),
    '--host-token', hostToken, '--guest-token', guestToken, '--save', save
  ], { windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
  await new Promise((ok, bad) => {
    const timer = setTimeout(() => bad(new Error('Server startup timed out\n' + logs)), 5000);
    server.once('error', bad);
    server.once('exit', code => { clearTimeout(timer); bad(new Error('Server exited ' + code + '\n' + logs)); });
    server.stdout.on('data', chunk => {
      logs += chunk.toString();
      if (logs.includes('LISTENING 127.0.0.1:' + port)) { clearTimeout(timer); ok(); }
    });
    server.stderr.on('data', chunk => { logs += chunk.toString(); });
  });
}

async function stop() {
  for (const socket of sockets) socket.destroy();
  sockets.clear();
  if (server && server.exitCode === null) {
    const exited = new Promise(ok => server.once('exit', ok));
    server.kill();
    await exited;
  }
  server = undefined;
  logs = '';
}

async function client() {
  const socket = net.createConnection({ host: '127.0.0.1', port });
  sockets.add(socket);
  await new Promise((ok, bad) => { socket.once('connect', ok); socket.once('error', bad); });
  let buffer = Buffer.alloc(0), messages = [], waiters = [], serial = 0, closed = false;
  const closedPromise = new Promise(ok => socket.on('close', () => {
    closed = true;
    for (const w of waiters) { clearTimeout(w.timer); w.bad(new Error('Client closed')); }
    waiters = [];
    ok();
  }));
  socket.on('error', () => {});
  socket.on('data', chunk => {
    buffer = Buffer.concat([buffer, chunk]);
    while (buffer.length >= 4) {
      const n = buffer.readInt32LE();
      assert.ok(n >= 2 && n <= 1048576);
      if (buffer.length < n + 4) break;
      const message = JSON.parse(buffer.subarray(4, n + 4));
      buffer = buffer.subarray(n + 4);
      const item = { message, serial: ++serial };
      const index = waiters.findIndex(w => item.serial > w.after && w.match(message));
      if (index >= 0) {
        const w = waiters.splice(index, 1)[0]; clearTimeout(w.timer); w.ok(message);
      } else messages.push(item);
    }
  });
  function wait(match, after = 0) {
    const index = messages.findLastIndex(m => m.serial > after && match(m.message));
    if (index >= 0) return Promise.resolve(messages.splice(index, 1)[0].message);
    if (closed) return Promise.reject(new Error('Client closed'));
    return new Promise((ok, bad) => {
      const w = { match, after, ok, bad };
      w.timer = setTimeout(() => {
        waiters = waiters.filter(item => item !== w);
        bad(new Error('Message wait timed out\n' + logs));
      }, 5000);
      waiters.push(w);
    });
  }
  function frame(data) {
    const body = Buffer.from(JSON.stringify(data));
    const head = Buffer.alloc(4); head.writeInt32LE(body.length);
    return Buffer.concat([head, body]);
  }
  return {
    socket, wait, closed: closedPromise, frame,
    send(data) { socket.write(frame(data)); },
    request(data, match) {
      const pending = wait(match, serial);
      socket.write(frame(data));
      return pending;
    }
  };
}

async function hello(role, token = role === 'host' ? hostToken : guestToken, fp = fingerprint) {
  const c = await client();
  const reply = await c.request({ type: 'hello', protocol: 2, role, token, fingerprint: fp },
    m => m.type === 'welcome' || m.type === 'error');
  return { c, reply };
}
const stateMessage = revision => m => m.type === 'state' && m.revision === revision;
const appearance = { modelInstance: 123, modelType: 0x2b978c46, modelGroup: 0,
  cellResource: 456, scale: 0.55, targetSize: 0.55, opacity: 1 };
const missions = () => Array(24).fill(0);
async function failure(c, data, contains) {
  const message = await c.request(data, m => m.type === 'error');
  check(message.message.includes(contains), 'Expected error ' + contains + ', got ' + message.message);
}

try {
  await start();
  const invalid = await hello('host', 'wrong-token-that-is-still-long-enough');
  check(invalid.reply.type === 'error', 'Invalid token must be refused');
  await invalid.c.closed;
  const { c: host, reply } = await hello('host');
  check(reply.type === 'welcome', 'Host handshake: ' + JSON.stringify(reply));
  const wrongBuild = await hello('guest', guestToken, 'b'.repeat(64));
  check(wrongBuild.reply.type === 'error', 'Different game/mod fingerprint must be refused');
  await wrongBuild.c.closed;
  const { c: guest } = await hello('guest');
  let state = await host.wait(m => m.type === 'state' && m.players.host && m.players.guest);
  check(state.dna === 0 && state.stage === 'cell', 'Initial shared state');
  await failure(guest, { type: 'award', revision: state.revision, eventId: 'fraud', amount: 100 }, 'Host authority');
  state = await host.request({ type: 'award', revision: state.revision, eventId: 'food-1', amount: 50 }, stateMessage(state.revision + 1));
  check(state.dna === 50, 'Shared DNA increased');
  const guestDna = await guest.wait(stateMessage(state.revision));
  check(guestDna.dna === 50, 'Guest receives identical DNA');
  await failure(host, { type: 'award', revision: state.revision, eventId: 'food-1', amount: 50 }, 'Duplicate');
  await failure(host, { type: 'award', revision: state.revision - 1, eventId: 'food-2', amount: 10 }, 'Stale');
  await failure(host, { type: 'award', revision: state.revision, eventId: 'negative', amount: -10 }, 'Invalid amount');
  const north = await host.request({ type: 'position', sequence: 0, position: [0, 10, 0], ...appearance }, m => m.type === 'position' && m.role === 'host');
  const west = await guest.request({ type: 'position', sequence: 0, position: [-10, 0, 0], ...appearance, modelInstance: 789 }, m => m.type === 'position' && m.role === 'guest');
  check(north.position[1] === 10 && west.position[0] === -10, 'Players move independently');
  check(west.modelInstance === 789 && west.scale === 0.55, 'Remote appearance is relayed with movement');
  const fullAppearance = Buffer.from('scp1-complete-cell-appearance').toString('base64');
  const relayedAppearance = await host.request({
    type: 'appearance', sequence: 0, modelInstance: 123,
    modelType: 0x2b978c46, modelGroup: 0, appearance: fullAppearance
  }, m => m.type === 'appearance' && m.role === 'host');
  const appearanceOnGuest = await guest.wait(m => m.type === 'appearance' && m.role === 'host');
  check(relayedAppearance.appearance === fullAppearance &&
    appearanceOnGuest.modelInstance === 123,
    'Complete creature appearance is relayed byte-for-byte');
  await failure(host, {
    type: 'appearance', sequence: 0, modelInstance: 123,
    modelType: 0x2b978c46, modelGroup: 0, appearance: fullAppearance
  }, 'Stale appearance');
  await failure(guest, { type: 'position', sequence: 0, position: [0, 0, 0], ...appearance }, 'Stale position');
  await failure(guest, { type: 'position', sequence: 1, position: ['bad', 0, 0], ...appearance }, 'Invalid position');

  state = await host.request({ type: 'invite' }, stateMessage(state.revision + 1));
  const invitation = await guest.wait(stateMessage(state.revision));
  check(state.invitePending && invitation.invitePending && !state.inviteAccepted,
    'Host invitation is shown to the guest');
  state = await guest.request({ type: 'inviteResponse', accepted: true },
    stateMessage(state.revision + 1));
  const acceptedInvite = await host.wait(stateMessage(state.revision));
  check(state.inviteAccepted && acceptedInvite.inviteAccepted && !state.invitePending,
    'Accepted invitation unlocks automatic world joining');

  const seedUnlocks = Array(13).fill(0);
  seedUnlocks[2] = 1;
  state = await host.request({
    type: 'seedProgress', food: 10, plantFood: 6, overPlantFood: 2,
    overAnimalFood: 1, spent: 3, unlocks: seedUnlocks, missions: missions(),
    killCount: 0, playerHasMoved: true, playerHasEaten: true,
    partCinematicPlayed: false, showMateButton: false, firstEditorEntry: false
  }, stateMessage(state.revision + 1));
  const seededGuest = await guest.wait(stateMessage(state.revision));
  check(state.progressInitialized && seededGuest.progressInitialized,
    'Host seeds the shared cell progression once');
  check(state.progress.food === 10 && state.progress.spent === 3 && state.progress.unlocks[2] === 1,
    'Seeded cell currency and unlocks are identical');

  const deltaUnlocks = Array(13).fill(0);
  deltaUnlocks[2] = 2;
  deltaUnlocks[7] = 1;
  state = await guest.request({
    type: 'progressDelta', eventId: 'progress-1', food: 14, plantFood: 7,
    overPlantFood: 2, overAnimalFood: 4, spent: -1, unlocks: deltaUnlocks,
    missions: Object.assign(missions(), { 0: 1, 1: 2 }), killCount: 1,
    playerHasMoved: true, playerHasEaten: true,
    partCinematicPlayed: true, showMateButton: true, firstEditorEntry: true
  }, stateMessage(state.revision + 1));
  const progressedHost = await host.wait(stateMessage(state.revision));
  check(state.progress.food === 14 && state.progress.spent === 2 &&
    progressedHost.progress.food === 14 && progressedHost.progress.spent === 2,
    'Either player can add shared progress or refund shared points');
  check(state.progress.unlocks[2] === 2 && state.progress.unlocks[7] === 1,
    'Unlocked parts merge into one shared inventory');
  check(state.progress.partCinematicPlayed && state.progress.showMateButton &&
    state.progress.firstEditorEntry,
    'Cell tutorial cinematics, mate prompt, and first editor entry are shared');
  await failure(guest, {
    type: 'progressDelta', eventId: 'progress-1', food: 1, plantFood: 0,
    overPlantFood: 0, overAnimalFood: 0, spent: 0, unlocks: deltaUnlocks,
    missions: missions(), killCount: 0, playerHasMoved: false, playerHasEaten: false,
    partCinematicPlayed: false, showMateButton: false, firstEditorEntry: false
  }, 'Duplicate');

  state = await host.request({ type: 'editorOpen', editorId: 12345 }, stateMessage(state.revision + 1));
  const mirroredEditor = await guest.wait(stateMessage(state.revision));
  check(state.evolving && mirroredEditor.evolving && state.editorId === 12345,
    'Opening an editor mirrors its shared editor state');
  const liveSpecies = Buffer.from('live-editor-model-v1').toString('base64');
  const liveOnGuest = await host.request({
    type: 'speciesLive', sequence: 0, species: liveSpecies
  }, m => m.type === 'speciesLive' && m.role === 'host');
  const liveOnHost = await guest.wait(m => m.type === 'speciesLive' && m.role === 'host');
  check(liveOnGuest.species === liveSpecies && liveOnHost.species === liveSpecies,
    'Live creature edits are broadcast byte-for-byte');
  const finalLiveSpecies = Buffer.from('live-editor-model-v2').toString('base64');
  state = await guest.request({ type: 'editorClose', species: finalLiveSpecies }, stateMessage(state.revision + 1));
  const editorClosedHost = await host.wait(stateMessage(state.revision));
  check(!state.evolving && !editorClosedHost.evolving && state.species === finalLiveSpecies,
    'Either player can finish the mirrored editor with the same creature');

  state = await host.request({ type: 'editorOpen', editorId: 54321 }, stateMessage(state.revision + 1));
  await guest.wait(stateMessage(state.revision));
  state = await guest.request({ type: 'editorClose', species: '' }, stateMessage(state.revision + 1));
  await host.wait(stateMessage(state.revision));
  check(!state.evolving && state.species === finalLiveSpecies,
    'An empty final editor snapshot unlocks the world without erasing the last valid creature');

  await guest.request({ type: 'requestEvolution' }, m => m.type === 'evolutionRequestAccepted');
  const requested = await host.wait(m => m.type === 'evolutionRequested');
  check(requested.role === 'guest', 'Guest can request evolution');
  state = await host.request({ type: 'beginEvolution', revision: state.revision, owner: 'guest' }, stateMessage(state.revision + 1));
  const guestEditor = await guest.wait(stateMessage(state.revision));
  check(state.evolving && guestEditor.evolving && state.editor === 'guest', 'Both enter shared editor state');
  await failure(host, { type: 'position', sequence: 1, position: [1, 1, 0], ...appearance }, 'World paused');
  await failure(host, { type: 'proposeEdit', revision: state.revision, eventId: 'locked', species: '' }, 'Editor is locked');
  await failure(guest, { type: 'proposeEdit', revision: state.revision, eventId: 'invalid', species: '!' }, 'Invalid base64');
  const species = Buffer.from('test-opaque-genome-v1').toString('base64');
  await guest.request({ type: 'proposeEdit', revision: state.revision, eventId: 'edit-1', species }, m => m.type === 'proposalAccepted');
  const proposal = await host.wait(m => m.type === 'editProposed');
  check(proposal.species === species, 'Editor proposal reaches host unchanged');
  await failure(host, { type: 'applyEdit', revision: state.revision, eventId: 'edit-1', cost: 100 }, 'Insufficient DNA');
  state = await host.request({ type: 'applyEdit', revision: state.revision, eventId: 'edit-1', cost: 20 }, stateMessage(state.revision + 1));
  const shared = await guest.wait(stateMessage(state.revision));
  check(state.dna === 30 && shared.dna === 30 && state.species === species && shared.species === species, 'Single DNA charge and identical genome');
  await failure(host, { type: 'applyEdit', revision: state.revision, eventId: 'edit-1', cost: 20 }, 'Duplicate');
  await failure(host, { type: 'finishEvolution', revision: state.revision, stage: 'creature' }, 'Both players');
  await guest.request({ type: 'ready', revision: state.revision }, m => m.type === 'state' && m.ready.includes('guest'));
  await host.request({ type: 'ready', revision: state.revision }, m => m.type === 'state' && m.ready.includes('host') && m.ready.includes('guest'));
  state = await host.request({ type: 'finishEvolution', revision: state.revision, stage: 'creature' }, stateMessage(state.revision + 1));
  const final = await guest.wait(stateMessage(state.revision));
  check(state.stage === 'creature' && final.stage === 'creature' && !state.evolving && !final.evolving, 'Both finish evolution');
  const saved = JSON.parse(await readFile(save, 'utf8'));
  check(saved.Dna === 30 && saved.Species === species && saved.Stage === 'creature', 'Persistent protocol state');
  check(saved.ProgressInitialized && saved.FoodProgression === 14 &&
    saved.EvolutionPointsSpent === 2 && saved.CellUnlocks[2] === 2 && saved.CellUnlocks[7] === 1,
    'Shared cell progress and unlocked parts are persisted');
  check(saved.CellMissions[0] === 1 && saved.CellMissions[1] === 2 && saved.PlayerHasEaten,
    'Tutorial mission state is persisted');
  check(saved.PartCinematicPlayed && saved.ShowMateButton && saved.FirstEditorEntry,
    'Tutorial transition flags are persisted');
  guest.socket.destroy();
  await guest.closed;
  const disconnected = await host.wait(m => m.type === 'state' && !m.players.guest);
  check(!disconnected.evolving, 'Disconnect releases editor');
  const { c: reconnect } = await hello('guest');
  const recovered = await reconnect.wait(m => m.type === 'state');
  check(recovered.species === species && recovered.dna === 30, 'Guest reconnect receives full shared state');
  const duplicate = await hello('guest');
  check(duplicate.reply.type === 'error', 'A third player cannot reuse the guest role');
  await duplicate.c.closed;
  const malformed = await client();
  const badLength = Buffer.alloc(4); badLength.writeInt32LE(1048577);
  malformed.socket.write(badLength);
  await malformed.closed;
  check(true, 'Oversized frame rejected');
  // A frame split across TCP packets must still be read exactly once.
  const ping = host.frame({ type: 'ping' });
  const pong = host.wait(m => m.type === 'pong');
  host.socket.write(ping.subarray(0, 2));
  host.socket.write(ping.subarray(2, 5));
  host.socket.write(ping.subarray(5));
  await pong;
  check(true, 'Fragmented TCP frame');
  host.socket.destroy();
  await host.closed;
  await reconnect.closed;
  check(true, 'Host exit disconnects guest');
  await stop();
  await start();
  const { c: restored } = await hello('host');
  const loaded = await restored.wait(m => m.type === 'state');
  check(loaded.species === species && loaded.dna === 30 && loaded.stage === 'creature', 'Server restart restores protocol save');
  check(loaded.progressInitialized && loaded.progress.food === 14 &&
    loaded.progress.spent === 2 && loaded.progress.unlocks[7] === 1,
    'Server restart restores shared progress and parts');
  await failure(restored, { type: 'award', revision: loaded.revision, eventId: 'food-1', amount: 50 }, 'Duplicate');
  console.log('PASS: ' + checks + ' protocol assertions. No gameplay was tested. Artifacts: ' + temp);
} catch (error) {
  console.error('Failed after ' + checks + ' assertions. Server log:\n' + logs);
  throw error;
} finally {
  await stop();
}
