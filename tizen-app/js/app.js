/*
 * Moonlight T4 - main UI and bridge connection.
 * Written in conservative ES2015 for the Tizen 4.0 (Chromium 56 based) web engine.
 */
(function (ML) {
  'use strict';

  var KEY = ML.KEY;
  var BRIDGE_PORT = 8090;
  var MSG_VIDEO_INIT = 1, MSG_VIDEO_FRAG = 2, MSG_AUDIO_PCM = 3;

  var RESOLUTIONS = [[1280, 720], [1920, 1080], [2560, 1440], [3840, 2160]];
  var FPS = [30, 60];
  var BITRATES = [5000, 10000, 15000, 20000, 30000, 40000, 50000];

  var $ = function (id) { return document.getElementById(id); };

  ML.log = function (msg) {
    if (window.console) console.log('[ML] ' + msg);
  };

  /* ---------- settings ---------- */

  var settings = { bridge: '', res: 1, fps: 1, bitrate: 3, hostAudio: false, stats: false };
  try {
    var saved = JSON.parse(localStorage.getItem('ml.settings') || '{}');
    for (var k in saved) if (saved.hasOwnProperty(k)) settings[k] = saved[k];
  } catch (e) { /* defaults */ }
  function saveSettings() {
    try { localStorage.setItem('ml.settings', JSON.stringify(settings)); } catch (e) { /* ignore */ }
  }

  var SETTING_VIEW = {
    res: function () { var r = RESOLUTIONS[settings.res]; return r[1] + 'p (' + r[0] + 'x' + r[1] + ')'; },
    fps: function () { return FPS[settings.fps] + ' FPS'; },
    bitrate: function () { return (BITRATES[settings.bitrate] / 1000) + ' Mbps'; },
    hostAudio: function () { return settings.hostAudio ? 'Açık' : 'Kapalı'; },
    stats: function () { return settings.stats ? 'Açık' : 'Kapalı'; }
  };
  var SETTING_NEXT = {
    res: function () { settings.res = (settings.res + 1) % RESOLUTIONS.length; },
    fps: function () { settings.fps = (settings.fps + 1) % FPS.length; },
    bitrate: function () { settings.bitrate = (settings.bitrate + 1) % BITRATES.length; },
    hostAudio: function () { settings.hostAudio = !settings.hostAudio; },
    stats: function () { settings.stats = !settings.stats; }
  };

  /* ---------- screens & focus ---------- */

  var current = null;
  var focusStack = [];   // overlays (menus/dialogs) that capture focus

  function show(name) {
    var screens = document.querySelectorAll('.screen');
    for (var i = 0; i < screens.length; i++) screens[i].classList.remove('active');
    $('screen-' + name).classList.add('active');
    current = name;
    focusFirst();
  }

  function focusScope() {
    return focusStack.length ? focusStack[focusStack.length - 1] : $('screen-' + current);
  }

  function focusables() {
    var all = focusScope().querySelectorAll('.focusable');
    var out = [];
    for (var i = 0; i < all.length; i++) {
      if (all[i].offsetParent !== null) out.push(all[i]);
    }
    return out;
  }

  function focusFirst() {
    var f = focusables();
    if (f.length) f[0].focus();
  }

  // Spatial navigation: pick the nearest focusable in the pressed direction.
  function moveFocus(dx, dy) {
    var list = focusables();
    var cur = document.activeElement;
    if (list.indexOf(cur) < 0) { focusFirst(); return; }
    var a = cur.getBoundingClientRect();
    var ax = a.left + a.width / 2, ay = a.top + a.height / 2;
    var best = null, bestScore = Infinity;
    for (var i = 0; i < list.length; i++) {
      var el = list[i];
      if (el === cur) continue;
      var b = el.getBoundingClientRect();
      var bx = b.left + b.width / 2, by = b.top + b.height / 2;
      var ddx = bx - ax, ddy = by - ay;
      var along = dx ? ddx * dx : ddy * dy;
      if (along <= 1) continue;
      var across = dx ? Math.abs(ddy) : Math.abs(ddx);
      var score = along + across * 2;
      if (score < bestScore) { bestScore = score; best = el; }
    }
    if (best) best.focus();
  }

  function pushOverlay(el) {
    el.classList.remove('hidden');
    focusStack.push(el);
    focusFirst();
  }

  function popOverlay(el) {
    el.classList.add('hidden');
    var i = focusStack.indexOf(el);
    if (i >= 0) focusStack.splice(i, 1);
    if (document.activeElement) document.activeElement.blur();
    if (focusStack.length || current !== 'stream') focusFirst();
  }

  /* ---------- toast / error ---------- */

  var toastTimer, errorTimer;
  function toast(msg) {
    var t = $('toast');
    t.textContent = msg;
    t.classList.remove('hidden');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { t.classList.add('hidden'); }, 2500);
  }
  function showError(msg) {
    ML.log('error: ' + msg);
    var e = $('error');
    e.textContent = msg;
    e.classList.remove('hidden');
    clearTimeout(errorTimer);
    errorTimer = setTimeout(function () { e.classList.add('hidden'); }, 7000);
  }

  /* ---------- bridge connection ---------- */

  var ws = null;
  var reconnectTimer = null;
  var host = { name: '', paired: false, currentGame: 0 };
  var streaming = false;
  var streamStarted = false;
  var lastApps = [];
  var rtt = 0;

  function send(obj) {
    if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj));
  }

  function connect() {
    clearTimeout(reconnectTimer);
    if (!settings.bridge) { show('connect'); return; }
    if (ws) { ws.onclose = null; ws.close(); }
    $('connect-status').textContent = settings.bridge + ' adresine bağlanılıyor…';
    var sock;
    try {
      sock = new WebSocket('ws://' + settings.bridge + ':' + BRIDGE_PORT);
    } catch (e) {
      $('connect-status').textContent = 'Geçersiz adres';
      show('connect');
      return;
    }
    sock.binaryType = 'arraybuffer';
    ws = sock;
    sock.onopen = function () {
      $('connect-status').textContent = 'Bağlandı, bilgisayar bilgisi alınıyor…';
      send({ t: 'hello' });
    };
    sock.onmessage = function (e) {
      if (typeof e.data === 'string') handleText(JSON.parse(e.data));
      else handleBinary(new Uint8Array(e.data));
    };
    sock.onclose = function () {
      if (ws !== sock) return;
      ws = null;
      if (streaming) endStream('Köprü bağlantısı koptu');
      if (current !== 'connect') show('connect');
      $('connect-status').textContent = 'Köprüye bağlanılamadı (' + settings.bridge + ':' + BRIDGE_PORT +
        '). Köprü programı çalışıyor mu? Tekrar deneniyor…';
      reconnectTimer = setTimeout(connect, 3000);
    };
  }

  function handleText(m) {
    switch (m.t) {
      case 'host':
        host = m;
        $('pair-host').textContent = m.name || 'Bilgisayar';
        $('apps-host').textContent = m.name || 'Bilgisayar';
        if (streaming) break;
        if (!m.reachable) {
          show('connect');
          $('connect-status').textContent = 'Köprü çalışıyor ama Sunshine\'a ulaşılamadı. Sunshine açık mı?';
          reconnectTimer = setTimeout(function () { send({ t: 'hello' }); }, 5000);
        } else if (!m.paired) {
          $('pin-box').classList.add('hidden');
          $('btn-pair').classList.remove('hidden');
          show('pair');
        } else {
          if (current !== 'apps' && current !== 'settings') show('apps');
          send({ t: 'apps' });
        }
        break;
      case 'pin':
        $('pin').textContent = m.pin;
        $('pin-box').classList.remove('hidden');
        $('btn-pair').classList.add('hidden');
        focusFirst();
        break;
      case 'paired':
        if (m.ok) toast('Eşleştirme başarılı');
        else showError('Eşleştirme başarısız: ' + (m.msg || 'bilinmeyen hata'));
        break;
      case 'apps':
        host.currentGame = m.current;
        renderApps(m.apps);
        break;
      case 'video':
        if (!ML.Player.supports(m.codec)) {
          showError('TV bu video biçimini desteklemiyor: ' + m.codec + '. Çözünürlüğü düşürmeyi deneyin.');
        }
        player.setCodec(m.codec);
        break;
      case 'audio':
        audio.configure(m.rate, m.channels);
        break;
      case 'stage':
        if (!streamStarted) $('stream-status').textContent = 'Bağlanıyor: ' + m.name;
        break;
      case 'started':
        streamStarted = true;
        $('stream-status').textContent = 'Görüntü bekleniyor…';
        break;
      case 'error':
        showError(m.msg);
        if (streaming && !streamStarted) endStream();
        break;
      case 'ended':
        if (streaming) endStream(m.code === 0 ? 'Yayın sona erdi' : 'Bağlantı koptu (hata ' + m.code + ')');
        break;
      case 'stopped':
        if (streaming) endStream();
        break;
      case 'net':
        if (m.poor) toast('Ağ bağlantısı zayıf');
        break;
      case 'pong':
        rtt = Date.now() - m.ts;
        break;
      case 'rumble':
        rumble(m);
        break;
    }
  }

  function handleBinary(b) {
    var payload = b.subarray(1);
    switch (b[0]) {
      case MSG_VIDEO_INIT: player.pushInit(payload); break;
      case MSG_VIDEO_FRAG: player.pushFragment(payload); break;
      case MSG_AUDIO_PCM: audio.push(payload); break;
    }
  }

  /* ---------- apps ---------- */

  function renderApps(apps) {
    lastApps = apps || [];
    var grid = $('app-grid');
    var focusedId = document.activeElement && document.activeElement.getAttribute('data-app');
    grid.innerHTML = '';
    lastApps.forEach(function (a) {
      var b = document.createElement('button');
      b.className = 'focusable app-tile';
      b.setAttribute('data-app', a.id);
      b.textContent = a.title;
      if (a.id === host.currentGame) {
        var r = document.createElement('span');
        r.className = 'running';
        r.textContent = '● Çalışıyor';
        b.appendChild(r);
      }
      b.onclick = function () { chooseApp(a); };
      grid.appendChild(b);
    });
    $('apps-status').textContent = lastApps.length ? '' : 'Uygulama bulunamadı. Sunshine\'da uygulama ekleyin.';
    if (current === 'apps' && !focusStack.length) {
      var again = focusedId && grid.querySelector('[data-app="' + focusedId + '"]');
      if (again) again.focus(); else focusFirst();
    }
  }

  function dialog(title, buttons) {
    var d = $('dialog');
    $('dialog-title').textContent = title;
    var box = $('dialog-buttons');
    box.innerHTML = '';
    buttons.forEach(function (bt) {
      var b = document.createElement('button');
      b.className = 'focusable' + (bt.danger ? ' danger' : '');
      b.textContent = bt.label;
      b.onclick = function () { popOverlay(d); if (bt.action) bt.action(); };
      box.appendChild(b);
    });
    pushOverlay(d);
  }

  function chooseApp(app) {
    var running = host.currentGame;
    if (running && running !== app.id) {
      var name = 'Başka bir uygulama';
      lastApps.forEach(function (a) { if (a.id === running) name = a.title; });
      dialog(name + ' şu an çalışıyor', [
        { label: name + ' uygulamasına dön', action: function () { startStream(running); } },
        { label: 'Kapat ve ' + app.title + ' başlat', danger: true, action: function () {
          send({ t: 'quit' });
          host.currentGame = 0;
          setTimeout(function () { startStream(app.id); }, 1500);
        } },
        { label: 'İptal' }
      ]);
      return;
    }
    startStream(app.id);
  }

  /* ---------- streaming ---------- */

  var video = $('video');
  var player = new ML.Player(video, function () { send({ t: 'idr' }); });
  var audio = new ML.Audio();
  var input = new ML.Input(send, video);
  var statsTimer = null, pingTimer = null;
  var lastStats = { frames: 0, bytes: 0, t: Date.now() };

  input.onMenu = function () { pushOverlay($('stream-menu')); input.stop(); };
  input.onStats = toggleStats;
  ML.app = { toast: toast, showError: showError };

  function startStream(appId) {
    var r = RESOLUTIONS[settings.res];
    streaming = true;
    streamStarted = false;
    $('stream-status').textContent = 'Başlatılıyor…';
    $('stream-status').classList.remove('hidden');
    show('stream');
    if (document.activeElement) document.activeElement.blur();
    audio.resume();
    $('stats').classList.toggle('hidden', !settings.stats);
    send({
      t: 'start', app: appId, w: r[0], h: r[1], fps: FPS[settings.fps],
      bitrate: BITRATES[settings.bitrate], hostAudio: settings.hostAudio ? 1 : 0, sops: 1
    });
    input.start();
    lastStats = { frames: player.stats.frames, bytes: player.stats.bytes, t: Date.now() };
    statsTimer = setInterval(updateStats, 1000);
    pingTimer = setInterval(function () { send({ t: 'ping', ts: Date.now() }); }, 2000);
  }

  function endStream(msg) {
    if (!streaming) return;
    streaming = false;
    input.stop();
    clearInterval(statsTimer);
    clearInterval(pingTimer);
    player.reset();
    audio.stop();
    var menu = $('stream-menu');
    if (!menu.classList.contains('hidden')) popOverlay(menu);
    if (msg) toast(msg);
    if (ws) {
      show('apps');
      send({ t: 'hello' });
    }
  }

  video.addEventListener('playing', function () {
    $('stream-status').classList.add('hidden');
  });

  function toggleStats() {
    $('stats').classList.toggle('hidden');
  }

  function updateStats() {
    var now = Date.now();
    var s = player.stats;
    var dt = (now - lastStats.t) / 1000;
    var fps = (s.frames - lastStats.frames) / dt;
    var mbps = (s.bytes - lastStats.bytes) * 8 / dt / 1e6;
    lastStats = { frames: s.frames, bytes: s.bytes, t: now };
    var dropped = video.getVideoPlaybackQuality ? video.getVideoPlaybackQuality().droppedVideoFrames :
      (video.webkitDroppedFrameCount || 0);
    $('stats').textContent =
      'Video     ' + (video.videoWidth || '?') + 'x' + (video.videoHeight || '?') + '  ' + (player.codec || '') + '\n' +
      'Kare/sn   ' + fps.toFixed(1) + '\n' +
      'Bit hızı  ' + mbps.toFixed(1) + ' Mbps\n' +
      'Tampon    ' + Math.round(s.lag * 1000) + ' ms\n' +
      'Atlama    ' + s.jumps + '   Yeniden eşitleme ' + s.resyncs + '\n' +
      'Düşen kare ' + dropped + '\n' +
      'Ses tampon ' + Math.round(audio.stats.delay * 1000) + ' ms  (atlanan ' + audio.stats.drops + ')\n' +
      'Köprü RTT ' + rtt + ' ms';
  }

  function rumble(m) {
    var pads = navigator.getGamepads ? navigator.getGamepads() : [];
    var gp = pads[m.i];
    if (gp && gp.vibrationActuator && gp.vibrationActuator.playEffect) {
      gp.vibrationActuator.playEffect('dual-rumble', {
        duration: 1000, strongMagnitude: m.lo / 65535, weakMagnitude: m.hi / 65535
      });
    }
  }

  /* ---------- buttons ---------- */

  $('btn-connect').onclick = function () {
    var ip = $('bridge-ip').value.trim();
    if (!ip) { showError('Lütfen bir IP adresi girin'); return; }
    settings.bridge = ip;
    saveSettings();
    connect();
  };
  $('btn-pair').onclick = function () { send({ t: 'pair' }); };
  function changeBridge() {
    clearTimeout(reconnectTimer);
    if (ws) { ws.onclose = null; ws.close(); ws = null; }
    $('bridge-ip').value = settings.bridge;
    $('connect-status').textContent = '';
    show('connect');
  }
  $('btn-change-bridge-pair').onclick = changeBridge;
  $('btn-change-bridge').onclick = changeBridge;
  $('btn-refresh').onclick = function () { send({ t: 'hello' }); };
  $('btn-settings').onclick = function () { renderSettings(); show('settings'); };
  $('btn-settings-back').onclick = function () { show('apps'); };
  $('btn-resume').onclick = function () { popOverlay($('stream-menu')); input.start(); };
  $('btn-toggle-stats').onclick = function () { toggleStats(); };
  $('btn-disconnect').onclick = function () { send({ t: 'stop' }); endStream(); };
  $('btn-quit').onclick = function () { send({ t: 'quit' }); endStream('Oyun kapatılıyor…'); };

  function renderSettings() {
    var items = document.querySelectorAll('.setting');
    for (var i = 0; i < items.length; i++) {
      var key = items[i].getAttribute('data-key');
      items[i].querySelector('span').textContent = SETTING_VIEW[key]();
    }
  }
  (function () {
    var items = document.querySelectorAll('.setting');
    for (var i = 0; i < items.length; i++) {
      items[i].onclick = function () {
        SETTING_NEXT[this.getAttribute('data-key')]();
        saveSettings();
        renderSettings();
      };
    }
  })();

  /* ---------- keys (menus) ---------- */

  function exitApp() {
    try { tizen.application.getCurrentApplication().exit(); } catch (e) { /* not on a TV */ }
  }

  document.addEventListener('keydown', function (e) {
    if (input.active) return;  // streaming: Input handles everything
    var kc = e.keyCode;
    var inText = document.activeElement && document.activeElement.tagName === 'INPUT';
    switch (kc) {
      case KEY.LEFT: if (inText) return; moveFocus(-1, 0); break;
      case KEY.RIGHT: if (inText) return; moveFocus(1, 0); break;
      case KEY.UP: moveFocus(0, -1); break;
      case KEY.DOWN: moveFocus(0, 1); break;
      case KEY.ENTER:
        if (inText) { $('btn-connect').focus(); return; }
        if (document.activeElement && document.activeElement.click) document.activeElement.click();
        break;
      case KEY.BACK:
      case 8:  // Backspace on a keyboard
        if (inText && kc === 8) return;
        if (focusStack.length) {
          var top = focusStack[focusStack.length - 1];
          popOverlay(top);
          if (top === $('stream-menu')) input.start();
        } else if (current === 'settings') {
          show('apps');
        } else if (current === 'stream') {
          pushOverlay($('stream-menu'));
        } else {
          exitApp();
        }
        break;
      default:
        return;
    }
    e.preventDefault();
  });

  /* ---------- startup ---------- */

  function registerKeys() {
    if (!window.tizen || !tizen.tvinputdevice) return;
    ['ColorF0Red', 'ColorF1Green', 'ColorF2Yellow', 'ColorF3Blue',
     'MediaPlayPause', 'MediaPlay', 'MediaPause'].forEach(function (k) {
      try { tizen.tvinputdevice.registerKey(k); } catch (e) { ML.log('registerKey ' + k + ': ' + e); }
    });
  }

  document.addEventListener('visibilitychange', function () {
    // Leaving the app (Home button) ends the stream; the game keeps running on the PC.
    if (document.hidden && streaming) {
      send({ t: 'stop' });
      endStream();
    }
  });

  registerKeys();
  $('bridge-ip').value = settings.bridge;
  if (settings.bridge) {
    show('connect');
    connect();
  } else {
    show('connect');
  }
})(window.ML = window.ML || {});
