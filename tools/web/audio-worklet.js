// The game's sound (runtime/portable/sdl_web.c's ring): interleaved stereo floats at 48 kHz in the wasm
// memory, which this reads straight from. Silence when the ring runs dry.
class XiAudio extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ring = null;
    this.port.onmessage = (e) => {
      const { mem, ring } = e.data;
      this.idx = new Int32Array(mem, ring, 4); // write, read, size
      this.data = new Float32Array(mem, ring + 16, this.idx[2] * 2);
      this.ring = ring;
    };
  }
  process(inputs, outputs) {
    const out = outputs[0], L = out[0], R = out[1] || out[0];
    if (!this.ring) return true;
    const size = this.idx[2];
    let r = Atomics.load(this.idx, 1);
    const w = Atomics.load(this.idx, 0);
    for (let i = 0; i < L.length; i++) {
      if (r === w) { L[i] = 0; R[i] = 0; continue; }
      L[i] = this.data[2 * r];
      R[i] = this.data[2 * r + 1];
      r = (r + 1) % size;
    }
    Atomics.store(this.idx, 1, r);
    return true;
  }
}
registerProcessor('xi-audio', XiAudio);
