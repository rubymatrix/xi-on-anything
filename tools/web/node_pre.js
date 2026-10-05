// --pre-js of the Node build: in Node the build's `var Module` is local to its CommonJS module, so a
// runner (tools/web/node_run.js) hands its hooks over in globalThis.xiModule instead.
if (globalThis.xiModule) Object.assign(Module, globalThis.xiModule);

// XI_CPU_PROF=<folder>: V8's sampling profiler on the game's thread (web_frame starts it at the first frame and
// stops it XI_CPU_PROF_AT seconds later), written there as prof-game.cpuprofile for Chrome's or VS Code's
// profile viewers. --cpu-prof, or a profiler in every worker from its start, stalls the start-up.
if (ENVIRONMENT_IS_NODE && ENVIRONMENT_IS_PTHREAD && process.env.XI_CPU_PROF) {
  let session = null;
  globalThis.xiProfStart = () => {
    session = new (require('inspector').Session)();
    session.connect();
    session.post('Profiler.enable');
    session.post('Profiler.setSamplingInterval', { interval: 250 });
    session.post('Profiler.start');
  };
  globalThis.xiProfStop = (tag) => session && session.post('Profiler.stop', (err, r) => {
    if (!err) require('fs').writeFileSync(process.env.XI_CPU_PROF + '/prof-' + tag + '.cpuprofile', JSON.stringify(r.profile));
  });
}
