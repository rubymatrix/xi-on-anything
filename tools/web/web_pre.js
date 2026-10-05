// --pre-js of the browser build. The render thread (gfx_queue.c) gets the page's canvas as an
// OffscreenCanvas in a message of our own (web_give_canvas, tools/web/gfx.js); in that worker it becomes
// Module.canvas and Emscripten's "#canvas" target (specialHTMLTargets: a worker has no document to look it
// up in), where Dawn's WebGPU bindings find it. Emscripten's own message handler ignores messages without
// a cmd.
if (typeof ENVIRONMENT_IS_PTHREAD != 'undefined' && ENVIRONMENT_IS_PTHREAD) {
  self.addEventListener('message', (e) => {
    if (!e.data || !e.data.xiCanvas) return;
    Module['canvas'] = e.data.xiCanvas;
    if (typeof specialHTMLTargets != 'undefined') specialHTMLTargets['#canvas'] = e.data.xiCanvas;
  });
}
