// The browser build's graphics plumbing (gfx_queue.c): the page's <canvas id="canvas"> handed to the render
// thread's worker as an OffscreenCanvas, once, on the page's thread. The worker may not exist yet when this
// runs (pthread_create starts it from the page's thread too), so it waits for it.
addToLibrary({
  web_give_canvas__proxy: 'async',
  web_give_canvas__deps: ['$PThread'],
  web_give_canvas: (thread) => {
    if (typeof document == 'undefined') return;
    const canvas = document.getElementById('canvas');
    if (!canvas || canvas.xiGiven) return;
    const give = (tries) => {
      const worker = PThread.pthreads[thread];
      if (!worker) {
        if (tries < 500) setTimeout(() => give(tries + 1), 10);
        return;
      }
      const oc = canvas.transferControlToOffscreen();
      canvas.xiGiven = true;
      worker.postMessage({ xiCanvas: oc }, [oc]);
    };
    give(0);
  },
});
