// --pre-js of the Node build: in Node the build's `var Module` is local to its CommonJS module, so a
// runner (tools/web/node_run.js) hands its hooks over in globalThis.xiModule instead.
if (globalThis.xiModule) Object.assign(Module, globalThis.xiModule);
