/*
 * Input capture while streaming: TV remote, USB/Bluetooth keyboard and mouse,
 * and gamepads (Gamepad API). Everything is forwarded to the bridge as small
 * JSON messages.
 */
(function (ML) {
  'use strict';

  var KEY = {
    ENTER: 13, LEFT: 37, UP: 38, RIGHT: 39, DOWN: 40,
    BACK: 10009, EXIT: 10182,
    RED: 403, GREEN: 404, YELLOW: 405, BLUE: 406,
    PLAY_PAUSE: 10252, PLAY: 415, PAUSE: 19
  };
  ML.KEY = KEY;

  var LONG_PRESS_MS = 700;

  // Remote keys that we turn into keyboard keys on the PC (Windows VK codes).
  var REMOTE_TO_VK = {};
  REMOTE_TO_VK[KEY.ENTER] = 0x0D;
  REMOTE_TO_VK[KEY.LEFT] = 0x25;
  REMOTE_TO_VK[KEY.UP] = 0x26;
  REMOTE_TO_VK[KEY.RIGHT] = 0x27;
  REMOTE_TO_VK[KEY.DOWN] = 0x28;

  // Remote keys mapped to gamepad buttons when "remote = gamepad" is on.
  var REMOTE_TO_PAD = {};
  REMOTE_TO_PAD[KEY.ENTER] = 0x1000;  // A
  REMOTE_TO_PAD[KEY.UP] = 0x0001;
  REMOTE_TO_PAD[KEY.DOWN] = 0x0002;
  REMOTE_TO_PAD[KEY.LEFT] = 0x0004;
  REMOTE_TO_PAD[KEY.RIGHT] = 0x0008;
  REMOTE_TO_PAD[KEY.PLAY_PAUSE] = 0x0010; // Start
  REMOTE_TO_PAD[KEY.YELLOW] = 0x0020;     // Back/Select

  // Standard Gamepad API button index -> Moonlight button flag.
  var PAD_BUTTONS = [
    0x1000, 0x2000, 0x4000, 0x8000,   // A B X Y
    0x0100, 0x0200, 0, 0,             // LB RB (LT/RT are analog)
    0x0020, 0x0010,                   // Back Start
    0x0040, 0x0080,                   // LS RS click
    0x0001, 0x0002, 0x0004, 0x0008,   // D-pad
    0x0400                            // Guide
  ];

  function Input(send, video) {
    this.send = send;
    this.video = video;
    this.active = false;
    this.remotePad = false;
    this.remoteButtons = 0;
    this.backDownAt = 0;
    this.onMenu = null;
    this.onStats = null;
    this.pads = {};
    this.padTimer = null;
    this.lastMouse = 0;
    var self = this;
    this.handlers = {
      keydown: function (e) { self.key(e, true); },
      keyup: function (e) { self.key(e, false); },
      mousemove: function (e) { self.mouseMove(e); },
      mousedown: function (e) { self.mouseButton(e, true); },
      mouseup: function (e) { self.mouseButton(e, false); },
      wheel: function (e) { self.wheel(e); },
      contextmenu: function (e) { if (self.active) e.preventDefault(); }
    };
  }

  Input.prototype.start = function () {
    if (this.active) return;
    this.active = true;
    for (var k in this.handlers) document.addEventListener(k, this.handlers[k], true);
    var self = this;
    this.padTimer = setInterval(function () { self.pollPads(); }, 8);
  };

  Input.prototype.stop = function () {
    if (!this.active) return;
    this.active = false;
    for (var k in this.handlers) document.removeEventListener(k, this.handlers[k], true);
    clearInterval(this.padTimer);
    this.pads = {};
    this.remoteButtons = 0;
  };

  Input.prototype.isRemoteKey = function (e) {
    // Tizen remote keys arrive without a physical key "code".
    return e.keyCode >= 10000 || (e.keyCode >= 403 && e.keyCode <= 415) || !e.code;
  };

  Input.prototype.key = function (e, down) {
    if (!this.active) return;
    var kc = e.keyCode;

    if (kc === KEY.BACK || kc === KEY.EXIT) {
      e.preventDefault();
      if (down) {
        if (!this.backDownAt) this.backDownAt = Date.now();
        return;
      }
      var held = Date.now() - this.backDownAt;
      this.backDownAt = 0;
      if (held >= LONG_PRESS_MS || kc === KEY.EXIT) {
        if (this.onMenu) this.onMenu();
      } else if (this.remotePad) {
        this.tapPad(0x2000);  // B
      } else {
        this.tapKey(0x1B);    // Escape
      }
      return;
    }
    if (kc === KEY.RED) {
      e.preventDefault();
      if (!down && this.onMenu) this.onMenu();
      return;
    }
    if (kc === KEY.GREEN) {
      e.preventDefault();
      if (!down && this.onStats) this.onStats();
      return;
    }
    if (kc === KEY.BLUE) {
      e.preventDefault();
      if (!down) {
        this.remotePad = !this.remotePad;
        ML.app.toast(this.remotePad ? 'Kumanda: gamepad modu' : 'Kumanda: klavye modu');
      }
      return;
    }

    if (this.remotePad && REMOTE_TO_PAD[kc] && this.isRemoteKey(e)) {
      e.preventDefault();
      var flag = REMOTE_TO_PAD[kc];
      this.remoteButtons = down ? (this.remoteButtons | flag) : (this.remoteButtons & ~flag);
      this.sendPad(3, this.remoteButtons, 0, 0, 0, 0, 0, 0);
      return;
    }

    var vk = kc;
    if (this.isRemoteKey(e)) {
      vk = REMOTE_TO_VK[kc];
      if (!vk) return;
    }
    if (e.repeat && down) { e.preventDefault(); return; }
    e.preventDefault();
    var mods = (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0);
    this.send({ t: 'k', c: vk, d: down ? 1 : 0, m: mods });
  };

  Input.prototype.tapKey = function (vk) {
    var self = this;
    this.send({ t: 'k', c: vk, d: 1, m: 0 });
    setTimeout(function () { self.send({ t: 'k', c: vk, d: 0, m: 0 }); }, 50);
  };

  Input.prototype.tapPad = function (flag) {
    var self = this;
    this.sendPad(3, this.remoteButtons | flag, 0, 0, 0, 0, 0, 0);
    setTimeout(function () { self.sendPad(3, self.remoteButtons, 0, 0, 0, 0, 0, 0); }, 80);
  };

  Input.prototype.activeMask = function () {
    var mask = this.remotePad ? 0x8 : 0;
    for (var i in this.pads) mask |= 1 << Number(i);
    return mask || 1;
  };

  Input.prototype.sendPad = function (i, b, lt, rt, lx, ly, rx, ry) {
    this.send({ t: 'g', i: i, mask: this.activeMask(), b: b, lt: lt, rt: rt, lx: lx, ly: ly, rx: rx, ry: ry });
  };

  /* ---------- mouse ---------- */

  Input.prototype.videoRect = function () {
    var v = this.video;
    var r = v.getBoundingClientRect();
    var vw = v.videoWidth || 16, vh = v.videoHeight || 9;
    var scale = Math.min(r.width / vw, r.height / vh);
    var w = vw * scale, h = vh * scale;
    return { x: r.left + (r.width - w) / 2, y: r.top + (r.height - h) / 2, w: w, h: h };
  };

  Input.prototype.mouseMove = function (e) {
    if (!this.active) return;
    var now = Date.now();
    if (now - this.lastMouse < 4) return;  // ~250 Hz is plenty
    this.lastMouse = now;
    var r = this.videoRect();
    var x = Math.round(Math.max(0, Math.min(r.w, e.clientX - r.x)));
    var y = Math.round(Math.max(0, Math.min(r.h, e.clientY - r.y)));
    this.send({ t: 'mp', x: x, y: y, w: Math.round(r.w), h: Math.round(r.h) });
  };

  Input.prototype.mouseButton = function (e, down) {
    if (!this.active) return;
    e.preventDefault();
    var b = e.button === 1 ? 2 : e.button === 2 ? 3 : e.button === 3 ? 4 : e.button === 4 ? 5 : 1;
    this.send({ t: 'mb', b: b, d: down ? 1 : 0 });
  };

  Input.prototype.wheel = function (e) {
    if (!this.active) return;
    e.preventDefault();
    var dy = e.deltaMode === 1 ? e.deltaY * 40 : e.deltaY;
    var v = Math.round(-dy * 120 / 100);
    if (v) this.send({ t: 'ms', v: Math.max(-32768, Math.min(32767, v)) });
  };

  /* ---------- gamepads ---------- */

  function axis(v) {
    if (Math.abs(v) < 0.08) return 0;
    return Math.max(-32767, Math.min(32767, Math.round(v * 32767)));
  }

  Input.prototype.pollPads = function () {
    if (!navigator.getGamepads) return;
    var list = navigator.getGamepads();
    var seen = {};
    for (var n = 0; n < list.length; n++) {
      var gp = list[n];
      if (!gp || !gp.connected || gp.index > 3) continue;
      var i = gp.index;
      seen[i] = true;
      var b = 0;
      for (var k = 0; k < PAD_BUTTONS.length && k < gp.buttons.length; k++) {
        if (PAD_BUTTONS[k] && gp.buttons[k].pressed) b |= PAD_BUTTONS[k];
      }
      var lt = gp.buttons[6] ? Math.round(gp.buttons[6].value * 255) : 0;
      var rt = gp.buttons[7] ? Math.round(gp.buttons[7].value * 255) : 0;
      var ax = gp.axes;
      var st = [b, lt, rt, axis(ax[0] || 0), -axis(ax[1] || 0), axis(ax[2] || 0), -axis(ax[3] || 0)];
      var key = st.join(',');
      if (!this.pads[i]) {
        this.pads[i] = { key: '' };
        this.send({ t: 'ga', i: i, mask: this.activeMask() });
      }
      if (this.pads[i].key !== key) {
        this.pads[i].key = key;
        this.sendPad(i, st[0], st[1], st[2], st[3], st[4], st[5], st[6]);
      }
    }
    for (var j in this.pads) {
      if (!seen[j]) {
        delete this.pads[j];
        this.sendPad(Number(j), 0, 0, 0, 0, 0, 0, 0);
      }
    }
  };

  ML.Input = Input;
})(window.ML = window.ML || {});
