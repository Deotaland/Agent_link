// Exercise the actual embedded page script without a device or network.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const html = fs.readFileSync(path.join(__dirname, '../../firmware/components/agent_link/portal/connecting.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
function page(response) {
  const nodes = Object.fromEntries(['lang', 't', 'm', 'sp', 'retry'].map(id =>
    [id, { checked: false, textContent: '', style: {}, hidden: true }]));
  const timers = [];
  const context = vm.createContext({
    document: { getElementById: id => nodes[id] },
    fetch: response,
    setTimeout: (callback, delay) => { const timer = { callback, delay }; timers.push(timer); return timer; },
    clearTimeout: timer => { timer.cleared = true; },
  });
  vm.runInContext(script, context);
  return { nodes, timers, context };
}
const settle = () => new Promise(resolve => setImmediate(resolve));

test('WiFi success retains an explicit activation/binding next step', async () => {
  const p = page(async () => ({ json: async () => ({ state: 'connected', ssid: 'Home', ip: '192.0.2.1' }) }));
  await settle();
  assert.equal(p.nodes.sp.style.display, 'none');
  assert.match(p.nodes.m.textContent, /设备设置/);
  assert.equal(p.nodes.retry.hidden, true);
  assert.equal(p.timers[0].cleared, true);
});

test('Wrong password has a retry path and translates after result', async () => {
  const p = page(async () => ({ json: async () => ({ state: 'failed', reason: 'badpass' }) }));
  await settle();
  assert.match(p.nodes.m.textContent, /密码/);
  assert.equal(p.nodes.retry.hidden, false);
  p.nodes.lang.checked = true;
  p.context.render();
  assert.match(p.nodes.m.textContent, /Wrong password/);
});

test('A vanished hotspot or stalled request ends progress without claiming failure', async () => {
  const p = page(() => new Promise(() => {}));
  p.timers.find(timer => timer.delay === 35000).callback();
  assert.match(p.nodes.t.textContent, /查看设备/);
  assert.equal(p.nodes.sp.style.display, 'none');
  assert.equal(p.nodes.retry.hidden, true);
  p.context.finish({ state: 'connected', ssid: 'late result' });
  assert.doesNotMatch(p.nodes.m.textContent, /late result/);
});

test('A polling error remains retryable before the bounded deadline', async () => {
  const p = page(async () => { throw new Error('hotspot disconnected'); });
  await settle();
  assert.ok(p.timers.some(timer => timer.delay === 1500));
  p.timers.find(timer => timer.delay === 35000).callback();
  assert.equal(p.context.finished, true);
});

test('Network names are rendered as text and remain visible across language changes', async () => {
  const ssid = '<img src=x onerror=alert(1)>"';
  const p = page(async () => ({ json: async () => ({ state: 'connected', ssid }) }));
  await settle();
  assert.ok(p.nodes.m.textContent.includes(ssid));
  p.nodes.lang.checked = true;
  p.context.render();
  assert.ok(p.nodes.m.textContent.includes(ssid));
  assert.match(p.nodes.t.textContent, /WiFi connected/);
});
