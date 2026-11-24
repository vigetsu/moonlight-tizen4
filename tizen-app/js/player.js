/*
 * Low-latency video player: appends fMP4 fragments (one per frame) from the
 * bridge into a Media Source Extensions SourceBuffer and keeps the playhead
 * pinned to the live edge.
 */
(function (ML) {
  'use strict';

  var MAX_LAG = 0.12;        // seconds behind live edge before we jump forward
  var JUMP_TARGET = 0.03;    // how far behind the edge to land after a jump
  var MAX_QUEUE = 20;        // queued fragments before we give up and resync
  var KEEP_BEHIND = 3;       // seconds of old buffer kept before trimming

  function Player(video, onNeedIdr) {
    this.video = video;
    this.onNeedIdr = onNeedIdr;
    this.ms = null;
    this.sb = null;
    this.codec = null;
    this.queue = [];
    this.waitInit = true;
    this.lastTrim = 0;
    this.stats = { frames: 0, bytes: 0, jumps: 0, resyncs: 0, lag: 0 };
    var self = this;
    video.addEventListener('error', function () {
      ML.log('video error: ' + (video.error && video.error.code));
      self.reset();
      self.requestIdr();
    });
  }

  Player.supports = function (codec) {
    return !!(window.MediaSource && MediaSource.isTypeSupported('video/mp4; codecs="' + codec + '"'));
  };

  Player.prototype.setCodec = function (codec) {
    if (this.codec === codec && this.ms) return;
    this.reset();
    this.codec = codec;
    var self = this;
    var ms = new MediaSource();
    this.ms = ms;
    ms.addEventListener('sourceopen', function () {
      if (self.ms !== ms) return;
      try {
        self.sb = ms.addSourceBuffer('video/mp4; codecs="' + codec + '"');
      } catch (e) {
        ML.log('addSourceBuffer failed: ' + e);
        ML.app.showError('Bu TV "' + codec + '" video biçimini desteklemiyor.');
        return;
      }
      self.sb.addEventListener('updateend', function () { self.onUpdateEnd(); });
      self.sb.addEventListener('error', function () { ML.log('SourceBuffer error'); });
      self.pump();
    });
    this.video.src = URL.createObjectURL(ms);
  };

  Player.prototype.reset = function () {
    this.queue = [];
    this.waitInit = true;
    if (this.ms) {
      try { if (this.ms.readyState === 'open') this.ms.endOfStream(); } catch (e) { /* ignore */ }
    }
    this.ms = null;
    this.sb = null;
    this.codec = null;
    try {
      this.video.removeAttribute('src');
      this.video.load();
    } catch (e) { /* ignore */ }
  };

  Player.prototype.requestIdr = function () {
    this.stats.resyncs++;
    this.queue = [];
    this.waitInit = true;
    this.onNeedIdr();
  };

  Player.prototype.pushInit = function (buf) {
    // A new init segment always precedes a keyframe, so older queued frames are useless.
    this.queue = [buf];
    this.waitInit = false;
    this.pump();
  };

  Player.prototype.pushFragment = function (buf) {
    if (this.waitInit) return;
    this.stats.frames++;
    this.stats.bytes += buf.byteLength;
    if (this.queue.length >= MAX_QUEUE) {
      ML.log('video queue overflow, resyncing');
      this.requestIdr();
      return;
    }
    this.queue.push(buf);
    this.pump();
  };

  Player.prototype.pump = function () {
    var sb = this.sb;
    if (!sb || sb.updating || !this.queue.length) return;
    var trimEnd = this.trimPoint();
    if (trimEnd > 0) {
      try { sb.remove(0, trimEnd); return; } catch (e) { /* fall through to append */ }
    }
    // Merge everything queued into one append: far fewer round trips on a slow TV.
    var data;
    if (this.queue.length === 1) {
      data = this.queue[0];
    } else {
      var total = 0, i;
      for (i = 0; i < this.queue.length; i++) total += this.queue[i].byteLength;
      data = new Uint8Array(total);
      var off = 0;
      for (i = 0; i < this.queue.length; i++) {
        data.set(this.queue[i], off);
        off += this.queue[i].byteLength;
      }
    }
    this.queue = [];
    try {
      sb.appendBuffer(data);
    } catch (e) {
      ML.log('appendBuffer failed: ' + e.name);
      if (e.name === 'QuotaExceededError') {
        try { sb.remove(0, Math.max(0, this.video.currentTime - 0.5)); } catch (e2) { /* ignore */ }
      }
      this.requestIdr();
    }
  };

  Player.prototype.trimPoint = function () {
    var now = Date.now();
    if (now - this.lastTrim < 5000) return 0;
    this.lastTrim = now;
    var b = this.sb.buffered;
    if (!b.length) return 0;
    var t = this.video.currentTime - KEEP_BEHIND;
    return t > b.start(0) + 1 ? t : 0;
  };

  Player.prototype.onUpdateEnd = function () {
    var v = this.video;
    var b = v.buffered;
    if (b.length) {
      var end = b.end(b.length - 1);
      var lag = end - v.currentTime;
      this.stats.lag = lag;
      if (lag > MAX_LAG || v.currentTime < b.start(b.length - 1)) {
        v.currentTime = Math.max(b.start(b.length - 1), end - JUMP_TARGET);
        this.stats.jumps++;
      }
      if (v.paused) {
        var p = v.play();
        if (p && p.catch) p.catch(function () { /* autoplay retry on next frame */ });
      }
    }
    this.pump();
  };

  ML.Player = Player;
})(window.ML = window.ML || {});
