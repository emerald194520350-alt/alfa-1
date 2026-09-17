// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const project = dirname(dirname(fileURLToPath(import.meta.url)));
const probe = await readFile(join(project, 'native', 'Probe.cpp'), 'utf8');
const net = await readFile(join(project, 'native', 'CoopNet.cpp'), 'utf8');

function body(source, signature, nextSignature) {
  const start = source.indexOf(signature);
  const end = source.indexOf(nextSignature, start + signature.length);
  assert.ok(start >= 0 && end > start, `Unable to isolate ${signature}`);
  return source.slice(start, end);
}

const updateRemote = body(probe, 'void UpdateRemoteCell(', 'std::string Base64Encode(');
const updateStability = body(probe, 'bool UpdateLocalCellStability(', 'void SubmitLocalAppearance(');
const submitAppearance = body(probe, 'void SubmitLocalAppearance(', 'void ApplyRemoteAppearance(');
const coopUpdate = body(probe, 'void CoopUpdate()', 'void Spawn()');
const inviteUI = body(probe, 'void UpdateInviteUI(', 'void CoopUpdate()');
const stateHandler = body(net, 'if (type == "state")', 'if (type == "editorOpen")');

assert.match(net, /std::uint64_t gSpeciesSequence = 1;/,
  'The first appearance sequence must be newer than the zero-initialized receiver state');
assert.match(net, /constexpr int kProtocol = 2;/,
  'The native client must reject older incompatible protocol builds');
assert.doesNotMatch(updateRemote,
  /ResourceKey\(snapshot\.remoteModelInstance[\s\S]*snapshot\.remoteModelGroup\)/,
  'A network clone must never fall back to an uncached foreign resource key');
assert.match(updateRemote, /if \(!remote\) return;[\s\S]*remote->mIsIdle/,
  'Clone cooldown and failed creation paths must return before dereferencing remote');
assert.match(updateRemote, /gRemoteCellResource != snapshot\.remoteCellResource/,
  'A changed cell body resource must rebuild the network clone');
assert.match(updateRemote, /remotePositionReceivedTick[\s\S]*RemoveRemoteCell\(/,
  'A stale position stream must remove the clone after the peer leaves the world');
assert.match(updateRemote, /player->mTargetSize[\s\S]*snapshot\.remoteTargetSize/,
  'The local player must adopt the larger shared cell growth target');
assert.match(updateRemote, /player->mTransform\.SetScale/,
  'The local player scale must catch up when the peer grows first');
assert.match(updateStability, /RemoveRemoteCell\(/,
  'A local player-cell rebuild must remove the clone that references the old cell resource');
assert.match(coopUpdate,
  /if \(!snapshot\.enabled \|\| !snapshot\.connected\)[\s\S]*RemoveRemoteCell\([\s\S]*return;/,
  'Disconnect handling must remove the frozen network clone before returning');
assert.match(coopUpdate, /connectionGeneration/,
  'A reconnect must reset per-connection appearance state so it is submitted again');
assert.match(stateHandler, /ObjectHasKey\(json, "players"[\s\S]*ClearRemotePeerStateLocked\(\)/,
  'A state snapshot without the peer must clear its position and appearance identity');
assert.match(net, /void ClearRemotePeerStateLocked\(\)[\s\S]*hasRemotePosition = false[\s\S]*remoteAppearanceSequence = 0/,
  'Peer-state cleanup must also reset sequence numbers for a replacement process');
assert.match(coopUpdate, /!snapshot\.hasRemotePosition[\s\S]*RemoveRemoteCell\("cooperative peer left"\)/,
  'A peer leaving the session must explicitly remove its frozen network clone');
for (const reset of [
  'gProgressSeedSent = false',
  'gHasProgressBaseline = false',
  'gAppliedProgressRevision = 0',
  'gAppliedSpeciesSequence = 0',
  'gLastLocalSpecies.clear()'
]) {
  assert.ok(coopUpdate.includes(reset), `Reconnect handling must perform: ${reset}`);
}
assert.match(net, /type == "welcome"[\s\S]*speciesSequence = 0;[\s\S]*speciesBlob\.clear\(\)/,
  'A fresh server handshake must discard mirrored-editor sequence state from the old connection');
assert.match(probe, /UTFWin::IWindow\* FindVisiblePausePanel\(/,
  'The Esc panel must be detected from the visible UI hierarchy');
assert.match(inviteUI, /FindVisiblePausePanel\(/,
  'Invite visibility must follow the detected Esc panel');
assert.doesNotMatch(inviteUI, /IsPaused\(\)|GetPauseCount\(/,
  'Invite visibility must not depend on simulation pause state');
assert.match(probe, /GetComponentName\(\)[\s\S]*"Button"/,
  'The Esc panel signature must use its visible button hierarchy instead of an unknown hard-coded ID');
assert.match(inviteUI, /BringToFront\(/,
  'A root-level cooperative button must be raised above the modal Esc layout every frame');
assert.match(submitAppearance, /gLastSubmittedAppearanceCellResource/,
  'A growth resource rebuild must resubmit appearance even when the species key is unchanged');
assert.match(probe, /gWasEditorMode[\s\S]*gLastSubmittedAppearanceKey = ResourceKey\{\}/,
  'Leaving the editor must force the edited appearance to be submitted to the world clone');
for (const field of ['partCinematicPlayed', 'showMateButton', 'firstEditorEntry']) {
  assert.match(probe, new RegExp(`result\\.${field}`),
    `${field} must be read from the cell save state`);
  assert.match(probe, new RegExp(`value\\.${field}`),
    `${field} must be applied to the cell save state`);
  assert.match(net, new RegExp(`\\\\"${field}\\\\"`),
    `${field} must be sent over the cooperative protocol`);
}

console.log('PASS: native lifecycle source invariants. No gameplay was tested.');