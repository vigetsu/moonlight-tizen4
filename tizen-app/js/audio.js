/*
 * PCM audio output via Web Audio. The bridge sends 20 ms chunks of 16-bit
 * interleaved PCM; we schedule them back to back with a small jitter buffer.
 */
(function (ML) {
  'use strict';

  var TARGET_DELAY = 0.05;  // seconds of audio buffered ahead
  var MAX_DELAY = 0.15;     // above this we drop to catch up

  function Audio() {
    this.ctx = null;
    this.rate = 48000;
    this.channels = 2;
    this.next = 0;
    this.enabled = true;
    this.stats = { drops: 0, delay: 0 };
  }

  Audio.prototype.configure = function (rate, channels) {
    this.rate = rate;
    this.channels = channels;
    this.ensure();
    this.next = 0;
  };

  Audio.prototype.ensure = function () {
    if (this.ctx) return;
    var Ctx = window.AudioContext || window.webkitAudioContext;
    if (!Ctx) {
      ML.log('Web Audio not available');
      this.enabled = false;
      return;
    }
    this.ctx = new Ctx();
  };

  Audio.prototype.resume = function () {
    this.ensure();
    if (this.ctx && this.ctx.state === 'suspended' && this.ctx.resume) this.ctx.resume();
  };

  Audio.prototype.push = function (bytes) {
    if (!this.enabled) return;
    this.ensure();
    var ctx = this.ctx;
    if (!ctx) return;
    var ch = this.channels;
    // bytes may not be 2-byte aligned within its ArrayBuffer, so copy.
    var pcm = new Int16Array(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength));
    var frames = Math.floor(pcm.length / ch);
    if (!frames) return;
    var buf = ctx.createBuffer(ch, frames, this.rate);
    for (var c = 0; c < ch; c++) {
      var out = buf.getChannelData(c);
      for (var i = 0, j = c; i < frames; i++, j += ch) out[i] = pcm[j] / 32768;
    }
    var now = ctx.currentTime;
    var delay = this.next - now;
    if (delay < 0.005) {
      this.next = now + TARGET_DELAY;   // underrun: rebuild the cushion
    } else if (delay > MAX_DELAY) {
      this.stats.drops++;
      this.next = now + TARGET_DELAY;   // too far behind: skip ahead
    }
    this.stats.delay = this.next - now;
    var src = ctx.createBufferSource();
    src.buffer = buf;
    src.connect(ctx.destination);
    src.start(this.next);
    this.next += frames / this.rate;
  };

  Audio.prototype.stop = function () {
    this.next = 0;
  };

  ML.Audio = Audio;
})(window.ML = window.ML || {});
