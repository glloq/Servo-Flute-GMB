/***********************************************************************************************
 * web_content.h - Interface web embarquee en PROGMEM
 *
 * Single Page Application avec 3 onglets + overlay settings :
 * 1. Clavier virtuel (dynamique depuis cfg.notes[])
 * 2. Lecteur MIDI (upload + transport)
 * 3. Calibration (3 etapes : Doigts, Doigtes, Souffle)
 * + Overlay Settings (engrenage) : tous les parametres avances
 *
 * Interface responsive / mobile-friendly
 ***********************************************************************************************/
#ifndef WEB_CONTENT_H
#define WEB_CONTENT_H

const char WEB_HTML_CONTENT[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>ServoFlute</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;
background:#1a1a2e;color:#e0e0e0;overflow-x:hidden;min-height:100vh;padding-bottom:36px}
.hdr{background:#16213e;padding:10px 16px;display:flex;justify-content:space-between;
align-items:center;border-bottom:2px solid #0f3460;position:sticky;top:0;z-index:10}
.hdr h1{font-size:1.1em;color:#e94560;display:flex;align-items:center}
.hdr-c{display:flex;align-items:center;gap:2px;font-size:.85em;font-weight:700;color:#8aa;user-select:none}
.hdr-c svg{vertical-align:middle}
.hdr-r{display:flex;align-items:center;gap:12px}
.dot{width:10px;height:10px;border-radius:50%;display:inline-block}
.dot.on{background:#4ecca3;box-shadow:0 0 6px #4ecca3}.dot.off{background:#777}
.unsaved-badge{display:none;background:#e9a645;color:#1a1a2e;font-size:.6em;padding:2px 6px;
border-radius:8px;font-weight:bold;margin-left:8px;vertical-align:middle}
.unsaved-badge.show{display:inline-block}
.gear-btn{background:none;border:none;color:#9aa;font-size:1.3em;cursor:pointer;padding:4px;
display:flex;align-items:center}
.gear-btn:hover{color:#e94560}
.tabs{display:flex;background:#16213e;border-bottom:1px solid #0f3460;overflow-x:auto}
.tabs button{flex:1;background:none;border:none;color:#9aa;padding:12px 8px;font-size:0.85em;
cursor:pointer;border-bottom:2px solid transparent;white-space:nowrap;min-width:80px;
display:flex;align-items:center;justify-content:center;gap:6px}
.tabs button.active{color:#e94560;border-bottom-color:#e94560}
.tabs button svg{width:14px;height:14px;flex-shrink:0}
.tab{display:none;padding:12px}.tab.active{display:block}
.section{background:#16213e;border-radius:8px;padding:12px;margin-bottom:12px;border:1px solid #0f3460}
.section h3{color:#e94560;font-size:0.9em;margin-bottom:8px}
.btn{padding:8px 16px;border:none;border-radius:6px;cursor:pointer;font-size:0.85em;
position:relative;transition:all .2s;display:inline-flex;align-items:center;gap:6px}
.btn-p{background:#e94560;color:#fff}.btn-p:hover{background:#d63650}
.btn-s{background:#0f3460;color:#e0e0e0;border:1px solid #1a4080}.btn-s:hover{background:#1a4080}
.btn-s.active,.expr-mode.active{background:#4ecca3;color:#1a1a2e;border-color:#4ecca3}
.btn-g{background:#4ecca3;color:#1a1a2e}.btn-g:hover{background:#3db892}
.btn:disabled{opacity:.4;cursor:default}
.btn-row{display:flex;gap:8px;margin-top:10px;flex-wrap:wrap}
.btn svg{width:14px;height:14px;flex-shrink:0}
.btn.loading{color:transparent !important;pointer-events:none}
.btn.loading::after{content:'';position:absolute;width:14px;height:14px;border:2px solid transparent;
border-top-color:#fff;border-radius:50%;animation:spin .6s linear infinite;
top:50%;left:50%;margin:-7px 0 0 -7px}
@keyframes spin{to{transform:rotate(360deg)}}
@keyframes testPulse{0%{box-shadow:0 0 0 0 rgba(78,204,163,.7)}70%{box-shadow:0 0 0 10px rgba(78,204,163,0)}100%{box-shadow:0 0 0 0 rgba(78,204,163,0)}}
.test-pulse{animation:testPulse .6s ease}
input[type=range]{width:100%;accent-color:#e94560}
input[type=number],input[type=text],input[type=password],select{background:#0d1b3e;border:1px solid #1a4080;
color:#e0e0e0;padding:6px 8px;border-radius:4px;font-size:0.85em;width:100%}
.cfg-row{display:flex;align-items:center;gap:8px;margin-bottom:6px}
.cfg-row label{flex:0 0 140px;font-size:0.8em;color:#aaa;text-align:right}
.cfg-row input,.cfg-row select{flex:1}
.keys{display:flex;flex-wrap:wrap;gap:6px;justify-content:center;padding:8px 0}
.key{background:linear-gradient(180deg,#2a2a4a,#1a1a2e);border:1px solid #0f3460;
border-radius:6px;padding:10px 8px;text-align:center;cursor:pointer;user-select:none;
-webkit-user-select:none;touch-action:manipulation;min-width:60px;flex:0 0 auto;transition:all .15s}
.key.black{background:linear-gradient(180deg,#1a1a2e,#0a0a1e);border-color:#444}
.key.pressed,.key:active{background:#e94560;border-color:#e94560;transform:scale(.96)}
.piano-keys{position:relative;display:flex;padding:8px 0;justify-content:center}
.pkey{position:relative;display:flex;align-items:flex-end;justify-content:center;
cursor:pointer;user-select:none;-webkit-user-select:none;touch-action:manipulation;transition:all .12s;box-sizing:border-box}
.pkey.white{background:linear-gradient(180deg,#f8f8f8,#ddd);border:1px solid #999;
border-radius:0 0 5px 5px;width:44px;height:140px;z-index:1;margin:0 1px}
.pkey.blk{background:linear-gradient(180deg,#333,#111);border:1px solid #000;
border-radius:0 0 3px 3px;width:28px;height:90px;z-index:2;margin:0 -14px}
.pkey.white.pressed{background:linear-gradient(180deg,#e94560,#c03050)}
.pkey.blk.pressed{background:linear-gradient(180deg,#e94560,#a02040)}
.pkey .pkey-label{font-size:.6em;padding-bottom:6px;text-align:center;pointer-events:none}
.pkey.white .pkey-label{color:#555}.pkey.blk .pkey-label{color:#bbb}
.note-name{display:block;font-weight:bold;font-size:1em;color:#fff}
.note-midi{display:block;font-size:0.65em;color:#9aa;margin-top:2px}
.key-shortcut{display:none;font-size:.55em;color:#777;margin-top:2px}
.key:hover .key-shortcut{display:block}
.kf-row{display:flex;gap:3px;justify-content:center;margin-top:4px}
.kf{width:8px;height:8px;border-radius:50%;border:1px solid #777}
.kf.c{background:#444}.kf.o{background:#4ecca3}.kf.h{background:linear-gradient(180deg,#4ecca3 50%,#444 50%)}
.flute-box{background:#0d1b3e;border-radius:8px;padding:12px;text-align:center;margin-bottom:8px}
.flute-box svg{width:100%;max-width:600px;height:auto}
.flute-hole{stroke:#5C4A0A;stroke-width:2;transition:all .2s}
.flute-hole.closed{fill:#3a2a0a}.flute-hole.open{fill:#4ecca3}.flute-hole.half{fill:#4ecca3;fill-opacity:.5}
.flute-hole.thumb{filter:drop-shadow(0 0 3px #e94560)}
.flute-hole:hover{filter:drop-shadow(0 0 6px #e94560);cursor:pointer}
@keyframes holePulse{0%,100%{filter:drop-shadow(0 0 4px #4ecca3)}50%{filter:drop-shadow(0 0 14px #4ecca3)}}
.flute-hole.playing{animation:holePulse 1s ease-in-out infinite}
.flute-lbl{font-size:11px;fill:#9aa}
.flute-num{font-size:10px;fill:#fff;font-weight:bold;pointer-events:none}
.flute-info{text-align:center;font-size:0.8em;color:#9aa;margin-top:4px}
.toast-container{position:fixed;top:56px;right:12px;z-index:200;display:flex;flex-direction:column;
gap:8px;pointer-events:none}
.toast{padding:10px 16px;border-radius:8px;font-size:.82em;color:#fff;opacity:0;
transform:translateX(40px);transition:all .3s ease;display:flex;align-items:center;gap:8px;
max-width:320px;pointer-events:auto;box-shadow:0 4px 16px rgba(0,0,0,.4)}
.toast.show{opacity:1;transform:translateX(0)}
.toast.success{background:rgba(45,107,79,.95);border:1px solid #4ecca3}
.toast.error{background:rgba(107,45,58,.95);border:1px solid #e94560}
.toast.info{background:rgba(45,58,107,.95);border:1px solid #4a7eca}
.toast svg{flex-shrink:0;width:16px;height:16px}
.skeleton{background:linear-gradient(90deg,#16213e 25%,#1e2d50 50%,#16213e 75%);
background-size:200% 100%;animation:shimmer 1.5s infinite;border-radius:6px;min-height:20px}
@keyframes shimmer{0%{background-position:200% 0}100%{background-position:-200% 0}}
@keyframes fadeInUp{from{opacity:0;transform:translateY(12px)}to{opacity:1;transform:translateY(0)}}
.fade-in{animation:fadeInUp .3s ease forwards}
.fade-delay-1{animation-delay:.05s;opacity:0}.fade-delay-2{animation-delay:.1s;opacity:0}
.fade-delay-3{animation-delay:.15s;opacity:0}.fade-delay-4{animation-delay:.2s;opacity:0}
.steps{display:flex;align-items:center;justify-content:center;gap:0;padding:12px 0}
.step-dot{width:24px;height:24px;border-radius:50%;background:#444;cursor:pointer;transition:.2s;border:2px solid transparent;
display:flex;align-items:center;justify-content:center;font-size:.65em;font-weight:bold;color:rgba(255,255,255,.6)}
.step-dot.active{background:#e94560;box-shadow:0 0 10px #e94560;border-color:#e94560}
.step-dot.active.modified{background:#e9a645;box-shadow:0 0 10px #e9a645;border-color:#e9a645}
.step-dot.done{background:#4ecca3;border-color:#4ecca3}
.step-dot.modified{background:#e9a645;border-color:#e9a645;box-shadow:0 0 6px rgba(233,166,69,.5)}
.step-dot.locked{opacity:.4;cursor:not-allowed}
.step-line{width:40px;height:2px;background:#444}
.step-labels{display:flex;justify-content:center;gap:22px;font-size:0.75em;color:#9aa;margin-bottom:8px}
.cal-card{background:linear-gradient(135deg,#0d1b3e 0%,#101f45 100%);border:1px solid #1a4080;
border-radius:8px;padding:10px;margin-bottom:8px}
.cal-card h4{font-size:0.85em;color:#e94560;margin-bottom:6px}
.cal-card.pca-conflict{border-color:#e9a645;box-shadow:0 0 8px rgba(233,166,69,.3)}
.pca-warn{color:#e9a645;font-size:.75em;margin-top:4px;display:none}
.pca-conflict .pca-warn{display:block}
.fg-row{display:flex;align-items:center;gap:8px;padding:6px 4px;border-bottom:1px solid #0f3460}
.fg-row:last-child{border-bottom:none}
.fg-note{font-weight:bold;min-width:50px;font-size:0.9em}
.fg-midi{color:#9aa;font-size:0.75em;min-width:36px}
.fg-octave{background:#0f3460;padding:4px 10px;font-size:.75em;color:#e94560;
font-weight:bold;border-radius:4px;margin:6px 0}
.fg-dots{display:flex;gap:4px;flex:1}
.fg-dot{width:18px;height:18px;border-radius:50%;border:2px solid #777;cursor:pointer;transition:.15s}
.fg-dot.closed{background:#444}.fg-dot.open{background:#4ecca3;border-color:#4ecca3}.fg-dot.half{background:linear-gradient(180deg,#4ecca3 50%,#444 50%);border-color:#4ecca3}
.fg-dot.thumb{border-style:dashed;border-color:#e94560}
.air-card{display:flex;align-items:center;gap:8px;padding:6px 0;border-bottom:1px solid #0f3460;flex-wrap:wrap}
.air-note{font-weight:bold;min-width:40px;font-size:0.85em}
.air-sliders{flex:1;min-width:150px}
.air-vals{font-size:0.75em;color:#9aa;display:flex;justify-content:space-between}
.dual-range{position:relative;height:28px;margin:4px 0}
.dual-range-track{position:absolute;top:12px;left:0;right:0;height:4px;background:#0f3460;border-radius:2px}
.dual-range-fill{position:absolute;top:12px;height:4px;background:#e94560;border-radius:2px}
.dual-range input[type=range]{position:absolute;top:0;width:100%;margin:0;pointer-events:none;
-webkit-appearance:none;appearance:none;background:transparent;height:28px}
.dual-range input[type=range]::-webkit-slider-thumb{pointer-events:all;-webkit-appearance:none;
width:16px;height:16px;background:#e94560;border-radius:50%;cursor:pointer;border:2px solid #fff}
.dual-range input[type=range]::-moz-range-thumb{pointer-events:all;width:16px;height:16px;
background:#e94560;border-radius:50%;cursor:pointer;border:2px solid #fff}
.undo-bar{display:flex;gap:6px;align-items:center;margin-bottom:8px}
.undo-bar button{padding:4px 10px;font-size:.8em}.undo-bar span{font-size:.75em;color:#777}
.settings-overlay{display:none;position:fixed;top:0;left:0;right:0;bottom:0;
background:rgba(0,0,0,.85);z-index:100;overflow-y:auto}
.settings-overlay.open{display:block}
.wiz-card{display:flex;align-items:center;gap:10px;padding:10px 14px;background:#16213e;border:2px solid #0f3460;border-radius:8px;cursor:pointer;transition:border-color .2s}
.wiz-card:has(input:checked){border-color:#e94560;background:#1a2744}
.wiz-card input{accent-color:#e94560}
.settings-box{max-width:600px;margin:0 auto;padding:16px;padding-bottom:48px}
.settings-box h2{color:#e94560;margin-bottom:12px;display:flex;justify-content:space-between;align-items:center}
.close-btn{background:none;border:none;color:#9aa;font-size:1.5em;cursor:pointer}
.close-btn:hover{color:#e94560}
.drop-zone{border:2px dashed #0f3460;border-radius:8px;padding:30px;text-align:center;
color:#777;cursor:pointer;transition:border-color .2s}
.drop-zone.hover{border-color:#e94560;color:#e94560}
.transport{display:flex;gap:8px;justify-content:center;align-items:center;margin:12px 0}
.transport button{width:44px;height:44px;border-radius:50%;font-size:1.2em;
display:flex;align-items:center;justify-content:center}
.progress-bar{height:6px;background:#0f3460;border-radius:3px;overflow:hidden;margin:8px 0}
.progress-fill{height:100%;background:#e94560;width:0%;transition:width .3s}
.file-info{font-size:0.8em;color:#9aa;text-align:center}
.upload-bar{height:4px;background:#0f3460;border-radius:2px;overflow:hidden;margin-top:8px;display:none}
.upload-fill{height:100%;background:#4ecca3;width:0%;transition:width .15s}
.cc-bar{display:flex;align-items:center;gap:8px;margin-bottom:6px;font-size:0.8em}
.cc-label{min-width:70px;color:#9aa}.cc-val{min-width:24px;text-align:right}
.cc-track{flex:1;height:6px;background:#0f3460;border-radius:3px;overflow:hidden}
.cc-fill{height:100%;background:#4ecca3;transition:width .2s}
.vu{display:flex;align-items:center;gap:8px;margin:8px 0}
.vu-track{flex:1;height:10px;background:#0f3460;border-radius:5px;overflow:hidden}
.vu-fill{height:100%;background:#4ecca3;width:0%;transition:width .1s}
.vu-val{font-size:0.8em;min-width:36px;text-align:right}
.mic-badge{display:inline-block;background:#4ecca3;color:#1a1a2e;font-size:0.7em;
padding:2px 8px;border-radius:10px;font-weight:bold}
.mic-badge.off{background:#777;color:#9aa}
.pitch{display:flex;gap:16px;align-items:center;font-size:0.9em}
.pitch-note{font-size:1.4em;font-weight:bold;color:#e94560;min-width:50px}
.pitch-hz{color:#9aa;font-size:0.85em}
.pitch-cents{font-size:0.85em}.pitch-cents.ok{color:#4ecca3}.pitch-cents.sharp{color:#e94560}.pitch-cents.flat{color:#e9a645}
.acal-progress{background:#0d1b3e;border-radius:8px;padding:12px;display:none}
.acal-bar{height:8px;background:#0f3460;border-radius:4px;overflow:hidden;margin:6px 0}
.acal-fill{height:100%;background:#e94560;width:0%;transition:width .3s}
.acal-info{font-size:0.8em;color:#9aa;display:flex;justify-content:space-between}
.wifi-item{padding:8px;border-bottom:1px solid #0f3460;cursor:pointer;display:flex;justify-content:space-between}
.wifi-item:hover{background:#0f3460}
.status-bar{background:#0d1117;padding:6px 16px;font-size:0.75em;color:#777;
display:flex;justify-content:space-between;position:fixed;bottom:0;left:0;right:0;z-index:5}
.log{background:#0a0a1a;border-radius:4px;padding:8px;font-family:monospace;font-size:0.75em;
max-height:120px;overflow-y:auto;color:#9aa}
.keys,.piano-keys{-webkit-user-select:none;user-select:none;touch-action:manipulation}
.modal-overlay{position:fixed;inset:0;background:rgba(0,0,0,.7);z-index:100;display:none;align-items:center;justify-content:center}
.modal-overlay.show{display:flex}
.modal{background:#16213e;border-radius:12px;width:95vw;max-width:700px;max-height:90vh;overflow-y:auto;
box-shadow:0 8px 32px rgba(0,0,0,.6);border:1px solid #1a4080}
.modal-hdr{display:flex;align-items:center;justify-content:space-between;padding:12px 16px;
border-bottom:1px solid #1a4080;position:sticky;top:0;background:#16213e;z-index:1}
.modal-hdr h3{margin:0;font-size:1em;color:#e0e0e0}
.modal-close{background:none;border:none;color:#888;font-size:1.4em;cursor:pointer;padding:0 4px}
.modal-close:hover{color:#e94560}
.modal-body{padding:16px}
.pump-toggle{cursor:pointer;opacity:.9;transition:opacity .15s}
.pump-toggle:hover{opacity:1}
.pump-off{opacity:.4}
</style>
</head>
<body>
<div class="toast-container" id="toastContainer"></div>
<div class="hdr">
  <h1 id="devName">ServoFlute<span class="unsaved-badge" id="unsavedBadge">modifi&eacute;</span></h1>
  <div class="hdr-c" onclick="openSeqModal()" style="cursor:pointer" title="Editeur de sequences"><span style="color:#e94560">B</span><svg viewBox="0 0 28 28" width="22" height="22"><circle cx="14" cy="14" r="12" fill="none" stroke="#8aa" stroke-width="1.5"/><text x="14" y="19" text-anchor="middle" fill="#e94560" font-size="18" font-weight="bold">&#8734;</text></svg><span style="color:#e94560">P</span></div>
  <div class="hdr-r">
    <span class="dot off" id="sDot"></span>
    <button class="gear-btn" onclick="toggleSettings()" title="Reglages" id="gearBtn">
      <svg viewBox="0 0 16 16" width="18" height="18"><circle cx="8" cy="8" r="2" fill="currentColor"/><path d="M14.3 6.7l-1.2-.2a5.2 5.2 0 00-.5-1.1l.7-1-1.7-1.7-1 .7c-.3-.2-.7-.4-1.1-.5L9.3 1.7H7.7l-.2 1.2c-.4.1-.8.3-1.1.5l-1-.7L3.7 4.4l.7 1c-.2.3-.4.7-.5 1.1L2.7 6.7v1.6l1.2.2c.1.4.3.8.5 1.1l-.7 1 1.7 1.7 1-.7c.3.2.7.4 1.1.5l.2 1.2h1.6l.2-1.2c.4-.1.8-.3 1.1-.5l1 .7 1.7-1.7-.7-1c.2-.3.4-.7.5-1.1l1.2-.2V6.7z" fill="none" stroke="currentColor" stroke-width="1.2"/></svg>
    </button>
  </div>
</div>

<div class="tabs">
  <button class="active" onclick="showTab('keyboard',this)"><svg viewBox="0 0 16 14" width="14" height="12"><rect x="1" y="1" width="14" height="12" rx="2" fill="none" stroke="currentColor" stroke-width="1.2"/><rect x="3" y="4" width="2" height="2" rx=".5" fill="currentColor"/><rect x="7" y="4" width="2" height="2" rx=".5" fill="currentColor"/><rect x="11" y="4" width="2" height="2" rx=".5" fill="currentColor"/><rect x="4" y="8" width="8" height="2" rx=".5" fill="currentColor"/></svg>Clavier</button>
  <button onclick="showTab('midi',this)"><svg viewBox="0 0 14 16" width="12" height="14"><path d="M12 1v10.5a2.5 2.5 0 11-2-2.45V3.5L5 5v8a2.5 2.5 0 11-2-2.45V1l9-2z" fill="currentColor" opacity=".85"/></svg>MIDI</button>
  <button id="btnTabAir" onclick="showTab('air',this)" style="display:none"><svg viewBox="0 0 16 16" width="14" height="14"><path d="M8 1C4.5 1 2 4 2 7c0 2 1 3.5 2.5 4.5L4 15h8l-.5-3.5C13 10.5 14 9 14 7c0-3-2.5-6-6-6z" fill="none" stroke="currentColor" stroke-width="1.2"/><path d="M6 7.5c0-1.5 1-2.5 2-2.5s2 1 2 2.5" fill="none" stroke="currentColor" stroke-width="1"/></svg>Air</button>
  <button id="btnTabCalib" onclick="showTab('calib',this)"><svg viewBox="0 0 16 16" width="14" height="14"><path d="M6.5 1L7 4H5L2 8h3l-.5 7 6-9H7.5l2-5z" fill="currentColor" opacity=".85"/></svg>Calibration</button>
</div>

<!-- TAB: KEYBOARD -->
<div class="tab active" id="tab-keyboard">
  <div class="flute-box">
    <svg id="fluteSvg" viewBox="0 0 400 100"></svg>
    <div class="flute-info"><span id="fluteNote">-</span> <span id="fluteInfo" style="color:#555">Jouez une note</span></div>
  </div>
  <div class="section">
    <div class="cfg-row"><label>Velocity</label>
      <input type="range" min="1" max="127" value="100" id="velSlider" oninput="setVelocity(this.value)">
      <span id="velVal" style="min-width:28px;text-align:right">100</span>
    </div>
    <div style="display:flex;justify-content:flex-end;margin-top:6px">
      <button class="btn btn-s" id="kbdToggle" onclick="toggleKbdMode()" style="padding:4px 10px;font-size:.75em"><svg viewBox="0 0 16 16" width="12" height="12"><rect x="1" y="3" width="14" height="10" rx="2" fill="none" stroke="currentColor" stroke-width="1.2"/><rect x="3" y="5" width="2" height="2" rx=".3" fill="currentColor"/><rect x="7" y="5" width="2" height="2" rx=".3" fill="currentColor"/><rect x="11" y="5" width="2" height="2" rx=".3" fill="currentColor"/><rect x="5" y="9" width="6" height="2" rx=".3" fill="currentColor"/></svg><span id="kbdModeLabel">Piano</span></button>
    </div>
  </div>
  <div class="keys" id="pianoKeys"></div>
</div>

<!-- TAB: MIDI PLAYER -->
<div class="tab" id="tab-midi">
  <div class="section">
    <h3>Fichier MIDI</h3>
    <div class="drop-zone" id="dropZone" onclick="document.getElementById('midiFile').click()">
      Glisser-deposer un fichier MIDI ou cliquer
      <input type="file" id="midiFile" accept=".mid,.midi" style="display:none" onchange="uploadMidi(this)">
    </div>
    <div class="upload-bar" id="uploadBar"><div class="upload-fill" id="uploadFill"></div></div>
    <div style="margin-top:8px">
      <div style="display:flex;align-items:center;gap:8px;margin-bottom:4px">
        <span style="font-size:.75em;color:#888" id="midiStorageText">0 / 500 KB</span>
      </div>
      <div class="progress-bar" style="height:6px"><div id="midiStorageFill" class="progress-fill" style="width:0%;background:#4ecca3"></div></div>
    </div>
    <div id="midiFileList" style="margin-top:8px"></div>
    <div class="file-info" id="fileInfo" style="margin-top:8px">
      <span id="fName"></span> &bull; <span id="fEvents"></span> evt &bull; <span id="fDuration"></span>
    </div>
    <div id="chSelect" style="display:none;margin-top:8px">
      <div class="cfg-row"><label>Canal</label><select id="midiCh" onchange="setMidiCh(this.value)"><option value="255">Tous</option></select></div>
    </div>
  </div>
  <div class="section">
    <div class="transport">
      <button class="btn btn-g" id="btnPlay" onclick="wsSend({t:'play'})" disabled><svg viewBox="0 0 16 16" width="18" height="18"><path d="M4 2l10 6-10 6z" fill="currentColor"/></svg></button>
      <button class="btn btn-s" id="btnPause" onclick="wsSend({t:'pause'})" disabled><svg viewBox="0 0 16 16" width="18" height="18"><rect x="3" y="2" width="3.5" height="12" rx="1" fill="currentColor"/><rect x="9.5" y="2" width="3.5" height="12" rx="1" fill="currentColor"/></svg></button>
      <button class="btn btn-p" id="btnStop" onclick="wsSend({t:'stop'})" disabled><svg viewBox="0 0 16 16" width="18" height="18"><rect x="3" y="3" width="10" height="10" rx="1" fill="currentColor"/></svg></button>
    </div>
    <div class="progress-bar"><div class="progress-fill" id="progressFill"></div></div>
    <div class="file-info" id="progressText">--:-- / --:--</div>
  </div>

  </div>
</div>

<!-- TAB: CALIBRATION -->
<div class="tab" id="tab-calib">
  <div class="steps">
    <div class="step-dot active" onclick="goStep(1)">1</div>
    <div class="step-line"></div>
    <div class="step-dot" onclick="goStep(2)">2</div>
    <div class="step-line"></div>
    <div class="step-dot" onclick="goStep(3)">3</div>
    <div class="step-line"></div>
    <div class="step-dot" onclick="goStep(4)">4</div>
  </div>
  <div class="step-labels">
    <span>Doigts</span><span>Doigtes</span><span>Souffle</span><span>Expression</span>
  </div>

  <!-- STEP 1: FINGERS -->
  <div id="step1" class="step-panel">
    <div class="section">
      <h3>Type d'instrument</h3>
      <p style="font-size:.8em;color:#888;margin:0 0 8px">Choisissez l'instrument pour configurer automatiquement le nombre de trous et le type d'embouchure.</p>
      <div class="cfg-row"><label>Instrument</label>
        <select id="instrumentSelect" style="flex:1;max-width:300px" onchange="selectInstrument(this.value)"></select>
      </div>
    </div>
    <div class="section">
      <h3>Configuration des servos</h3>
      <div class="cfg-row"><label>Nombre de doigts</label>
        <div style="display:flex;align-items:center;gap:8px">
          <button class="btn btn-s" onclick="changeFingers(-1)">-</button>
          <span id="numFingersDisp" style="min-width:24px;text-align:center;font-weight:bold">6</span>
          <button class="btn btn-s" onclick="changeFingers(1)">+</button>
        </div>
      </div>
      <div class="cfg-row"><label>Servo airflow PCA</label>
        <select id="airPca" style="max-width:80px" onchange="checkPca()"></select>
      </div>
      <div class="cfg-row"><label>Amplitude ouverture</label>
        <input type="range" min="10" max="90" value="30" id="angleOpen" oninput="$('aoVal').textContent=this.value+'&deg;'">
        <span id="aoVal" style="min-width:36px">30&deg;</span>
      </div>
      <div class="cfg-row"><label>Demi-ouverture</label>
        <input type="range" min="10" max="90" value="50" id="halfHolePct" oninput="$('hhVal').textContent=this.value+'%'">
        <span id="hhVal" style="min-width:36px">50%</span>
      </div>
    </div>
    <div class="flute-box">
      <svg id="calFluteSvg" viewBox="0 0 400 100"></svg>
    </div>
    <div id="fingerCards"></div>
    <div class="btn-row" style="justify-content:flex-end">
      <button class="btn btn-p" id="btnSaveStep1" onclick="saveStep1()"><svg viewBox="0 0 16 16" width="14" height="14"><path d="M12.7 1H3a2 2 0 00-2 2v10a2 2 0 002 2h10a2 2 0 002-2V3.3L12.7 1zM8 13a2.5 2.5 0 110-5 2.5 2.5 0 010 5zM11 5H5V2h6v3z" fill="currentColor"/></svg>Sauver &amp; Continuer &rarr;</button>
    </div>
  </div>

  <!-- STEP 2: FINGERINGS -->
  <div id="step2" class="step-panel" style="display:none">
    <div class="section">
      <h3>Accordage &amp; doigtes</h3>
      <p style="font-size:.8em;color:#888;margin:0 0 8px">Selectionnez l'accordage correspondant a l'instrument choisi. Cela definit les notes jouables et les combinaisons de doigts associees.</p>
      <div class="cfg-row"><label>Accordage</label>
        <select id="presetSelect" style="flex:1;max-width:320px" onchange="applyPreset(this.value);updPresetInfo()"></select>
      </div>
    </div>
    <div class="section" id="fingeringSection">
      <div style="display:flex;gap:12px;font-size:.75em;color:#888;margin-bottom:8px">
        <span><span class="fg-dot open" style="display:inline-block;width:12px;height:12px;vertical-align:middle"></span> Ouvert</span>
        <span><span class="fg-dot closed" style="display:inline-block;width:12px;height:12px;vertical-align:middle"></span> Ferme</span>
        <span><span class="fg-dot closed thumb" style="display:inline-block;width:12px;height:12px;vertical-align:middle"></span> Pouce</span>
      </div>
      <div class="undo-bar">
        <button class="btn btn-s" id="undoBtn" onclick="undoFp()" disabled title="Ctrl+Z"><svg viewBox="0 0 16 16" width="12" height="12"><path d="M4 7h8a3 3 0 010 6H9" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/><path d="M7 4L4 7l3 3" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg>Annuler</button>
        <button class="btn btn-s" id="redoBtn" onclick="redoFp()" disabled title="Ctrl+Y"><svg viewBox="0 0 16 16" width="12" height="12"><path d="M12 7H4a3 3 0 000 6h3" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/><path d="M9 4l3 3-3 3" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg>Retablir</button>
        <span id="undoInfo"></span>
      </div>
      <div id="fingeringRows"></div>
      <div class="btn-row">
        <button class="btn btn-s" onclick="addNote()">+ Ajouter note</button>
        <button class="btn btn-s" onclick="removeLastNote()">- Supprimer</button>
      </div>
    </div>
    <div class="btn-row" style="justify-content:space-between">
      <button class="btn btn-s" onclick="goStep(1)"><svg viewBox="0 0 16 16" width="14" height="14"><path d="M10 3L5 8l5 5" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/></svg>Retour</button>
      <button class="btn btn-p" id="btnSaveStep2" onclick="saveStep2()"><svg viewBox="0 0 16 16" width="14" height="14"><path d="M12.7 1H3a2 2 0 00-2 2v10a2 2 0 002 2h10a2 2 0 002-2V3.3L12.7 1zM8 13a2.5 2.5 0 110-5 2.5 2.5 0 010 5zM11 5H5V2h6v3z" fill="currentColor"/></svg>Sauver &amp; Continuer &rarr;</button>
    </div>
  </div>

  <!-- STEP 3: AIRFLOW -->
  <div id="step3" class="step-panel" style="display:none">
    <div class="section" id="micSection" style="display:none">
      <h3><span class="mic-badge" id="micBadge">MIC</span> Auto-calibration</h3>
      <div class="vu"><span style="font-size:.8em;min-width:24px">VU</span>
        <div class="vu-track"><div class="vu-fill" id="vuFill"></div></div>
        <span class="vu-val" id="vuVal">0%</span>
      </div>
      <div class="pitch">
        <span class="pitch-note" id="pitchNote">-</span>
        <span class="pitch-hz" id="pitchHz">- Hz</span>
        <span class="pitch-cents ok" id="pitchCents">-</span>
      </div>
      <div class="btn-row">
        <button class="btn btn-g" id="btnAcalStart" onclick="startAutoCal()">Auto-calibrer toutes les notes</button>
        <button class="btn btn-p" id="btnAcalStop" onclick="stopAutoCal()" style="display:none">Arreter</button>
      </div>
      <div class="acal-progress" id="acalProgress">
        <div class="acal-info"><span id="acalStep">-</span><span id="acalState">-</span></div>
        <div class="acal-bar"><div class="acal-fill" id="acalFill"></div></div>
        <div id="acalResults" style="margin-top:8px;display:none"></div>
      </div>
    </div>
    <div class="section">
      <h3>Souffle par note</h3>
      <div id="airflowRows"></div>
    </div>
    <div class="btn-row" style="justify-content:space-between">
      <button class="btn btn-s" onclick="goStep(2)"><svg viewBox="0 0 16 16" width="14" height="14"><path d="M10 3L5 8l5 5" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/></svg>Retour</button>
      <button class="btn btn-p" id="btnSaveStep3" onclick="saveStep3()"><svg viewBox="0 0 16 16" width="14" height="14"><path d="M12.7 1H3a2 2 0 00-2 2v10a2 2 0 002 2h10a2 2 0 002-2V3.3L12.7 1zM8 13a2.5 2.5 0 110-5 2.5 2.5 0 010 5zM11 5H5V2h6v3z" fill="currentColor"/></svg>Sauver &amp; Continuer &rarr;</button>
    </div>
  </div>

  <!-- STEP 4: EXPRESSION -->
  <div id="step4" class="step-panel" style="display:none">
    <div class="section">
      <h3>Comportement du souffle</h3>
      <p style="font-size:.8em;color:#888;margin:0 0 12px">Definissez comment le servo airflow reagit au debut de chaque note (attaque) et l'influence de la velocite MIDI sur le volume de souffle.<br>Ces valeurs servent de defaut ; <b>CC73 (Attack Time)</b> peut changer le mode en temps reel via MIDI : 0-42=Stable, 43-84=Accent, 85-127=Crescendo.</p>
      <div class="cfg-row"><label>Mode d'attaque</label>
        <div style="display:flex;gap:6px;flex-wrap:wrap">
          <button class="btn btn-s expr-mode" data-mode="0" onclick="setExprMode(0)">Stable</button>
          <button class="btn btn-s expr-mode" data-mode="1" onclick="setExprMode(1)">Accent</button>
          <button class="btn btn-s expr-mode" data-mode="2" onclick="setExprMode(2)">Crescendo</button>
        </div>
      </div>
      <div id="exprModeDesc" style="font-size:.78em;color:#aaa;margin:4px 0 12px;padding:6px 10px;background:rgba(255,255,255,.04);border-radius:6px"></div>
      <div id="exprParams">
        <div class="cfg-row"><label>Ecart (%)</label>
          <input type="range" min="5" max="50" value="20" id="airAtkOff" oninput="CFG.air_atk_off=parseInt(this.value);$('atkOffVal').textContent=this.value+'%';drawExprCurve();markDirty()">
          <span id="atkOffVal" style="min-width:36px">20%</span>
        </div>
        <div class="cfg-row"><label>Duree attaque (ms)</label>
          <input type="range" min="10" max="1000" step="10" value="150" id="airAtkMs" oninput="CFG.air_atk_ms=parseInt(this.value);$('atkMsVal').textContent=this.value+'ms';drawExprCurve();markDirty()">
          <span id="atkMsVal" style="min-width:48px">150ms</span>
        </div>
      </div>
      <div class="cfg-row"><label>Reponse velocite</label>
        <input type="range" min="0" max="100" value="50" id="airVelResp" oninput="CFG.air_vel_resp=parseInt(this.value);$('velRespVal').textContent=this.value+'%';drawExprCurve();markDirty()">
        <span id="velRespVal" style="min-width:36px">50%</span>
      </div>
    </div>
    <div class="section">
      <h3>Apercu</h3>
      <svg id="exprCurveSvg" viewBox="0 0 320 140" style="width:100%;max-width:400px;height:auto;background:rgba(0,0,0,.2);border-radius:8px"></svg>
      <div style="font-size:.7em;color:#666;margin-top:4px">Bleu = attaque forte (vel 127) / Gris = attaque douce (vel 40)</div>
    </div>
    <div class="section">
      <h3>Vibrato (CC1 Modulation)</h3>
      <p style="font-size:.8em;color:#888;margin:0 0 12px">Oscillation du servo airflow modulee par CC1. Amplitude=0 desactive le vibrato.</p>
      <div class="cfg-row"><label>Frequence (Hz)</label>
        <input type="range" min="1" max="12" step="0.5" value="5" id="exVibF" oninput="CFG.vib_freq=parseFloat(this.value);$('exVibFVal').textContent=this.value+'Hz';markDirty()">
        <span id="exVibFVal" style="min-width:40px">5Hz</span>
      </div>
      <div class="cfg-row"><label>Amplitude max (deg)</label>
        <input type="range" min="0" max="20" step="0.5" value="3" id="exVibA" oninput="CFG.vib_amp=parseFloat(this.value);$('exVibAVal').textContent=this.value+'\u00b0';markDirty()">
        <span id="exVibAVal" style="min-width:36px">3&deg;</span>
      </div>
    </div>

    <div class="section">
      <h3>Breath Controller (CC2)</h3>
      <p style="font-size:.8em;color:#888;margin:0 0 12px">Controle continu du souffle par CC2. Si aucun CC2 n'est recu, la velocite noteOn est utilisee en fallback.</p>
      <div class="cfg-row"><label>Actif</label><input type="checkbox" id="exCC2On" style="width:auto;flex:0" onchange="CFG.cc2_on=this.checked;markDirty()"></div>
      <div class="cfg-row"><label>Seuil silence</label>
        <input type="range" min="0" max="30" value="5" id="exCC2Thr" oninput="CFG.cc2_thr=parseInt(this.value);$('exCC2ThrVal').textContent=this.value;markDirty()">
        <span id="exCC2ThrVal" style="min-width:28px">5</span>
      </div>
      <div class="cfg-row"><label>Courbe reponse</label>
        <input type="range" min="0.1" max="3" step="0.1" value="1" id="exCC2Curve" oninput="CFG.cc2_curve=parseFloat(this.value);$('exCC2CurveVal').textContent=this.value;markDirty()">
        <span id="exCC2CurveVal" style="min-width:28px">1</span>
      </div>
      <div class="cfg-row"><label>Timeout fallback (ms)</label>
        <input type="range" min="100" max="5000" step="100" value="2000" id="exCC2To" oninput="CFG.cc2_timeout=parseInt(this.value);$('exCC2ToVal').textContent=this.value+'ms';markDirty()">
        <span id="exCC2ToVal" style="min-width:52px">2000ms</span>
      </div>
    </div>

    <div class="btn-row" style="justify-content:space-between">
      <button class="btn btn-s" onclick="goStep(3)"><svg viewBox="0 0 16 16" width="14" height="14"><path d="M10 3L5 8l5 5" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/></svg>Retour</button>
      <button class="btn btn-g" id="btnSaveStep4" onclick="saveStep4()"><svg viewBox="0 0 16 16" width="14" height="14"><path d="M3 8l3.5 4L13 4" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/></svg>Sauver &amp; Terminer</button>
    </div>
  </div>
</div>

<!-- TAB: AIR MANAGEMENT -->
<div class="tab" id="tab-air">
  <div class="section">
    <h3>Systeme d'air</h3>
    <div class="flute-box">
      <svg id="airSvgFull" viewBox="0 0 520 120" style="max-height:120px"></svg>
    </div>
    <div style="display:flex;gap:16px;flex-wrap:wrap;margin-top:8px">
      <div><span style="font-size:.7em;color:#9aa">Pompe</span><div style="font-weight:bold" id="airPumpPwm">OFF</div></div>
      <div><span style="font-size:.7em;color:#9aa">Reservoir</span><div style="font-weight:bold" id="airResPct">--%</div></div>
      <div><span style="font-size:.7em;color:#9aa">Distance</span><div style="font-weight:bold" id="airResMm">--mm</div></div>
      <div><span style="font-size:.7em;color:#9aa">Valve</span><div style="font-weight:bold" id="airValveState">--</div></div>
      <div><span style="font-size:.7em;color:#9aa">Servo air</span><div style="font-weight:bold" id="airServoAngle">--°</div></div>
    </div>
  </div>
  <div class="section">
    <h3>Controle pompe</h3>
    <div class="cfg-row"><label>Cible pression</label>
      <input type="range" min="0" max="100" value="0" id="pumpTarget" oninput="setPumpTarget(this.value)">
      <span id="pumpTargetVal" style="min-width:36px;text-align:right">0%</span>
    </div>
    <div class="btn-row">
      <button class="btn btn-p" onclick="wsSend({t:'pump_stop'})">Arreter pompe</button>
      <button class="btn btn-s" onclick="wsSend({t:'test_sol',o:1})">Ouvrir valve</button>
      <button class="btn btn-s" onclick="wsSend({t:'test_sol',o:0})">Fermer valve</button>
    </div>
  </div>
  <div class="section">
    <h3>Parametres</h3>
    <div class="cfg-row"><label>Mode air</label>
      <select id="airModeSelect" onchange="setAirMode(this.value)">
        <option value="0">Classique (solenoide)</option>
        <option value="1">Servo-valve</option>
        <option value="2">Pompe directe</option>
        <option value="3">Pompe + reservoir</option>
      </select>
    </div>
    <div id="airParamsPump" style="display:none">
      <div class="cfg-row"><label>GPIO pompe</label>
        <select id="airPumpPin"><option value="25">GPIO 25</option><option value="26">GPIO 26</option><option value="27">GPIO 27</option><option value="33">GPIO 33</option></select>
      </div>
      <div class="cfg-row"><label>PWM min</label>
        <input type="number" id="airPumpMin" min="0" max="255" value="80">
      </div>
      <div class="cfg-row"><label>PWM max</label>
        <input type="number" id="airPumpMax" min="0" max="255" value="255">
      </div>
    </div>
    <div id="airParamsValve" style="display:none">
      <div class="cfg-row"><label>Type valve</label>
        <select id="airValveType" onchange="toggleValveParams()">
          <option value="0">Solenoide GPIO</option>
          <option value="1">Servo PCA</option>
        </select>
      </div>
      <div id="airValveServoParams" style="display:none">
        <div class="cfg-row"><label>Canal PCA valve</label>
          <input type="number" id="airValveCh" min="0" max="15" value="11">
        </div>
      </div>
    </div>
    <div id="airParamsRes" style="display:none">
      <div class="cfg-row"><label>Capteur</label>
        <select id="airSensorType"><option value="0">VL53L0X</option><option value="1">VL6180X</option></select>
      </div>
      <div class="cfg-row"><label>Cible (mm)</label>
        <input type="number" id="airSensTarget" min="1" max="300" value="50">
      </div>
      <div class="cfg-row"><label>Min (mm)</label>
        <input type="number" id="airSensMin" min="1" max="300" value="10">
      </div>
      <div class="cfg-row"><label>Max (mm)</label>
        <input type="number" id="airSensMax" min="1" max="300" value="150">
      </div>
      <div class="cfg-row"><label>PID Kp (x10)</label>
        <input type="number" id="airPidKp" min="1" max="100" value="30">
      </div>
      <div class="cfg-row"><label>PID Ki (x10)</label>
        <input type="number" id="airPidKi" min="0" max="50" value="5">
      </div>
    </div>
    <div class="btn-row" style="margin-top:12px">
      <button class="btn btn-g" onclick="saveAirSettings()">Sauvegarder</button>
    </div>
    <div id="airSettingsMsg" style="font-size:.75em;color:#0f0;margin-top:6px"></div>
  </div>
</div>

<!-- TAB: MINI BROWSER -->
<!-- WIZARD OVERLAY (first boot) -->
<div class="settings-overlay" id="wizardOverlay">
<div class="settings-box" style="max-width:540px">
  <h2>Bienvenue ! Configuration initiale</h2>
  <div id="wizStep1">
    <p style="color:#9aa;font-size:.85em;margin-bottom:12px">Choisissez votre type d'instrument :</p>
    <div id="wizPresets" style="display:flex;flex-direction:column;gap:8px"></div>
    <div class="btn-row" style="margin-top:16px">
      <button class="btn btn-g" onclick="wizNext(2)">Suivant</button>
    </div>
  </div>
  <div id="wizStep2" style="display:none">
    <h3 style="color:#e94560;margin:0 0 4px">Systeme d'air</h3>
    <p style="color:#9aa;font-size:.8em;margin:0 0 14px">Comment l'air est-il envoye dans la flute ?</p>
    <div style="display:flex;flex-direction:column;gap:10px">
      <label class="wiz-card" onclick="wizSetAir(0)" style="align-items:flex-start">
        <input type="radio" name="wizAir" value="0" checked style="margin-top:4px">
        <span style="flex:1">
          <b>Solenoide</b>
          <span style="font-size:.75em;color:#9aa;display:block;margin:4px 0">La valve solenoide ouvre/ferme un flux d'air externe (compresseur, soufflerie). Branchee sur un GPIO.</span>
          <svg viewBox="0 0 220 48" width="220" height="48" style="display:block;margin-top:4px">
            <rect x="1" y="14" width="50" height="20" rx="4" fill="#0f3460" stroke="#4ecca3" stroke-width="1.5"/>
            <text x="26" y="28" text-anchor="middle" fill="#4ecca3" font-size="8">Air ext.</text>
            <line x1="51" y1="24" x2="80" y2="24" stroke="#9aa" stroke-width="1.5"/>
            <rect x="80" y="10" width="44" height="28" rx="4" fill="#16213e" stroke="#e94560" stroke-width="1.5"/>
            <text x="102" y="22" text-anchor="middle" fill="#e94560" font-size="7">Solenoide</text>
            <text x="102" y="32" text-anchor="middle" fill="#666" font-size="6">GPIO</text>
            <line x1="124" y1="24" x2="155" y2="24" stroke="#9aa" stroke-width="1.5"/>
            <polygon points="152,20 160,24 152,28" fill="#9aa"/>
            <rect x="160" y="8" width="56" height="32" rx="10" fill="none" stroke="#4ecca3" stroke-width="1.5"/>
            <text x="188" y="28" text-anchor="middle" fill="#4ecca3" font-size="8">Flute</text>
          </svg>
        </span>
      </label>
      <label class="wiz-card" onclick="wizSetAir(1)" style="align-items:flex-start">
        <input type="radio" name="wizAir" value="1" style="margin-top:4px">
        <span style="flex:1">
          <b>Servo-valve</b>
          <span style="font-size:.75em;color:#9aa;display:block;margin:4px 0">Un servo sur le bus PCA9685 controle l'ouverture de la valve. Permet un controle progressif du debit d'air.</span>
          <svg viewBox="0 0 220 48" width="220" height="48" style="display:block;margin-top:4px">
            <rect x="1" y="14" width="50" height="20" rx="4" fill="#0f3460" stroke="#4ecca3" stroke-width="1.5"/>
            <text x="26" y="28" text-anchor="middle" fill="#4ecca3" font-size="8">Air ext.</text>
            <line x1="51" y1="24" x2="80" y2="24" stroke="#9aa" stroke-width="1.5"/>
            <rect x="80" y="10" width="44" height="28" rx="4" fill="#16213e" stroke="#e94560" stroke-width="1.5"/>
            <text x="102" y="22" text-anchor="middle" fill="#e94560" font-size="7">Servo</text>
            <text x="102" y="32" text-anchor="middle" fill="#666" font-size="6">PCA9685</text>
            <line x1="124" y1="24" x2="155" y2="24" stroke="#9aa" stroke-width="1.5"/>
            <polygon points="152,20 160,24 152,28" fill="#9aa"/>
            <rect x="160" y="8" width="56" height="32" rx="10" fill="none" stroke="#4ecca3" stroke-width="1.5"/>
            <text x="188" y="28" text-anchor="middle" fill="#4ecca3" font-size="8">Flute</text>
          </svg>
        </span>
      </label>
      <label class="wiz-card" onclick="wizSetAir(2)" style="align-items:flex-start">
        <input type="radio" name="wizAir" value="2" style="margin-top:4px">
        <span style="flex:1">
          <b>Pompe directe</b>
          <span style="font-size:.75em;color:#9aa;display:block;margin:4px 0">Une pompe PWM souffle directement dans la flute. La valve (solenoide ou servo) coupe l'air entre les notes.</span>
          <svg viewBox="0 0 260 48" width="260" height="48" style="display:block;margin-top:4px">
            <circle cx="24" cy="24" r="16" fill="#16213e" stroke="#e94560" stroke-width="1.5"/>
            <text x="24" y="21" text-anchor="middle" fill="#e94560" font-size="7">Pompe</text>
            <text x="24" y="31" text-anchor="middle" fill="#666" font-size="6">PWM</text>
            <line x1="40" y1="24" x2="70" y2="24" stroke="#9aa" stroke-width="1.5"/>
            <polygon points="67,20 75,24 67,28" fill="#9aa"/>
            <rect x="76" y="10" width="44" height="28" rx="4" fill="#16213e" stroke="#e94560" stroke-width="1.5"/>
            <text x="98" y="22" text-anchor="middle" fill="#e94560" font-size="7">Valve</text>
            <text x="98" y="32" text-anchor="middle" fill="#666" font-size="6">ON/OFF</text>
            <line x1="120" y1="24" x2="150" y2="24" stroke="#9aa" stroke-width="1.5"/>
            <polygon points="147,20 155,24 147,28" fill="#9aa"/>
            <rect x="156" y="8" width="56" height="32" rx="10" fill="none" stroke="#4ecca3" stroke-width="1.5"/>
            <text x="184" y="28" text-anchor="middle" fill="#4ecca3" font-size="8">Flute</text>
          </svg>
        </span>
      </label>
      <label class="wiz-card" onclick="wizSetAir(3)" style="align-items:flex-start">
        <input type="radio" name="wizAir" value="3" style="margin-top:4px">
        <span style="flex:1">
          <b>Pompe + reservoir</b>
          <span style="font-size:.75em;color:#9aa;display:block;margin:4px 0">La pompe remplit un reservoir (ballon). Un capteur de distance mesure le niveau. PID pour reguler la pression.</span>
          <svg viewBox="0 0 320 48" width="320" height="48" style="display:block;margin-top:4px">
            <circle cx="24" cy="24" r="16" fill="#16213e" stroke="#e94560" stroke-width="1.5"/>
            <text x="24" y="21" text-anchor="middle" fill="#e94560" font-size="7">Pompe</text>
            <text x="24" y="31" text-anchor="middle" fill="#666" font-size="6">PWM</text>
            <line x1="40" y1="24" x2="62" y2="24" stroke="#9aa" stroke-width="1.5"/>
            <polygon points="59,20 67,24 59,28" fill="#9aa"/>
            <ellipse cx="100" cy="24" rx="28" ry="16" fill="#16213e" stroke="#4ecca3" stroke-width="1.5"/>
            <text x="100" y="21" text-anchor="middle" fill="#4ecca3" font-size="7">Reservoir</text>
            <text x="100" y="31" text-anchor="middle" fill="#666" font-size="6">+ capteur</text>
            <line x1="128" y1="24" x2="155" y2="24" stroke="#9aa" stroke-width="1.5"/>
            <polygon points="152,20 160,24 152,28" fill="#9aa"/>
            <rect x="160" y="10" width="44" height="28" rx="4" fill="#16213e" stroke="#e94560" stroke-width="1.5"/>
            <text x="182" y="22" text-anchor="middle" fill="#e94560" font-size="7">Valve</text>
            <text x="182" y="32" text-anchor="middle" fill="#666" font-size="6">ON/OFF</text>
            <line x1="204" y1="24" x2="230" y2="24" stroke="#9aa" stroke-width="1.5"/>
            <polygon points="227,20 235,24 227,28" fill="#9aa"/>
            <rect x="236" y="8" width="56" height="32" rx="10" fill="none" stroke="#4ecca3" stroke-width="1.5"/>
            <text x="264" y="28" text-anchor="middle" fill="#4ecca3" font-size="8">Flute</text>
          </svg>
        </span>
      </label>
    </div>
    <div class="btn-row" style="margin-top:16px">
      <button class="btn btn-s" onclick="wizNext(1)">Retour</button>
      <button class="btn btn-g" onclick="wizFinish()">Terminer</button>
    </div>
  </div>
</div>
</div>

<!-- SETTINGS OVERLAY -->
<div class="settings-overlay" id="settingsOverlay">
<div class="settings-box">
  <h2>Reglages <button class="close-btn" onclick="toggleSettings()">&times;</button></h2>

  <div class="section"><h3>Appareil</h3>
    <div class="cfg-row"><label>Nom</label><input type="text" id="cfgDevice" maxlength="31"></div>
    <div class="cfg-row"><label>Canal MIDI</label><select id="cfgMidiCh">
      <option value="0">Omni (tous)</option>
    </select></div>
  </div>

  <div class="section"><h3>Timing</h3>
    <div class="cfg-row"><label>Servo&rarr;valve (ms)</label><input type="number" id="cfgDelay" min="0" max="1000"></div>
    <div class="cfg-row"><label>Intervalle valve (ms)</label><input type="number" id="cfgValveInt" min="0" max="500"></div>
    <div class="cfg-row"><label>Note min (ms)</label><input type="number" id="cfgMinNote" min="0" max="500"></div>
  </div>

  <div class="section"><h3>Servo Airflow</h3>
    <div class="cfg-row"><label>Angle repos</label><input type="number" id="cfgAirOff" min="0" max="180"></div>
    <div class="cfg-row"><label>Angle min</label><input type="number" id="cfgAirMin" min="0" max="180"></div>
    <div class="cfg-row"><label>Angle max</label><input type="number" id="cfgAirMax" min="0" max="180"></div>
  </div>

  <div class="section"><h3>CC Defaults</h3>
    <div class="cfg-row"><label>Volume (CC7)</label><input type="number" id="cfgCCVol" min="0" max="127"></div>
    <div class="cfg-row"><label>Expression (CC11)</label><input type="number" id="cfgCCExpr" min="0" max="127"></div>
    <div class="cfg-row"><label>Modulation (CC1)</label><input type="number" id="cfgCCMod" min="0" max="127"></div>
    <div class="cfg-row"><label>Breath (CC2)</label><input type="number" id="cfgCCBreath" min="0" max="127"></div>
    <div class="cfg-row"><label>Brightness (CC74)</label><input type="number" id="cfgCCBright" min="0" max="127"></div>
  </div>

  <div class="section"><h3>Solenoide</h3>
    <div class="cfg-row"><label>GPIO Pin</label><select id="cfgSolPin"></select></div>
    <div class="cfg-row"><label>PWM activation</label><input type="number" id="cfgSolAct" min="0" max="255"></div>
    <div class="cfg-row"><label>PWM maintien</label><input type="number" id="cfgSolHold" min="0" max="255"></div>
    <div class="cfg-row"><label>Temps (ms)</label><input type="number" id="cfgSolTime" min="0" max="500"></div>
  </div>

  <div class="section"><h3>Power off servo</h3>
    <div class="cfg-row"><label>Timeout (ms)</label><input type="number" id="cfgUnpower" min="0" max="60000"></div>
  </div>

  <div class="section"><h3>Stockage MIDI</h3>
    <div class="cfg-row"><label>Limite (KB)</label><input type="number" id="cfgMidiLimit" min="50" max="2000" step="50"></div>
  </div>

  <div class="section"><h3>Interface</h3>
    <div class="cfg-row"><label>Couleur instrument</label><input type="color" id="cfgColor" value="#D4B044" style="width:40px;height:28px;flex:0;padding:0;border:1px solid #555;border-radius:4px;cursor:pointer"></div>
    <div class="cfg-row"><label>Mode clavier</label><select id="cfgKbdMode"><option value="0">Flute</option><option value="1">Piano</option></select></div>
    <div class="cfg-row"><label>Cacher Calibration</label><input type="checkbox" id="cfgHideCalib" style="width:auto;flex:0"></div>
    <div class="cfg-row"><label>Schema air (clavier)</label><input type="checkbox" id="cfgShowAir" style="width:auto;flex:0"></div>
  </div>

  <div class="section"><h3>WiFi</h3>
    <div class="cfg-row"><label>Etat</label><span id="wifiState" style="font-size:.85em;color:#888">-</span></div>
    <div class="btn-row"><button class="btn btn-s" onclick="startWifiScan()">Scanner</button>
      <span style="font-size:.75em;color:#555" id="scanStatus"></span></div>
    <div id="wifiList" style="margin:8px 0"></div>
    <div class="cfg-row"><label>SSID</label><input type="text" id="wifiSsid" maxlength="32"></div>
    <div class="cfg-row"><label>Mot de passe</label><input type="password" id="wifiPass" maxlength="64"></div>
    <div class="btn-row"><button class="btn btn-g" onclick="connectWifi()">Connecter</button></div>
    <div style="font-size:.75em;color:#888;margin-top:4px" id="wifiMsg"></div>
  </div>

  <div class="section"><h3>Monitor</h3>
    <div style="display:flex;gap:16px;margin-bottom:8px">
      <div><span style="color:#888;font-size:.8em">Etat</span><div style="font-weight:bold" id="monState">IDLE</div></div>
      <div><span style="color:#888;font-size:.8em">Heap</span><div style="font-weight:bold" id="monHeap">-</div></div>
    </div>
    <div class="cc-bar"><span class="cc-label">CC1 Mod</span><div class="cc-track"><div class="cc-fill" id="ccBar1"></div></div><span class="cc-val" id="ccV1">0</span></div>
    <div class="cc-bar"><span class="cc-label">CC2 Breath</span><div class="cc-track"><div class="cc-fill" id="ccBar2"></div></div><span class="cc-val" id="ccV2">127</span></div>
    <div class="cc-bar"><span class="cc-label">CC7 Vol</span><div class="cc-track"><div class="cc-fill" id="ccBar7"></div></div><span class="cc-val" id="ccV7">127</span></div>
    <div class="cc-bar"><span class="cc-label">CC11 Expr</span><div class="cc-track"><div class="cc-fill" id="ccBar11"></div></div><span class="cc-val" id="ccV11">127</span></div>
    <div class="log" id="logBox"></div>
    <div class="btn-row"><button class="btn btn-p" onclick="wsSend({t:'panic'})">ALL SOUND OFF</button></div>
  </div>

  <div class="btn-row" style="justify-content:center;margin-top:16px">
    <button class="btn btn-g" id="btnSaveSettings" onclick="saveSettings()"><svg viewBox="0 0 16 16" width="14" height="14"><path d="M12.7 1H3a2 2 0 00-2 2v10a2 2 0 002 2h10a2 2 0 002-2V3.3L12.7 1zM8 13a2.5 2.5 0 110-5 2.5 2.5 0 010 5zM11 5H5V2h6v3z" fill="currentColor"/></svg>Sauvegarder</button>
    <button class="btn btn-s" onclick="resetConfig()">Reset defauts</button>
  </div>
  <div style="font-size:.75em;color:#9aa;text-align:center;margin-top:8px" id="settingsMsg"></div>

  <div style="border-top:1px solid #333;margin-top:20px;padding-top:16px">
    <div class="btn-row" style="justify-content:center">
      <button class="btn btn-p" onclick="factoryReset()">Reset usine (changer instrument)</button>
    </div>
    <div style="font-size:.7em;color:#666;text-align:center;margin-top:6px">Remet tous les parametres par defaut et relance l'assistant de configuration</div>
  </div>
</div>
</div>

<div class="status-bar">
  <span id="sText">Deconnecte</span>
  <span id="heapBar">-</span>
</div>

<!-- SEQUENCE EDITOR MODAL -->
<div class="modal-overlay" id="seqModal" onclick="if(event.target===this)closeSeqModal()">
  <div class="modal">
    <div class="modal-hdr">
      <h3>Editeur de sequences</h3>
      <button class="modal-close" onclick="closeSeqModal()">&times;</button>
    </div>
    <div class="modal-body">
      <p style="font-size:.78em;color:#888;margin:0 0 8px">Creer une sequence. Cliquer sur la grille pour placer/retirer des notes.</p>
      <div class="cfg-row">
        <label>BPM</label><input type="number" id="seqBpm" value="120" min="40" max="300" style="width:60px" onchange="drawSeqGrid()">
        <label style="margin-left:12px">Mesures</label><input type="number" id="seqBars" value="4" min="1" max="16" style="width:50px" onchange="drawSeqGrid()">
      </div>
      <div style="overflow-x:auto;-webkit-overflow-scrolling:touch;margin:8px 0">
        <svg id="seqSvg" style="min-width:100%;height:auto;background:rgba(0,0,0,.2);border-radius:6px;cursor:crosshair"></svg>
      </div>
      <div class="btn-row">
        <button class="btn btn-g" onclick="uploadSeqMidi()"><svg viewBox="0 0 16 16" width="14" height="14"><path d="M4 2l10 6-10 6z" fill="currentColor"/></svg>Jouer</button>
        <button class="btn btn-s" onclick="clearSeq()">Effacer</button>
      </div>
      <div style="margin-top:12px;font-size:.72em;color:#666" id="seqMemInfo"></div>
    </div>
  </div>
</div>

<script>
// --- Constants (mirrored from settings.h) ---
const MIDI_CC_MAX=127,MIDI_VEL_MAX=127,MAX_FINGERS=15;
const WEB_DEF_VEL=100,TEST_SOL_MS=2000,VU_SCALE=500,PITCH_OK_CT=15;

const N=['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
const STATES=['IDLE','POSITIONING','PLAYING','STOPPING'];
let ws=null,velocity=WEB_DEF_VEL,CFG=null,curNote=null;
let calibStep=1,fileLoaded=false,playerDuration=0;
let micDetected=false,autoCalRunning=false;
let dirty=false,fpHistory=[],fpFuture=[];



// Presets: {id,n:name,h:holes,th:thumbIdx(-1=none),em:embouchure(trav|bec|naf|end|oca),d:[[midi,[fp],amn,amx],...]}
const PR=[
{id:'tabor_d',n:'Tabor Pipe (R\u00e9)',h:3,th:0,em:'bec',d:[
[74,[0,0,0],10,50],[76,[0,0,1],10,50],[78,[0,1,1],10,55],[79,[1,1,1],10,55],
[81,[0,0,0],30,70],[83,[0,0,1],30,70],[85,[0,1,1],30,75],[86,[1,1,1],35,75]]},
{id:'ocarina_c',n:'Ocarina 4 trous (Do)',h:4,th:-1,em:'oca',d:[
[72,[0,0,0,0],10,50],[74,[0,0,1,0],10,55],[76,[0,1,0,0],15,60],[77,[0,1,0,1],20,65],
[79,[0,1,1,1],25,70],[81,[1,0,1,0],30,75],[83,[1,1,1,0],35,85],[84,[1,1,1,1],35,90]]},
{id:'naf_a',n:'Fl\u00fbte am\u00e9rindienne (La)',h:4,th:-1,em:'naf',d:[
[69,[0,0,0,0],5,45],[72,[0,0,0,1],5,45],[74,[0,0,1,1],5,50],[76,[0,1,1,1],5,50],[79,[1,1,1,1],5,55],
[81,[0,0,0,0],40,80],[84,[0,0,0,1],40,80],[86,[0,0,1,1],40,85],[88,[0,1,1,1],45,85]]},
{id:'shaku_d',n:'Shakuhachi (R\u00e9)',h:5,th:0,em:'end',d:[
[62,[0,0,0,0,0],5,40],[65,[0,0,0,1,0],5,45],[67,[0,0,1,1,1],5,50],[69,[0,1,1,1,1],10,50],[72,[1,1,1,1,0],10,55],
[74,[0,0,0,0,0],40,80],[77,[0,0,0,1,0],40,80],[79,[0,0,1,1,1],40,85],[81,[0,1,1,1,1],45,85],[84,[1,1,1,1,0],45,90],[86,[0,0,0,0,0],60,100]]},
{id:'naf5_fs',n:'Fl\u00fbte am\u00e9rindienne 5 (Fa#)',h:5,th:-1,em:'naf',d:[
[66,[0,0,0,0,0],5,45],[69,[0,0,0,0,1],5,45],[71,[0,0,0,1,1],5,50],[73,[0,0,1,1,1],5,50],[76,[0,1,1,1,1],5,55],[78,[1,1,1,1,1],5,60],
[81,[0,0,0,0,1],35,75],[83,[0,0,0,1,1],35,80],[85,[0,0,1,1,1],40,80],[88,[0,1,1,1,1],40,85]]},
{id:'whistle_d',n:'Tin Whistle (R\u00e9)',h:6,th:-1,em:'bec',d:[
[74,[0,0,0,0,0,0],5,50],[76,[0,0,0,0,0,1],5,50],[78,[0,0,0,0,1,1],5,50],[79,[0,0,0,1,1,1],5,55],
[81,[0,0,1,1,1,1],5,55],[83,[0,1,1,1,1,1],5,60],[85,[1,1,1,1,1,1],5,60],
[86,[0,0,0,0,0,0],30,80],[88,[0,0,0,0,0,1],30,80],[90,[0,0,0,0,1,1],30,85],[91,[0,0,0,1,1,1],35,85],
[93,[0,0,1,1,1,1],35,90],[95,[0,1,1,1,1,1],35,90],[97,[1,1,1,1,1,1],40,95]]},
{id:'irish_c',n:'Fl\u00fbte irlandaise (Do)',h:6,th:-1,em:'trav',d:[
[82,[0,1,1,1,1,1],10,60],[83,[1,1,1,1,1,1],0,50],
[84,[0,0,0,0,0,0],20,75],[86,[0,0,0,0,0,1],15,70],[88,[0,0,0,0,1,1],10,65],[89,[0,0,0,1,1,1],10,60],
[91,[0,0,1,1,1,1],5,55],[93,[0,1,1,1,1,1],5,50],[95,[1,1,1,1,1,1],0,45],
[96,[0,0,0,0,0,0],50,100],[98,[0,0,0,0,0,1],45,95],[100,[0,0,0,0,1,1],40,90],[101,[0,0,0,1,1,1],35,85],
[103,[0,0,1,1,1,1],30,80]]},
{id:'bansuri_a',n:'Bansuri (La)',h:6,th:-1,em:'trav',d:[
[64,[0,0,0,0,0,0],5,45],[66,[0,0,0,0,0,1],5,45],[68,[0,0,0,0,1,1],5,50],
[69,[0,0,0,1,1,1],10,50],[71,[0,0,1,1,1,1],10,55],[73,[0,1,1,1,1,1],10,55],
[74,[1,0,0,0,0,0],15,60],[76,[0,0,0,0,0,0],20,65],[78,[0,0,0,0,0,1],20,65],[80,[0,0,0,0,1,1],20,70],
[81,[0,0,0,1,1,1],35,80],[83,[0,0,1,1,1,1],35,85],[85,[0,1,1,1,1,1],40,85]]},
{id:'dizi_d',n:'Dizi (R\u00e9)',h:6,th:-1,em:'trav',d:[
[69,[0,0,0,0,0,0],5,45],[71,[0,0,0,0,0,1],5,45],[73,[0,0,0,0,1,1],5,50],
[74,[0,0,0,1,1,1],10,50],[76,[0,0,1,1,1,1],10,55],[78,[0,1,1,1,1,1],10,55],
[81,[0,0,0,0,0,0],30,75],[83,[0,0,0,0,0,1],30,75],[85,[0,0,0,0,1,1],30,80],
[86,[0,0,0,1,1,1],35,80],[88,[0,0,1,1,1,1],35,85],[90,[0,1,1,1,1,1],40,85]]},
{id:'fife_bb',n:'Fifre (Sib)',h:6,th:-1,em:'trav',d:[
[70,[0,0,0,0,0,0],10,55],[72,[0,0,0,0,0,1],10,55],[74,[0,0,0,0,1,1],10,55],[75,[0,0,0,1,1,1],10,60],
[77,[0,0,1,1,1,1],10,60],[79,[0,1,1,1,1,1],10,60],[81,[1,1,1,1,1,1],10,65],
[82,[0,0,0,0,0,0],35,80],[84,[0,0,0,0,0,1],35,80],[86,[0,0,0,0,1,1],35,85],[87,[0,0,0,1,1,1],40,85],
[89,[0,0,1,1,1,1],40,90],[91,[0,1,1,1,1,1],40,90],[93,[1,1,1,1,1,1],45,95]]},
{id:'quena_g',n:'Quena (Sol)',h:7,th:0,em:'end',d:[
[67,[0,0,0,0,0,0,0],5,45],[69,[0,0,0,0,0,0,1],5,45],[71,[0,0,0,0,0,1,1],5,50],[72,[0,0,0,0,1,1,1],5,50],
[74,[0,0,0,1,1,1,1],5,55],[76,[0,0,1,1,1,1,1],5,55],[78,[0,1,1,1,1,1,1],5,60],
[79,[2,0,0,0,0,0,0],25,70],[81,[2,0,0,0,0,0,1],25,70],[83,[2,0,0,0,0,1,1],25,75],[84,[2,0,0,0,1,1,1],30,75],
[86,[2,0,0,1,1,1,1],30,80],[88,[2,0,1,1,1,1,1],30,80],[90,[2,1,1,1,1,1,1],35,85]]},
{id:'ney_a',n:'Ney turc (La)',h:7,th:0,em:'end',d:[
[57,[0,0,0,0,0,0,0],0,30],[59,[0,0,0,0,0,0,1],0,30],[60,[0,0,0,0,0,1,1],0,35],
[62,[0,0,0,0,1,1,1],0,35],[64,[0,0,0,1,1,1,1],0,40],[65,[0,0,1,1,1,1,1],0,40],[67,[0,1,1,1,1,1,1],0,45],
[69,[0,0,0,0,0,0,0],20,65],[71,[0,0,0,0,0,0,1],20,65],[72,[0,0,0,0,0,1,1],20,70],
[74,[0,0,0,0,1,1,1],25,70],[76,[0,0,0,1,1,1,1],25,75],[77,[0,0,1,1,1,1,1],25,75],[79,[0,1,1,1,1,1,1],30,80]]},
{id:'recorder_c',n:'Fl\u00fbte \u00e0 bec (Do)',h:7,th:0,em:'bec',d:[
[72,[0,0,0,0,0,0,0],5,40],[74,[0,0,0,0,0,0,1],5,40],[76,[0,0,0,0,0,1,1],5,45],[77,[0,0,0,0,1,1,1],5,45],
[79,[0,0,0,1,1,1,1],5,50],[81,[0,0,1,1,1,1,1],5,50],[83,[0,1,1,1,1,1,1],5,55],
[84,[2,0,0,0,0,0,0],20,65],[86,[2,0,0,0,0,0,1],20,65],[88,[2,0,0,0,0,1,1],25,70],[89,[2,0,0,0,1,1,1],25,70],
[91,[2,0,0,1,1,1,1],25,75],[93,[2,0,1,1,1,1,1],30,75],[95,[2,1,0,1,1,1,1],30,80],[96,[2,0,1,0,1,1,1],35,85]]},
{id:'recorder_b8',n:'Fl\u00fbte \u00e0 bec baroque (Do)',h:8,th:0,em:'bec',d:[
[72,[0,0,0,0,0,0,0,0],5,40],[74,[0,0,0,0,0,0,1,1],5,40],[76,[0,0,0,0,0,1,1,1],5,45],
[77,[0,0,0,0,1,0,0,1],5,45],[79,[0,0,0,1,1,1,1,1],5,50],[81,[0,0,1,1,1,1,1,1],10,55],
[83,[0,1,0,1,1,1,1,1],10,55],
[84,[2,0,0,1,1,1,1,1],15,60],[86,[2,0,0,0,0,0,1,1],25,70],[88,[2,0,0,0,0,1,1,1],25,70],
[89,[2,0,0,0,1,0,0,1],30,75],[91,[2,0,0,1,1,1,1,1],30,80],[93,[2,0,1,0,1,1,1,1],35,80],
[95,[2,1,0,1,0,1,1,1],35,85],[96,[2,0,1,0,1,0,1,1],40,90]]},
{id:'kaval_d',n:'Kaval (R\u00e9)',h:8,th:0,em:'end',d:[
[62,[0,0,0,0,0,0,0,0],0,30],[64,[0,0,0,0,0,0,0,1],0,30],[65,[0,0,0,0,0,0,1,1],0,35],
[67,[0,0,0,0,0,1,1,1],0,35],[69,[0,0,0,0,1,1,1,1],0,40],[71,[0,0,0,1,1,1,1,1],5,40],
[72,[0,0,1,1,1,1,1,1],5,45],[74,[0,1,1,1,1,1,1,1],5,45],
[76,[2,0,0,0,0,0,0,1],25,65],[77,[2,0,0,0,0,0,1,1],25,70],
[79,[2,0,0,0,0,1,1,1],25,70],[81,[2,0,0,0,1,1,1,1],30,75],[83,[2,0,0,1,1,1,1,1],30,75],
[84,[2,0,1,1,1,1,1,1],30,80],[86,[2,1,1,1,1,1,1,1],35,80]]}
];

function showToast(msg,type){type=type||'info';const c=$('toastContainer');
  const ic={success:'<svg viewBox="0 0 16 16" width="16" height="16"><path d="M3 8l3.5 4L13 4" fill="none" stroke="#fff" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg>',
  error:'<svg viewBox="0 0 16 16" width="16" height="16"><path d="M5.5 5.5l5 5M10.5 5.5l-5 5" stroke="#fff" stroke-width="1.5" stroke-linecap="round"/></svg>',
  info:'<svg viewBox="0 0 16 16" width="16" height="16"><circle cx="8" cy="8" r="6" fill="none" stroke="#fff" stroke-width="1.5"/><path d="M8 7v4M8 5v.5" stroke="#fff" stroke-width="1.5" stroke-linecap="round"/></svg>'};
  const t=document.createElement('div');t.className='toast '+type;
  t.innerHTML=(ic[type]||ic.info)+'<span>'+esc(msg)+'</span>';c.appendChild(t);
  requestAnimationFrame(()=>requestAnimationFrame(()=>t.classList.add('show')));
  setTimeout(()=>{t.classList.remove('show');setTimeout(()=>t.remove(),300)},3000)}
function markDirty(){dirty=true;$('unsavedBadge').classList.add('show');updStepDots()}
function markClean(){dirty=false;$('unsavedBadge').classList.remove('show');updStepDots()}
function btnLoad(id,on){const b=$(id);if(!b)return;if(on){b.classList.add('loading');b.disabled=true}else{b.classList.remove('loading');b.disabled=false}}
function testPulse(el){el.classList.add('test-pulse');setTimeout(()=>el.classList.remove('test-pulse'),600)}
function fpSnap(){if(!CFG)return;fpHistory.push(JSON.stringify(CFG.notes.map(n=>({midi:n.midi,fp:[...n.fp]}))));
  fpFuture=[];if(fpHistory.length>50)fpHistory.shift();updUndoUI()}
function undoFp(){if(!fpHistory.length||!CFG)return;
  fpFuture.push(JSON.stringify(CFG.notes.map(n=>({midi:n.midi,fp:[...n.fp]}))));
  const s=JSON.parse(fpHistory.pop());s.forEach((sn,i)=>{if(CFG.notes[i]){CFG.notes[i].midi=sn.midi;CFG.notes[i].fp=sn.fp}});
  buildFingeringRows();updUndoUI();markDirty()}
function redoFp(){if(!fpFuture.length||!CFG)return;
  fpHistory.push(JSON.stringify(CFG.notes.map(n=>({midi:n.midi,fp:[...n.fp]}))));
  const s=JSON.parse(fpFuture.pop());s.forEach((sn,i)=>{if(CFG.notes[i]){CFG.notes[i].midi=sn.midi;CFG.notes[i].fp=sn.fp}});
  buildFingeringRows();updUndoUI();markDirty()}
function updUndoUI(){$('undoBtn').disabled=!fpHistory.length;$('redoBtn').disabled=!fpFuture.length;
  $('undoInfo').textContent=fpHistory.length?fpHistory.length+' modif.':''}
function checkPca(){if(!CFG)return;const used={};const airP=parseInt($('airPca').value);used[airP]='Souffle';
  document.querySelectorAll('.cal-card').forEach((card,i)=>{const ch=CFG.fingers[i]?CFG.fingers[i].ch:i;
    let conflict=used[ch]!==undefined;card.classList.toggle('pca-conflict',conflict);
    const w=card.querySelector('.pca-warn');if(w)w.textContent=conflict?'Conflit PCA '+ch+' avec '+used[ch]:'';
    used[ch]='Doigt '+(i+1)})}
function updDualFill(ni){if(!CFG)return;
  if(CFG.notes[ni].amn>CFG.notes[ni].amx){const t=CFG.notes[ni].amn;CFG.notes[ni].amn=CFG.notes[ni].amx;CFG.notes[ni].amx=t;
    const mi=$('amn'+ni),mx=$('amx'+ni);if(mi)mi.textContent=CFG.notes[ni].amn;if(mx)mx.textContent=CFG.notes[ni].amx}
  const f=$('drf'+ni);if(!f)return;const a=CFG.notes[ni].amn,b=CFG.notes[ni].amx;
  f.style.left=a+'%';f.style.width=(b-a)+'%'}

function mn(m){return N[m%12]+(Math.floor(m/12)-1)}
function isBlack(m){return[1,3,6,8,10].includes(m%12)}
function $(id){return document.getElementById(id)}
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;')}
function fmt(ms){if(!ms||ms<=0)return'--:--';const s=Math.floor(ms/1000);return String(s/60|0).padStart(2,'0')+':'+String(s%60).padStart(2,'0')}
function addLog(t){const b=$('logBox');if(!b)return;b.innerHTML+='<div>'+esc(t)+'</div>';b.scrollTop=b.scrollHeight;
  while(b.children.length>100)b.removeChild(b.firstChild)}

// --- Tabs ---
function showTab(id,btn){
  document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));
  document.querySelectorAll('.tabs button').forEach(b=>b.classList.remove('active'));
  $('tab-'+id).classList.add('active');if(btn)btn.classList.add('active');
  if(id==='calib'&&CFG)buildCalibUI();
  if(id==='air'&&CFG)buildAirUI();
}
// --- Air System ---
function applyAirTabVisibility(){
  const airBtn=$('btnTabAir');
  const hasAir=CFG&&(CFG.show_air||CFG.air_mode>=1||CFG.pump_on||CFG.res_on);
  airBtn.style.display=hasAir?'':'none';
  // Rebuild flute to show/hide integrated air overlay
  if(CFG)buildFlute(CFG,'fluteSvg',false);
}
function setPumpTarget(v){$('pumpTargetVal').textContent=v+'%';wsSend({t:'pump_target',v:parseInt(v)})}
function setAirMode(v){
  const m=parseInt(v);
  $('airParamsPump').style.display=m>=2?'':'none';
  $('airParamsValve').style.display=m>=1?'':'none';
  $('airParamsRes').style.display=m>=3?'':'none';
}
function toggleValveParams(){
  $('airValveServoParams').style.display=$('airValveType').value==='1'?'':'none';
}
function saveAirSettings(){
  const m=parseInt($('airModeSelect').value);
  const d={air_mode:m,valve_servo:$('airValveType').value==='1',
    valve_ch:parseInt($('airValveCh').value),
    pump_on:m>=2,pump_pin:parseInt($('airPumpPin').value),
    pump_min:parseInt($('airPumpMin').value),pump_max:parseInt($('airPumpMax').value),
    res_on:m>=3,sens_type:parseInt($('airSensorType').value),
    sens_target:parseInt($('airSensTarget').value),sens_min:parseInt($('airSensMin').value),
    sens_max:parseInt($('airSensMax').value),pid_kp:parseInt($('airPidKp').value),
    pid_ki:parseInt($('airPidKi').value),show_air:true};
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)})
    .then(r=>r.json()).then(j=>{$('airSettingsMsg').textContent=j.ok?'Sauvegarde OK':'Erreur';
    if(j.ok){CFG.air_mode=m;CFG.pump_on=d.pump_on;CFG.res_on=d.res_on;CFG.show_air=true;applyAirTabVisibility()}
    setTimeout(()=>$('airSettingsMsg').textContent='',3000)})
}
function fillAirSettings(){
  if(!CFG)return;
  $('airModeSelect').value=CFG.air_mode||0;
  $('airValveType').value=CFG.valve_servo?'1':'0';
  $('airValveCh').value=CFG.valve_ch||11;
  $('airPumpPin').value=CFG.pump_pin||25;
  $('airPumpMin').value=CFG.pump_min||80;
  $('airPumpMax').value=CFG.pump_max||255;
  $('airSensorType').value=CFG.sens_type||1;
  $('airSensTarget').value=CFG.sens_target||50;
  $('airSensMin').value=CFG.sens_min||10;
  $('airSensMax').value=CFG.sens_max||150;
  $('airPidKp').value=CFG.pid_kp||30;
  $('airPidKi').value=CFG.pid_ki||5;
  setAirMode(CFG.air_mode||0);toggleValveParams();
}
function buildAirUI(){fillAirSettings();buildAirSvg('airSvgFull',true)}
function buildAirSvg(svgId,full){
  const svg=$(svgId);if(!svg||!CFG)return;
  const m=CFG.air_mode||0;
  const w=full?520:480,h=full?120:70;
  svg.setAttribute('viewBox','0 0 '+w+' '+h);
  let s='<defs><linearGradient id="agMetal" x1="0" y1="0" x2="0" y2="1">'+
    '<stop offset="0%" stop-color="#8899aa"/><stop offset="100%" stop-color="#556677"/></linearGradient>'+
    '<linearGradient id="agBalloon" x1="0" y1="0" x2="0" y2="1">'+
    '<stop offset="0%" stop-color="#e07050"/><stop offset="100%" stop-color="#a03020"/></linearGradient></defs>';
  const cy=h/2,ox=10;
  // Pompe (si mode>=2)
  if(m>=2){
    s+='<rect x="'+ox+'" y="'+(cy-20)+'" width="50" height="40" rx="5" fill="url(#agMetal)" stroke="#334" stroke-width="1.5"/>';
    s+='<circle id="airPumpIcon" cx="'+(ox+25)+'" cy="'+cy+'" r="12" fill="none" stroke="#dde" stroke-width="1.5"/>';
    s+='<line id="airPumpBlade" x1="'+(ox+25)+'" y1="'+(cy-10)+'" x2="'+(ox+25)+'" y2="'+(cy+10)+'" stroke="#dde" stroke-width="2"/>';
    s+='<text x="'+(ox+25)+'" y="'+(cy+30)+'" text-anchor="middle" style="font-size:8px;fill:#9aa">Pompe</text>';
    // Tuyau pompe -> reservoir/valve
    const tx=m>=3?140:200;
    s+='<line x1="'+(ox+50)+'" y1="'+cy+'" x2="'+tx+'" y2="'+cy+'" stroke="#7799bb" stroke-width="4" stroke-linecap="round"/>';
    s+='<polygon points="'+(tx-6)+','+(cy-4)+' '+tx+','+cy+' '+(tx-6)+','+(cy+4)+'" fill="#7799bb"/>';
  }
  // Reservoir/ballon (si mode>=3)
  if(m>=3){
    const bx=175,by=cy;
    s+='<ellipse id="airBalloon" cx="'+bx+'" cy="'+by+'" rx="30" ry="25" fill="url(#agBalloon)" stroke="#802010" stroke-width="1.5" opacity=".85"/>';
    s+='<ellipse cx="'+(bx-8)+'" cy="'+(by-10)+'" rx="8" ry="5" fill="#fff" opacity=".15"/>';
    s+='<text x="'+bx+'" y="'+(by+4)+'" text-anchor="middle" style="font-size:10px;fill:#fff;font-weight:bold" id="airBalloonPct">--%</text>';
    s+='<text x="'+bx+'" y="'+(by+38)+'" text-anchor="middle" style="font-size:8px;fill:#9aa">Reservoir</text>';
    // Capteur au-dessus
    s+='<rect x="'+(bx-6)+'" y="'+(by-45)+'" width="12" height="8" rx="2" fill="#334" stroke="#556" stroke-width=".8"/>';
    s+='<line x1="'+bx+'" y1="'+(by-37)+'" x2="'+bx+'" y2="'+(by-26)+'" stroke="#5af" stroke-width="1" stroke-dasharray="2,2"/>';
    s+='<text x="'+(bx+10)+'" y="'+(by-38)+'" style="font-size:7px;fill:#5af">ToF</text>';
    // Tuyau reservoir -> valve
    s+='<line x1="'+(bx+30)+'" y1="'+by+'" x2="260" y2="'+cy+'" stroke="#7799bb" stroke-width="4" stroke-linecap="round"/>';
    s+='<polygon points="254,'+(cy-4)+' 260,'+cy+' 254,'+(cy+4)+'" fill="#7799bb"/>';
  }
  // Valve (toujours si mode>=1)
  if(m>=1){
    const vx=m>=3?270:(m>=2?210:60);
    s+='<rect id="airValveRect" x="'+vx+'" y="'+(cy-15)+'" width="30" height="30" rx="4" fill="#444" stroke="#667" stroke-width="1.5"/>';
    s+='<rect id="airValveInd" x="'+(vx+10)+'" y="'+(cy-8)+'" width="10" height="16" rx="2" fill="#e44"/>';
    s+='<text x="'+(vx+15)+'" y="'+(cy+26)+'" text-anchor="middle" style="font-size:8px;fill:#9aa">'+(CFG.valve_servo?'Servo':'Valve')+'</text>';
    // Tuyau valve -> servo airflow
    const ax=vx+70;
    s+='<line x1="'+(vx+30)+'" y1="'+cy+'" x2="'+ax+'" y2="'+cy+'" stroke="#7799bb" stroke-width="4" stroke-linecap="round"/>';
    s+='<polygon points="'+(ax-6)+','+(cy-4)+' '+ax+','+cy+' '+(ax-6)+','+(cy+4)+'" fill="#7799bb"/>';
    // Servo airflow
    s+='<rect x="'+ax+'" y="'+(cy-18)+'" width="40" height="36" rx="5" fill="url(#agMetal)" stroke="#334" stroke-width="1.5"/>';
    s+='<path id="airServoNeedle" d="M'+(ax+20)+','+cy+' L'+(ax+20)+','+(cy-14)+'" stroke="#e94" stroke-width="2.5" stroke-linecap="round"/>';
    s+='<circle cx="'+(ax+20)+'" cy="'+cy+'" r="3" fill="#e94"/>';
    s+='<text x="'+(ax+20)+'" y="'+(cy+28)+'" text-anchor="middle" style="font-size:8px;fill:#9aa">Servo Air</text>';
    // Tuyau -> flute
    const fx=ax+80;
    s+='<line x1="'+(ax+40)+'" y1="'+cy+'" x2="'+fx+'" y2="'+cy+'" stroke="#7799bb" stroke-width="4" stroke-linecap="round"/>';
    s+='<polygon points="'+(fx-6)+','+(cy-4)+' '+fx+','+cy+' '+(fx-6)+','+(cy+4)+'" fill="#7799bb"/>';
    s+='<text x="'+(fx+10)+'" y="'+(cy+4)+'" style="font-size:9px;fill:#bbc;font-style:italic">Flute</text>';
  } else {
    // Mode classique : juste solenoide + servo
    s+='<rect x="20" y="'+(cy-15)+'" width="30" height="30" rx="4" fill="#444" stroke="#667" stroke-width="1.5"/>';
    s+='<rect id="airValveInd" x="30" y="'+(cy-8)+'" width="10" height="16" rx="2" fill="#e44"/>';
    s+='<text x="35" y="'+(cy+26)+'" text-anchor="middle" style="font-size:8px;fill:#9aa">Solenoide</text>';
    s+='<line x1="50" y1="'+cy+'" x2="90" y2="'+cy+'" stroke="#7799bb" stroke-width="4" stroke-linecap="round"/>';
    s+='<polygon points="84,'+(cy-4)+' 90,'+cy+' 84,'+(cy+4)+'" fill="#7799bb"/>';
    s+='<rect x="90" y="'+(cy-18)+'" width="40" height="36" rx="5" fill="url(#agMetal)" stroke="#334" stroke-width="1.5"/>';
    s+='<path id="airServoNeedle" d="M110,'+cy+' L110,'+(cy-14)+'" stroke="#e94" stroke-width="2.5" stroke-linecap="round"/>';
    s+='<circle cx="110" cy="'+cy+'" r="3" fill="#e94"/>';
    s+='<text x="110" y="'+(cy+28)+'" text-anchor="middle" style="font-size:8px;fill:#9aa">Servo Air</text>';
    s+='<line x1="130" y1="'+cy+'" x2="170" y2="'+cy+'" stroke="#7799bb" stroke-width="4" stroke-linecap="round"/>';
    s+='<polygon points="164,'+(cy-4)+' 170,'+cy+' 164,'+(cy+4)+'" fill="#7799bb"/>';
    s+='<text x="180" y="'+(cy+4)+'" style="font-size:9px;fill:#bbc;font-style:italic">Flute</text>';
  }
  svg.innerHTML=s;
}
function updateAirDiagram(d){
  // Update all valve indicators (keyboard + air tab)
  document.querySelectorAll('[id=airValveInd]').forEach(vi=>vi.setAttribute('fill',d.valve_open?'#4e4':'#e44'));
  // Update all balloon percent displays
  document.querySelectorAll('[id=airBalloonPct]').forEach(bp=>{bp.textContent=(d.res_pct!=null?d.res_pct:'--')+'%'});
  // Update servo air needle rotation on 120° arc
  document.querySelectorAll('[id=airServoNeedle]').forEach(nd=>{
    if(d.air_angle==null)return;
    // Map servo angle (0-180) to gauge angle (-150° to -30°)
    const pct=Math.min(1,Math.max(0,d.air_angle/180));
    const deg=-150+pct*120;
    const cx=nd.parentNode?nd.getAttribute('x1')||0:0;
    const cy2=nd.parentNode?nd.getAttribute('y1')||0:0;
    nd.setAttribute('transform','rotate('+deg+','+cx+','+cy2+')')});
  // Update pump icon state
  if(d.pump_pwm!=null){
    document.querySelectorAll('[id=airPumpIcon]').forEach(ic=>{
      ic.setAttribute('stroke',d.pump_pwm>0?'#4ecca3':'#dde')})}
  // Update pump/reservoir display in Air tab
  const pp=$('airPumpPwm');if(pp)pp.textContent=d.pump_pwm>0?d.pump_pwm:'OFF';
  const rp=$('airResPct');if(rp)rp.textContent=(d.res_pct!=null?d.res_pct:'-')+'%';
  const rm=$('airResMm');if(rm)rm.textContent=(d.res_mm!=null?d.res_mm:'-')+'mm';
  const vs=$('airValveState');if(vs)vs.textContent=d.valve_open?'OUVERT':'FERME';
  const sa=$('airServoAngle');if(sa)sa.textContent=(d.air_angle!=null?d.air_angle:'-')+'°';
}
// Pump toggle from keyboard (click pump in SVG to disable/enable)
let pumpDisabled=false;
function togglePump(){
  pumpDisabled=!pumpDisabled;
  wsSend({t:'pump_enable',v:pumpDisabled?0:1});
  document.querySelectorAll('.pump-toggle').forEach(g=>{
    g.classList.toggle('pump-off',pumpDisabled)});
  // Show red cross overlay
  document.querySelectorAll('[id=pumpCrossOff]').forEach(el=>{
    el.style.display=pumpDisabled?'':'none'});
  showToast(pumpDisabled?'Pompe desactivee':'Pompe activee','info')
}
// --- First boot wizard ---
let wizAirMode=0;
function showWizard(){
  const wp=$('wizPresets');if(!wp)return;
  // Build preset radio list from PR array
  let h='';
  PR.forEach((p,i)=>{
    h+='<label class="wiz-card"><input type="radio" name="wizPreset" value="'+i+'"'+(i===0?' checked':'')+'>'+
      '<span><b>'+p.n+'</b><br><span style="font-size:.75em;color:#9aa">'+p.h+' trous, '+
      ({trav:'Traversiere',bec:'A bec',naf:'Amerindienne',end:'Embouchure libre',oca:'Ocarina'}[p.em]||p.em)+
      '</span></span></label>';
  });
  h+='<label class="wiz-card"><input type="radio" name="wizPreset" value="-1">'+
    '<span><b>Personnalise</b><br><span style="font-size:.75em;color:#9aa">Configurer manuellement dans Calibration</span></span></label>';
  wp.innerHTML=h;
  $('wizardOverlay').classList.add('open');
}
function wizNext(step){
  $('wizStep1').style.display=step===1?'':'none';
  $('wizStep2').style.display=step===2?'':'none';
}
function wizSetAir(m){wizAirMode=m}
function wizFinish(){
  // Get selected preset
  const radios=document.querySelectorAll('input[name="wizPreset"]');
  let presetIdx=-1;
  radios.forEach(r=>{if(r.checked)presetIdx=parseInt(r.value)});
  // Build config body
  const body={air_mode:wizAirMode,pump_on:wizAirMode>=2,res_on:wizAirMode>=3,
    valve_servo:wizAirMode===1,show_air:true};
  // Apply preset if selected
  if(presetIdx>=0&&presetIdx<PR.length){
    const p=PR[presetIdx];
    body.num_fingers=p.h;body.embouchure=p.em;
    body.fingers=[];
    for(let i=0;i<p.h;i++){body.fingers.push({ch:i,a:90,d:-1,th:p.th===i?1:0})}
    body.notes=[];
    p.d.forEach(nd=>{body.notes.push({midi:nd[0],fp:nd[1],amn:nd[2],amx:nd[3]})});
    body.num_notes=p.d.length;body.angle_open=30;body.half_hole_pct=50;
  }
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(r=>r.json()).then(d=>{
      $('wizardOverlay').classList.remove('open');
      if(d.ok){showToast('Configuration sauvegardee','success');loadConfig()}
    }).catch(()=>{$('wizardOverlay').classList.remove('open')});
}
function toggleSettings(){$('settingsOverlay').classList.toggle('open');if($('settingsOverlay').classList.contains('open')&&CFG)fillSettings()}
function applyCalibVisibility(){
  const calibBtn=$('btnTabCalib');
  if(CFG&&CFG.hide_calib){calibBtn.style.display='none';
    if($('tab-calib').classList.contains('active')){showTab('keyboard',document.querySelector('.tabs button'))}
  }else{calibBtn.style.display=''}
}

// --- WebSocket ---
let wsRetry=0;
function wsConnect(){
  const p=location.protocol==='https:'?'wss:':'ws:';
  ws=new WebSocket(p+'//'+location.host+'/ws');
  ws.onopen=()=>{wsRetry=0;$('sDot').className='dot on';$('sText').textContent='Connecte';addLog('WS connecte')};
  ws.onclose=()=>{$('sDot').className='dot off';$('sText').textContent='Deconnecte';
    const d=Math.min(30000,2000*Math.pow(1.5,wsRetry));wsRetry++;setTimeout(wsConnect,d)};
  ws.onerror=()=>{ws.close()};
  ws.onmessage=e=>{try{handleWs(JSON.parse(e.data))}catch(x){addLog('WS err: '+x.message)}};
}
function wsSend(o){if(ws&&ws.readyState===1)ws.send(JSON.stringify(o))}

function handleWs(d){
  if(d.t==='status'){
    $('monState').textContent=STATES[d.state]||'?';
    $('monState').style.color=d.playing?'#e94560':'#4ecca3';
    if(d.heap){$('monHeap').textContent=(d.heap/1024|0)+'KB';$('heapBar').textContent=(d.heap/1024|0)+'KB'}
    updateCC(1,d.cc1);updateCC(2,d.cc2);updateCC(7,d.cc7);updateCC(11,d.cc11);
    if(d.pp!==undefined)$('progressFill').style.width=d.pp+'%';
    if(d.ppos!==undefined)$('progressText').textContent=fmt(d.ppos)+' / '+fmt(playerDuration);
    // Air system live update
    if(d.valve_open!==undefined)updateAirDiagram(d);
    if(d.ps!==undefined){$('btnPlay').disabled=(d.ps===1);$('btnPause').disabled=(d.ps!==1);$('btnStop').disabled=(d.ps===0&&!fileLoaded)}
  }else if(d.t==='midi_loaded'){
    fileLoaded=true;playerDuration=d.duration||0;
    $('fName').textContent=d.file;$('fEvents').textContent=d.events;$('fDuration').textContent=fmt(d.duration);
    $('btnPlay').disabled=false;$('btnStop').disabled=false;addLog('MIDI: '+d.file);
    // Canaux presents
    if(d.channels!==undefined){
      const sel=$('midiCh');sel.innerHTML='<option value="255">Tous</option>';
      for(let c=0;c<16;c++){if(d.channels&(1<<c)){
        const o=document.createElement('option');o.value=c;o.textContent='Canal '+(c+1);sel.appendChild(o)}}
      $('chSelect').style.display=sel.options.length>2?'':'none'}
  }else if(d.t==='midi_error'){addLog('ERR: '+d.msg)}
  else if(d.t==='audio'){
    const rms=Math.min(100,Math.round((d.rms||0)*VU_SCALE));
    $('vuFill').style.width=rms+'%';$('vuVal').textContent=rms+'%';
    $('vuFill').style.background=rms>60?'#e94560':rms>30?'#e9a645':'#4ecca3';
    if(d.midi>0){$('pitchNote').textContent=mn(d.midi);$('pitchHz').textContent=Math.round(d.hz)+' Hz';
      const c=d.cents||0;$('pitchCents').textContent=(c>=0?'+':'')+c.toFixed(0)+' ct';
      $('pitchCents').className='pitch-cents '+(Math.abs(c)<PITCH_OK_CT?'ok':c>0?'sharp':'flat')}
    else{$('pitchNote').textContent='-';$('pitchHz').textContent='- Hz';$('pitchCents').textContent='-'}
  }else if(d.t==='acal_prog'){
    $('acalProgress').style.display='block';$('acalStep').textContent='Note '+(d.idx+1)+'/'+d.total+' '+d.note;
    $('acalFill').style.width=(((d.idx||0)/(d.total||1))*100)+'%';
    const sn={0:'Attente',1:'Prep',2:'Stab',3:'Sweep...',4:'Analyse',5:'OK'};
    $('acalState').textContent=sn[d.st]||'...'
  }else if(d.t==='acal_done'){
    autoCalRunning=false;$('btnAcalStart').style.display='';$('btnAcalStop').style.display='none';
    $('acalFill').style.width='100%';$('acalState').textContent='Termine !';addLog('Auto-cal OK');
    if(d.results){let h='';d.results.forEach(r=>{h+='<div style="display:flex;justify-content:space-between;font-size:.8em;padding:2px 0">'+
      '<span>'+esc(r.name)+'</span><span style="color:'+(r.ok?'#4ecca3':'#e94560')+'">'+(r.ok?esc(r.min+'%-'+r.max+'%'):'Echec')+'</span></div>'});
      $('acalResults').innerHTML=h;$('acalResults').style.display='block'}
    setTimeout(loadConfig,1000)
  }
}
function updateCC(n,v){if(v===undefined)return;const p=(v/MIDI_CC_MAX*100).toFixed(0);
  const b=$('ccBar'+n),t=$('ccV'+n);if(b)b.style.width=p+'%';if(t)t.textContent=v}

// --- Load config ---
function loadConfig(){
  const pk=$('pianoKeys');if(pk&&!CFG)pk.innerHTML='<div class="skeleton" style="width:100%;height:80px;margin:12px 0"></div>';
  fetch('/api/config').then(r=>r.json()).then(d=>{
    CFG=d;micDetected=d.mic||false;
    $('devName').childNodes[0].textContent=d.device||'ServoFlute';
    buildKeyboard();buildFlute(CFG,'fluteSvg',false);markClean();
    applyCalibVisibility();applyAirTabVisibility();drawSeqGrid();
    buildAirSvg('airSvgFull',true);
    if(CFG.first_boot){showWizard()}
    if(micDetected){$('micSection').style.display='';wsSend({t:'mic_mon',on:1})}
    else $('micSection').style.display='none';
  }).catch(e=>{addLog('Erreur config: '+e);showToast('Erreur chargement config','error')})
}

// --- KEYBOARD ---
function buildKeyboard(){
  const c=$('pianoKeys');c.innerHTML='';if(!CFG||!CFG.notes||!CFG.notes.length){c.innerHTML='<div style="color:#888;padding:16px;text-align:center">Aucune note</div>';return}
  if(CFG.kbd_mode===1){buildPianoKeyboard(c)}else{buildFluteKeyboard(c)}
  buildKeyMap();updKbdToggle()
}
function updKbdToggle(){const l=$('kbdModeLabel');if(l&&CFG)l.textContent=CFG.kbd_mode===1?'Flute':'Piano'}
function toggleKbdMode(){
  if(!CFG)return;CFG.kbd_mode=CFG.kbd_mode===1?0:1;
  buildKeyboard();
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({kbd_mode:CFG.kbd_mode})})
}
function addKeyEvents(el,midi){
  el.addEventListener('touchstart',e=>{e.preventDefault();e.stopPropagation();if(noteOn(midi))el.classList.add('pressed')},{passive:false});
  el.addEventListener('touchend',e=>{e.preventDefault();noteOff(midi);el.classList.remove('pressed')},{passive:false});
  el.addEventListener('touchmove',e=>{e.preventDefault()},{passive:false});
  el.addEventListener('touchcancel',()=>{noteOff(midi);el.classList.remove('pressed')});
  el.addEventListener('mousedown',e=>{e.preventDefault();if(noteOn(midi))el.classList.add('pressed')});
  el.addEventListener('mouseup',()=>{noteOff(midi);el.classList.remove('pressed')});
  el.addEventListener('mouseleave',()=>{if(el.classList.contains('pressed')){noteOff(midi);el.classList.remove('pressed')}})
}
function buildFluteKeyboard(c){
  c.className='keys';
  CFG.notes.forEach((n,idx)=>{
    const name=mn(n.midi);const key=document.createElement('div');
    key.className='key'+(isBlack(n.midi)?' black':'')+' fade-in fade-delay-'+(Math.min(4,(idx%4)+1));key.dataset.midi=n.midi;
    let dots='<span class="kf-row">';for(let f=0;f<CFG.num_fingers;f++)dots+='<span class="kf '+kfClass(n.fp[f])+'"></span>';dots+='</span>';
    const sc=idx<KC.length?KC[idx].toUpperCase():'';
    key.innerHTML='<span class="note-name">'+name+'</span><span class="note-midi">'+n.midi+'</span>'+dots+(sc?'<span class="key-shortcut">'+sc+'</span>':'');
    addKeyEvents(key,n.midi);
    c.appendChild(key)
  })
}
function buildPianoKeyboard(c){
  c.className='piano-keys';
  // Construire un piano optimise pour les notes jouables uniquement
  const midiSet=new Set(CFG.notes.map(n=>n.midi));
  const lo=Math.min(...midiSet),hi=Math.max(...midiSet);
  // Padding minimal: 1 note blanche de chaque cote pour un rendu propre
  const startMidi=Math.max(0,lo-2);const endMidi=Math.min(127,hi+2);
  const blacks=new Set([1,3,6,8,10]);
  // Passe 1: creer les blanches, passe 2: inserer les noires
  const whiteKeys=[];const blackKeys=[];
  for(let m=startMidi;m<=endMidi;m++){
    const nb=m%12;const inSet=midiSet.has(m);
    const key=document.createElement('div');
    key.dataset.midi=m;
    const name=mn(m);
    if(blacks.has(nb)){
      key.className='pkey blk'+(inSet?'':' disabled');
      key.innerHTML='<span class="pkey-label">'+name+'</span>';
      blackKeys.push({midi:m,el:key,inSet:inSet})
    }else{
      key.className='pkey white'+(inSet?'':' disabled');
      key.innerHTML='<span class="pkey-label">'+name+'</span>';
      whiteKeys.push({midi:m,el:key,inSet:inSet});
      c.appendChild(key)
    }
    if(inSet){addKeyEvents(key,m)}
    else{key.style.opacity='0.3';key.style.pointerEvents='none'}
  }
  // Inserer les noires par-dessus (positionnement absolu relatif au conteneur)
  blackKeys.forEach(bk=>{c.appendChild(bk.el)});
  // Positionner les noires apres layout
  requestAnimationFrame(()=>{
    blackKeys.forEach(bk=>{
      // Trouver la blanche precedente et suivante
      const prevWhite=findAdjacentWhite(whiteKeys,bk.midi,-1);
      const nextWhite=findAdjacentWhite(whiteKeys,bk.midi,1);
      if(prevWhite&&nextWhite){
        const pr=prevWhite.el.getBoundingClientRect();
        const nr=nextWhite.el.getBoundingClientRect();
        const cr=c.getBoundingClientRect();
        bk.el.style.position='absolute';
        bk.el.style.left=((pr.right+nr.left)/2-cr.left-14)+'px';
        bk.el.style.top='8px'
      }
    })
  })
}
function findAdjacentWhite(whites,midi,dir){
  let m=midi+dir;
  const blacks=new Set([1,3,6,8,10]);
  while(m>=0&&m<=127){
    if(!blacks.has(m%12))return whites.find(w=>w.midi===m)||null;
    m+=dir
  }
  return null
}

function noteOn(midi){
  // Monophonique: ignorer si une note est deja jouee
  if(curNote!==null&&curNote!==midi)return false;
  curNote=midi;updateFluteForNote(midi);
  document.querySelectorAll('#fluteSvg .flute-hole.open').forEach(h=>h.classList.add('playing'));
  // Souffle: calculer a partir de velocity et plage de la note
  if(CFG){const nd=CFG.notes.find(n=>n.midi===midi);if(nd){
    const airPct=nd.amn+Math.round((nd.amx-nd.amn)*velocity/127);
    wsSend({t:'air_live',v:airPct})}}
  wsSend({t:'non',n:midi,v:velocity});return true}
function noteOff(midi){wsSend({t:'nof',n:midi});if(curNote===midi){curNote=null;
  document.querySelectorAll('#fluteSvg .flute-hole.playing').forEach(h=>h.classList.remove('playing'))}}
function setVelocity(v){velocity=parseInt(v);$('velVal').textContent=v;wsSend({t:'velocity',v:velocity})}

// Keyboard shortcuts
const KC='azertyuiopqsdfghjklmwxcvbn'.split('');let keyMap={},keysDown=new Set();
function buildKeyMap(){keyMap={};if(!CFG)return;CFG.notes.forEach((n,i)=>{if(i<KC.length)keyMap[KC[i]]=n.midi})}
document.addEventListener('keydown',e=>{if(e.target.tagName==='INPUT'||e.target.tagName==='SELECT'||e.repeat||e.ctrlKey||e.altKey||e.metaKey)return;
  const n=keyMap[e.key.toLowerCase()];if(n&&!keysDown.has(e.key)){
    if(noteOn(n)){keysDown.add(e.key);const el=document.querySelector('.key[data-midi="'+n+'"],.pkey[data-midi="'+n+'"]');if(el)el.classList.add('pressed')}}});
document.addEventListener('keyup',e=>{const n=keyMap[e.key.toLowerCase()];if(n&&keysDown.has(e.key)){keysDown.delete(e.key);noteOff(n);
    const el=document.querySelector('.key[data-midi="'+n+'"],.pkey[data-midi="'+n+'"]');if(el)el.classList.remove('pressed')}});
document.addEventListener('keydown',e=>{if(!e.ctrlKey)return;
  if(e.key==='z'&&calibStep===2){e.preventDefault();undoFp()}
  if(e.key==='y'&&calibStep===2){e.preventDefault();redoFp()}});

// --- SVG FLUTE ---
// Gradients for flute body materials
function hexDarken(hex,f){const r=parseInt(hex.slice(1,3),16),g=parseInt(hex.slice(3,5),16),b=parseInt(hex.slice(5,7),16);
  return '#'+[r,g,b].map(c=>Math.round(c*f).toString(16).padStart(2,'0')).join('')}
function hexLighten(hex,f){const r=parseInt(hex.slice(1,3),16),g=parseInt(hex.slice(3,5),16),b=parseInt(hex.slice(5,7),16);
  return '#'+[r,g,b].map(c=>Math.min(255,Math.round(c+(255-c)*f)).toString(16).padStart(2,'0')).join('')}
function fluteGrad(g,em){
  const base=(CFG&&CFG.color)?CFG.color:(em==='oca'?'#C47038':'#D4B044');
  const c1=base,c2=hexDarken(base,.85),c3=hexDarken(base,.6),c4=hexDarken(base,.4);
  return '<defs><linearGradient id="wg_'+g+'" x1="0" y1="0" x2="0" y2="1">'+
    '<stop offset="0%" stop-color="'+c1+'"/><stop offset="35%" stop-color="'+c2+'"/>'+
    '<stop offset="70%" stop-color="'+c3+'"/><stop offset="100%" stop-color="'+c4+'"/></linearGradient>'+
    '<linearGradient id="lp_'+g+'" x1="0" y1="0" x2="0" y2="1">'+
    '<stop offset="0%" stop-color="'+hexLighten(base,.25)+'"/><stop offset="40%" stop-color="'+c1+'"/>'+
    '<stop offset="100%" stop-color="'+c4+'"/></linearGradient>'+
    '<linearGradient id="cr_'+g+'" x1="0" y1="0" x2="0" y2="1">'+
    '<stop offset="0%" stop-color="'+c2+'"/><stop offset="50%" stop-color="'+c3+'"/>'+
    '<stop offset="100%" stop-color="'+c4+'"/></linearGradient>'+
    '<linearGradient id="eh_'+g+'" x1="0" y1="0" x2="0" y2="1">'+
    '<stop offset="0%" stop-color="#1A1008"/><stop offset="100%" stop-color="#0A0600"/></linearGradient></defs>'
}

// Draw mouthpiece based on embouchure type. ox = x offset for air system integration
function fluteMouth(g,em,ty,by,th,cy,ox){
  ox=ox||0;
  let m='';const lip=10,ar=th-lip;
  if(em==='naf'){
    // Amerindienne: bec (arc 90° centre=coin bas-gauche, sweep=CCW, lip en haut) + bloc oiseau + chanfrain
    m+='<path d="M'+(ox+4)+','+ty+' L'+(ox+58)+','+ty+' L'+(ox+58)+','+by+' L'+(ox+4+ar)+','+by+' A'+ar+','+ar+' 0 0,0 '+(ox+4)+','+(by-ar)+' L'+(ox+4)+','+ty+' Z" fill="url(#wg_'+g+')" stroke="#5C4A0A" stroke-width="1.2"/>';
    m+='<rect x="'+(ox+4+ar/2)+'" y="'+ty+'" width="'+(54-ar/2)+'" height="5" rx="0" fill="#D4B044" opacity=".15"/>';
    // Bloc oiseau/fetiche juste avant le chanfrain (plus large, dessus courbe vers le bas)
    const bx1=ox+36,bx2=ox+54,byt=ty-7,byb=ty+2;
    m+='<path d="M'+bx1+','+byb+' L'+bx1+','+byt+' Q'+((bx1+bx2)/2)+','+(byt+5)+' '+bx2+','+byt+' L'+bx2+','+byb+' Z" fill="url(#cr_'+g+')" stroke="#5C4A0A" stroke-width=".8"/>';
    // Chanfrain (fente d'air entre bloc oiseau et embouchure)
    m+='<rect x="'+(ox+50)+'" y="'+(ty-2)+'" width="10" height="5" rx="1" fill="url(#eh_'+g+')" stroke="#3D2A08" stroke-width=".8"/>'
  }else if(em==='bec'||em==='end'){
    // Bec / end-blown: arc 90° (centre=coin bas-gauche, sweep=CCW, lip en haut) + chanfrain haut-droite
    m+='<path d="M'+(ox+4)+','+ty+' L'+(ox+58)+','+ty+' L'+(ox+58)+','+by+' L'+(ox+4+ar)+','+by+' A'+ar+','+ar+' 0 0,0 '+(ox+4)+','+(by-ar)+' L'+(ox+4)+','+ty+' Z" fill="url(#wg_'+g+')" stroke="#5C4A0A" stroke-width="1.2"/>';
    m+='<rect x="'+(ox+4+ar/2)+'" y="'+ty+'" width="'+(54-ar/2)+'" height="5" rx="0" fill="#D4B044" opacity=".15"/>';
    // Chanfrain (petit rect noir en haut-droite du bloc embouchure)
    m+='<rect x="'+(ox+50)+'" y="'+(ty-2)+'" width="10" height="6" rx="1" fill="url(#eh_'+g+')" stroke="#3D2A08" stroke-width=".8"/>'
  }else{
    // Traversiere: rectangle embouchure + trou rond centre partie haute
    m+='<rect x="'+(ox+4)+'" y="'+(ty-2)+'" width="56" height="'+(th+4)+'" rx="2" fill="url(#wg_'+g+')" stroke="#5C4A0A" stroke-width="1.2"/>';
    m+='<rect x="'+(ox+4)+'" y="'+(ty-2)+'" width="56" height="5" rx="1" fill="#D4B044" opacity=".15"/>';
    // Bague de jonction tete/corps
    m+='<rect x="'+(ox+58)+'" y="'+(ty-3)+'" width="5" height="'+(th+6)+'" rx="1" fill="#A8862A" stroke="#5C4A0A" stroke-width=".6" opacity=".7"/>';
    // Trou rond d\'embouchure centre dans la partie haute
    m+='<circle cx="'+(ox+32)+'" cy="'+(ty+4)+'" r="6" fill="url(#eh_'+g+')" stroke="#3D2A08" stroke-width="1"/>';
    m+='<circle cx="'+(ox+31)+'" cy="'+(ty+3)+'" r="3" fill="none" stroke="#EDD580" stroke-width=".4" opacity=".3"/>'
  }
  return m
}

// Compute air system width needed to the left of the flute
function airSystemWidth(){
  if(!CFG||!CFG.show_air)return 0;
  const m=CFG.air_mode||0;
  // servo air(40) + gap(10) + valve(30) + gap(10) = 90 base
  let w=90;
  if(m>=3)w+=50; // reservoir column (pump below, balloon above)
  else if(m>=2)w+=45; // pump only
  return w+10; // small margin
}

// Draw integrated air system overlay inside the flute SVG
// ox = x offset where air system starts, em = embouchure type
// Returns SVG string. Connects to flute embouchure at (fluteX, connectY)
function buildAirOverlay(g,em,ox,fluteX,ty,by,cy,svgH){
  if(!CFG||!CFG.show_air)return '';
  const m=CFG.air_mode||0;
  const isTrav=em==='trav';
  let s='<defs><linearGradient id="agMetal_'+g+'" x1="0" y1="0" x2="0" y2="1">'+
    '<stop offset="0%" stop-color="#8899aa"/><stop offset="100%" stop-color="#556677"/></linearGradient>'+
    '<linearGradient id="agBalloon_'+g+'" x1="0" y1="0" x2="0" y2="1">'+
    '<stop offset="0%" stop-color="#e07050"/><stop offset="100%" stop-color="#a03020"/></linearGradient></defs>';
  // Layout: pump/balloon column on left, then valve, then servo air, then connects to flute
  // Servo air box is always just before the flute embouchure
  const saW=36,saH=30; // servo air box size
  const vlW=26,vlH=26; // valve box size
  let curX=fluteX; // build from right to left

  // -- Servo Airflow with 120° arc gauge (just before embouchure) --
  const saX=curX-saW-8;
  const saY=cy-saH/2;
  s+='<rect x="'+saX+'" y="'+saY+'" width="'+saW+'" height="'+saH+'" rx="4" fill="url(#agMetal_'+g+')" stroke="#334" stroke-width="1.2"/>';
  // Arc gauge: 120° sweep from -150° to -30° (bottom-centered)
  const gcx=saX+saW/2,gcy=cy+2,gr=11;
  const a1=-150*Math.PI/180,a2=-30*Math.PI/180;
  s+='<path d="M'+(gcx+gr*Math.cos(a1))+','+(gcy+gr*Math.sin(a1))+' A'+gr+','+gr+' 0 0,1 '+(gcx+gr*Math.cos(a2))+','+(gcy+gr*Math.sin(a2))+'" fill="none" stroke="#556" stroke-width="1.5" stroke-linecap="round"/>';
  // Tick marks at 0%, 50%, 100%
  for(let t=0;t<=2;t++){const ta=a1+(a2-a1)*t/2;const tx1=gcx+(gr+2)*Math.cos(ta),ty1=gcy+(gr+2)*Math.sin(ta);
    const tx2=gcx+(gr+4)*Math.cos(ta),ty2=gcy+(gr+4)*Math.sin(ta);
    s+='<line x1="'+tx1+'" y1="'+ty1+'" x2="'+tx2+'" y2="'+ty2+'" stroke="#888" stroke-width=".8"/>';}
  // Needle (default at 50% = -90°)
  s+='<path id="airServoNeedle" d="M'+gcx+','+gcy+' L'+gcx+','+(gcy-gr+1)+'" stroke="#e94" stroke-width="2" stroke-linecap="round"/>';
  s+='<circle cx="'+gcx+'" cy="'+gcy+'" r="2" fill="#e94"/>';
  s+='<text x="'+(saX+saW/2)+'" y="'+(saY+saH+10)+'" text-anchor="middle" style="font-size:7px;fill:#9aa">Servo Air</text>';

  // -- Tube: servo air -> flute embouchure --
  if(isTrav){
    // Traversiere: air goes up from servo, across above the flute, then down into embouchure hole
    const holeX=fluteX+28, holeY=ty+4;
    const aboveY=ty-16; // tube runs above the flute
    // Up from servo air to above level
    s+='<line x1="'+(saX+saW/2)+'" y1="'+saY+'" x2="'+(saX+saW/2)+'" y2="'+aboveY+'" stroke="#7799bb" stroke-width="3" stroke-linecap="round" opacity=".6"/>';
    // Horizontal across above the flute to hole position
    s+='<line x1="'+(saX+saW/2)+'" y1="'+aboveY+'" x2="'+holeX+'" y2="'+aboveY+'" stroke="#7799bb" stroke-width="3" stroke-linecap="round" opacity=".6"/>';
    // Down arrow into embouchure hole
    s+='<line x1="'+holeX+'" y1="'+aboveY+'" x2="'+holeX+'" y2="'+(holeY-7)+'" stroke="#7799bb" stroke-width="3" stroke-linecap="round" opacity=".6"/>';
    s+='<polygon points="'+(holeX-3)+','+(holeY-10)+' '+holeX+','+(holeY-5)+' '+(holeX+3)+','+(holeY-10)+'" fill="#7799bb" opacity=".6"/>';
  }else{
    // Other types: air arrives horizontally from the left into the bec
    s+='<line x1="'+(saX+saW)+'" y1="'+cy+'" x2="'+(fluteX-2)+'" y2="'+cy+'" stroke="#7799bb" stroke-width="3" stroke-linecap="round" opacity=".6"/>';
    s+='<polygon points="'+(fluteX-7)+','+(cy-3)+' '+(fluteX-2)+','+cy+' '+(fluteX-7)+','+(cy+3)+'" fill="#7799bb" opacity=".6"/>';
  }

  // -- Valve --
  const vlX=saX-vlW-12;
  const vlY=cy-vlH/2;
  s+='<rect id="airValveRect" x="'+vlX+'" y="'+vlY+'" width="'+vlW+'" height="'+vlH+'" rx="3" fill="#444" stroke="#667" stroke-width="1.2"/>';
  s+='<rect id="airValveInd" x="'+(vlX+8)+'" y="'+(vlY+5)+'" width="10" height="'+vlH*0.6+'" rx="2" fill="#e44"/>';
  const vlLabel=m===0?'Solenoide':(CFG.valve_servo?'Servo':'Valve');
  s+='<text x="'+(vlX+vlW/2)+'" y="'+(vlY+vlH+10)+'" text-anchor="middle" style="font-size:7px;fill:#9aa">'+vlLabel+'</text>';
  // Tube: valve -> servo air
  s+='<line x1="'+(vlX+vlW)+'" y1="'+cy+'" x2="'+saX+'" y2="'+cy+'" stroke="#7799bb" stroke-width="3" stroke-linecap="round" opacity=".6"/>';
  s+='<polygon points="'+(saX-5)+','+(cy-3)+' '+saX+','+cy+' '+(saX-5)+','+(cy+3)+'" fill="#7799bb" opacity=".6"/>';

  // -- Pump + Reservoir column --
  if(m>=2){
    // Pump at bottom-left (clickable to toggle on/off)
    const pW=40,pH=32;
    const pX=m>=3?ox:vlX-pW-12;
    const pY=cy+8;
    s+='<g class="pump-toggle'+(pumpDisabled?' pump-off':'')+'" onclick="togglePump()">';
    s+='<rect x="'+pX+'" y="'+pY+'" width="'+pW+'" height="'+pH+'" rx="4" fill="url(#agMetal_'+g+')" stroke="#334" stroke-width="1.2"/>';
    s+='<circle id="airPumpIcon" cx="'+(pX+pW/2)+'" cy="'+(pY+pH/2)+'" r="10" fill="none" stroke="#dde" stroke-width="1.2"/>';
    s+='<line id="airPumpBlade" x1="'+(pX+pW/2)+'" y1="'+(pY+pH/2-8)+'" x2="'+(pX+pW/2)+'" y2="'+(pY+pH/2+8)+'" stroke="#dde" stroke-width="1.5"/>';
    // Red cross when disabled
    s+='<g id="pumpCrossOff" style="display:'+(pumpDisabled?'':'none')+'">';
    s+='<line x1="'+(pX+8)+'" y1="'+(pY+6)+'" x2="'+(pX+pW-8)+'" y2="'+(pY+pH-6)+'" stroke="#e44" stroke-width="3" stroke-linecap="round"/>';
    s+='<line x1="'+(pX+pW-8)+'" y1="'+(pY+6)+'" x2="'+(pX+8)+'" y2="'+(pY+pH-6)+'" stroke="#e44" stroke-width="3" stroke-linecap="round"/>';
    s+='</g></g>';
    s+='<text x="'+(pX+pW/2)+'" y="'+(pY+pH+10)+'" text-anchor="middle" style="font-size:7px;fill:#9aa">Pompe</text>';

    if(m>=3){
      // Reservoir balloon above pump
      const bx=pX+pW/2, ballY=cy-18;
      const brx=22, bry=18;
      s+='<ellipse id="airBalloon" cx="'+bx+'" cy="'+ballY+'" rx="'+brx+'" ry="'+bry+'" fill="url(#agBalloon_'+g+')" stroke="#802010" stroke-width="1.2" opacity=".85"/>';
      s+='<ellipse cx="'+(bx-6)+'" cy="'+(ballY-7)+'" rx="6" ry="4" fill="#fff" opacity=".15"/>';
      s+='<text x="'+bx+'" y="'+(ballY+4)+'" text-anchor="middle" style="font-size:9px;fill:#fff;font-weight:bold" id="airBalloonPct">--%</text>';
      // ToF sensor above balloon
      const tofY=ballY-bry-14;
      s+='<rect x="'+(bx-5)+'" y="'+tofY+'" width="10" height="7" rx="2" fill="#334" stroke="#556" stroke-width=".7"/>';
      s+='<line x1="'+bx+'" y1="'+(tofY+7)+'" x2="'+bx+'" y2="'+(ballY-bry+2)+'" stroke="#5af" stroke-width="1" stroke-dasharray="2,2"/>';
      s+='<text x="'+(bx+8)+'" y="'+(tofY+5)+'" style="font-size:6px;fill:#5af">ToF</text>';
      // Tube: pump -> balloon (vertical)
      s+='<line x1="'+bx+'" y1="'+pY+'" x2="'+bx+'" y2="'+(ballY+bry)+'" stroke="#7799bb" stroke-width="3" stroke-linecap="round" opacity=".6"/>';
      // Tube: balloon -> valve (horizontal)
      s+='<line x1="'+(bx+brx)+'" y1="'+ballY+'" x2="'+(vlX+vlW/2)+'" y2="'+ballY+'" stroke="#7799bb" stroke-width="3" stroke-linecap="round" opacity=".6"/>';
      // Bend down to valve center
      s+='<line x1="'+(vlX+vlW/2)+'" y1="'+ballY+'" x2="'+(vlX+vlW/2)+'" y2="'+vlY+'" stroke="#7799bb" stroke-width="3" stroke-linecap="round" opacity=".6"/>';
      s+='<polygon points="'+(vlX+vlW/2-3)+','+(vlY-1)+' '+(vlX+vlW/2)+','+(vlY+3)+' '+(vlX+vlW/2+3)+','+(vlY-1)+'" fill="#7799bb" opacity=".6"/>';
      s+='<text x="'+bx+'" y="'+(ballY+bry+20)+'" text-anchor="middle" style="font-size:7px;fill:#9aa">Reservoir</text>';
    }else{
      // Mode 2: pump direct to valve (horizontal tube)
      s+='<line x1="'+(pX+pW)+'" y1="'+(pY+pH/2)+'" x2="'+vlX+'" y2="'+cy+'" stroke="#7799bb" stroke-width="3" stroke-linecap="round" opacity=".6"/>';
      s+='<polygon points="'+(vlX-5)+','+(cy-3)+' '+vlX+','+cy+' '+(vlX-5)+','+(cy+3)+'" fill="#7799bb" opacity=".6"/>';
    }
  }
  return s;
}

function buildFlute(cfg,svgId,showNums){
  const svg=$(svgId);if(!svg||!cfg)return;
  const nf=cfg.num_fingers||6,fingers=cfg.fingers||[];
  const em=cfg.embouchure||'trav';
  // Ocarina = forme speciale
  if(em==='oca'){buildOcarina(cfg,svgId,showNums);return}
  const airOx=airSystemWidth();
  const sp=50,sx=airOx+100,r=14;
  const topHoles=[],botHoles=[];
  for(let i=0;i<nf;i++){(fingers[i]&&fingers[i].th?botHoles:topHoles).push(i)}
  const posTop=topHoles.map((_,i)=>sx+i*sp);
  const posBot=botHoles.map((_,i)=>sx+i*sp);
  const allX=[...posTop,...posBot,sx+200];
  const tw=Math.max(...allX)+60;
  const svgH=CFG&&CFG.show_air&&(CFG.air_mode||0)>=3?130:100;
  const h_top=svgH===130?55:35,h_bot=svgH===130?85:65,cy=svgH===130?70:50;
  svg.setAttribute('viewBox','0 0 '+tw+' '+svgH);
  const g=svgId,ty=cy-16,by=cy+16,th=by-ty;
  let h=fluteGrad(g,em);
  // Air system overlay (drawn first, behind flute)
  const fluteStartX=airOx+4;
  h+=buildAirOverlay(g,em,0,fluteStartX,ty,by,cy,svgH);
  // Corps du tube (demarre apres l'embouchure)
  const bx=fluteStartX+(em==='trav'?59:54);
  h+='<rect x="'+bx+'" y="'+ty+'" width="'+(tw-bx-10)+'" height="'+th+'" rx="0" fill="url(#wg_'+g+')" stroke="#5C4A0A" stroke-width="1.5"/>';
  h+='<rect x="'+bx+'" y="'+ty+'" width="'+(tw-bx-10)+'" height="6" rx="0" fill="#D4B044" opacity=".18"/>';
  // Embouchure adaptative (offset by air system width)
  h+=fluteMouth(g,em,ty,by,th,cy,fluteStartX);
  // Type label
  const emLabels={trav:'Traversiere',bec:'A bec',naf:'Amerindienne',end:'Embouchure libre'};
  h+='<text x="'+(tw-20)+'" y="'+(svgH-6)+'" text-anchor="end" style="font-size:9px;fill:#667;font-style:italic">'+(emLabels[em]||'')+'</text>';
  // Top holes
  topHoles.forEach((fi,i)=>{
    h+='<circle id="fh_'+svgId+'_'+fi+'" cx="'+posTop[i]+'" cy="'+h_top+'" r="'+r+'" class="flute-hole closed"/>';
    if(showNums)h+='<text x="'+posTop[i]+'" y="'+(h_top+4)+'" text-anchor="middle" class="flute-num">'+(fi+1)+'</text>'
  });
  // Bottom holes (thumb)
  botHoles.forEach((fi,i)=>{
    h+='<circle id="fh_'+svgId+'_'+fi+'" cx="'+posBot[i]+'" cy="'+h_bot+'" r="'+(r-2)+'" class="flute-hole closed thumb"/>';
    if(showNums)h+='<text x="'+posBot[i]+'" y="'+(h_bot+4)+'" text-anchor="middle" class="flute-num">'+(fi+1)+'</text>'
  });
  if(botHoles.length>0)h+='<text x="'+(posBot[0])+'" y="'+(h_bot+20)+'" text-anchor="middle" class="flute-lbl">Pouce</text>';
  svg.innerHTML=h
}

// Ocarina: corps ovale en ceramique avec arrangement compact des trous
function buildOcarina(cfg,svgId,showNums){
  const svg=$(svgId);if(!svg||!cfg)return;
  const nf=cfg.num_fingers||4,fingers=cfg.fingers||[];
  const g=svgId;
  // Vue de dessus: forme oeuf/larme + embouchure courte avec chanfrein centre
  const bw=Math.min(160,Math.max(100,nf*22+20));// largeur corps
  const bh=Math.min(80,Math.max(55,nf*5+40));   // hauteur corps
  const cx=bw/2+40,cy=50;  // centre du corps
  const mw=28;              // longueur embouchure (courte)
  const tw=cx+bw/2+20;
  svg.setAttribute('viewBox','0 0 '+tw+' 100');
  let h=fluteGrad(g,'oca');
  // Corps: forme oeuf (plus large a droite, pointu a gauche vers embouchure)
  const rx1=bw/2,ry1=bh/2;
  // Path oeuf: demi-cercle droite large, gauche plus pointu
  const lx=cx-rx1,rxp=cx+rx1;
  h+='<path d="M'+cx+','+(cy-ry1)+' C'+(rxp+10)+','+(cy-ry1)+' '+(rxp+10)+','+(cy+ry1)+' '+cx+','+(cy+ry1)+
    ' C'+(lx+15)+','+(cy+ry1)+' '+(lx-5)+','+(cy+8)+' '+(lx-5)+','+cy+
    ' C'+(lx-5)+','+(cy-8)+' '+(lx+15)+','+(cy-ry1)+' '+cx+','+(cy-ry1)+
    ' Z" fill="url(#wg_'+g+')" stroke="#5C2810" stroke-width="1.8"/>';
  // Reflet ceramique
  h+='<ellipse cx="'+(cx+15)+'" cy="'+(cy-ry1*0.35)+'" rx="'+(rx1*0.5)+'" ry="'+(ry1*0.3)+'" fill="#D88050" opacity=".1"/>';
  // Embouchure: petit bec centre qui sort a gauche du corps
  const mx=lx-5,my=cy;// point de sortie
  h+='<path d="M'+mx+','+(my-6)+' L'+(mx-mw)+','+(my-4)+' Q'+(mx-mw-4)+','+my+' '+(mx-mw)+','+(my+4)+
    ' L'+mx+','+(my+6)+' Z" fill="url(#lp_'+g+')" stroke="#5C2810" stroke-width="1"/>';
  // Chanfrein au centre de l'embouchure (fente d'air)
  const chx=mx-mw/2;
  h+='<ellipse cx="'+chx+'" cy="'+my+'" rx="5" ry="2" fill="url(#eh_'+g+')" stroke="#3D2A08" stroke-width=".6"/>';
  // Trous: repartis sur le corps vu de dessus
  // Impair=rangee haute, pair=rangee basse, pouces en bas
  const topRow=[],botRow=[];
  for(let i=0;i<nf;i++){
    if(fingers[i]&&fingers[i].th){botRow.push(i)}
    else if(i%2===0){topRow.push(i)}
    else{botRow.push(i)}
  }
  const hsp=Math.min(28,bw*0.7/Math.max(Math.max(topRow.length,botRow.length),1));
  // Rangee haute (index, majeur, etc.)
  if(topRow.length){
    const tsx=cx-(topRow.length-1)*hsp/2;
    topRow.forEach((fi,i)=>{
      const px=tsx+i*hsp;
      h+='<circle id="fh_'+svgId+'_'+fi+'" cx="'+px+'" cy="'+(cy-ry1*0.32)+'" r="8" class="flute-hole closed"/>';
      if(showNums)h+='<text x="'+px+'" y="'+(cy-ry1*0.32+3)+'" text-anchor="middle" class="flute-num">'+(fi+1)+'</text>'
    })}
  // Rangee basse (annulaire, auriculaire, pouces)
  if(botRow.length){
    const bsx=cx-(botRow.length-1)*hsp/2;
    botRow.forEach((fi,i)=>{
      const px=bsx+i*hsp;const isThumb=fingers[fi]&&fingers[fi].th;
      h+='<circle id="fh_'+svgId+'_'+fi+'" cx="'+px+'" cy="'+(cy+ry1*0.32)+'" r="'+(isThumb?6:8)+'" class="flute-hole closed'+(isThumb?' thumb':'')+'"/>';
      if(showNums)h+='<text x="'+px+'" y="'+(cy+ry1*0.32+3)+'" text-anchor="middle" class="flute-num">'+(fi+1)+'</text>'
    })}
  h+='<text x="'+(tw-10)+'" y="94" text-anchor="end" style="font-size:9px;fill:#667;font-style:italic">Ocarina</text>';
  svg.innerHTML=h
}

function updateFluteForNote(midi){
  if(!CFG)return;const nd=CFG.notes.find(n=>n.midi===midi);
  for(let i=0;i<CFG.num_fingers;i++){const el=$('fh_fluteSvg_'+i);
    if(el){const v=nd?nd.fp[i]:0;el.setAttribute('class','flute-hole '+(v===1?'open':v===2?'half':'closed')+(CFG.fingers[i]&&CFG.fingers[i].th?' thumb':''))}}
  $('fluteNote').textContent=nd?mn(nd.midi):'-';$('fluteInfo').textContent=nd?'MIDI '+nd.midi:''
}

// --- MIDI FILE MANAGEMENT ---
const dz=$('dropZone');
dz.addEventListener('dragover',e=>{e.preventDefault();dz.classList.add('hover')});
dz.addEventListener('dragleave',()=>dz.classList.remove('hover'));
dz.addEventListener('drop',e=>{e.preventDefault();dz.classList.remove('hover');
  if(e.dataTransfer.files.length)validateAndUpload(e.dataTransfer.files[0])});
function uploadMidi(input){if(input.files.length)validateAndUpload(input.files[0])}
function validateAndUpload(file){
  // Verifier extension
  const name=file.name.toLowerCase();
  if(!name.endsWith('.mid')&&!name.endsWith('.midi')){
    showToast('Fichier invalide : extension .mid ou .midi requise','error');return}
  // Verifier magic bytes MThd
  const reader=new FileReader();
  reader.onload=()=>{
    const arr=new Uint8Array(reader.result);
    if(arr.length<4||arr[0]!==0x4D||arr[1]!==0x54||arr[2]!==0x68||arr[3]!==0x64){
      showToast('Fichier invalide : pas un fichier MIDI (MThd absent)','error');return}
    uploadMidiFile(file)};
  reader.readAsArrayBuffer(file.slice(0,4))
}
function uploadMidiFile(file){
  const fd=new FormData();fd.append('file',file);
  const ub=$('uploadBar'),uf=$('uploadFill');ub.style.display='block';uf.style.width='0%';
  const xhr=new XMLHttpRequest();
  xhr.upload.onprogress=e=>{if(e.lengthComputable)uf.style.width=(e.loaded/e.total*100)+'%'};
  xhr.onload=()=>{uf.style.width='100%';setTimeout(()=>ub.style.display='none',1000);
    try{const d=JSON.parse(xhr.responseText);if(d.ok){showToast('Upload OK: '+d.events+' evt','success');addLog('Upload OK');loadMidiList()}
    else{showToast('Erreur: '+(d.msg||'echec'),'error')}}catch(e){showToast('Erreur upload','error')}};
  xhr.onerror=()=>{ub.style.display='none';showToast('Erreur upload reseau','error')};
  xhr.open('POST','/api/midi');xhr.send(fd)
}
function setMidiCh(v){wsSend({t:'ch_filter',ch:parseInt(v)})}

function loadMidiList(){
  fetch('/api/midi/list').then(r=>r.json()).then(d=>{
    updateMidiStorage(d.used,d.limit);
    const list=$('midiFileList');list.innerHTML='';
    if(!d.files||!d.files.length){list.innerHTML='<div style="font-size:.78em;color:#666">Aucun fichier</div>';return}
    d.files.forEach(f=>{
      const row=document.createElement('div');
      row.style.cssText='display:flex;align-items:center;gap:6px;padding:4px 0;border-bottom:1px solid #333';
      const isLoaded=d.loaded&&d.loaded===f.name;
      const name=document.createElement('span');
      name.textContent=f.name;name.style.cssText='flex:1;font-size:.82em;cursor:pointer;'+(isLoaded?'color:#4ecca3;font-weight:bold':'color:#ccc');
      name.onclick=()=>loadMidiFile(f.name);
      const size=document.createElement('span');
      size.textContent=(f.size/1024).toFixed(1)+'KB';size.style.cssText='font-size:.72em;color:#888';
      const del=document.createElement('button');
      del.textContent='\u2715';del.style.cssText='background:none;border:1px solid #555;color:#e94560;border-radius:4px;padding:1px 6px;cursor:pointer;font-size:.72em';
      del.onclick=()=>deleteMidiFile(f.name);
      row.appendChild(name);row.appendChild(size);row.appendChild(del);list.appendChild(row)
    })
  }).catch(()=>{})
}
function updateMidiStorage(used,limit){
  const pct=limit>0?Math.min(100,used/limit*100):0;
  $('midiStorageFill').style.width=pct+'%';
  $('midiStorageFill').style.background=pct>90?'#e94560':pct>70?'#e9a645':'#4ecca3';
  $('midiStorageText').textContent=(used/1024|0)+' / '+(limit/1024|0)+' KB'
}
function deleteMidiFile(name){
  if(!confirm('Supprimer '+name+' ?'))return;
  fetch('/api/midi/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({file:name})})
    .then(r=>r.json()).then(d=>{if(d.ok){showToast('Supprime','success');loadMidiList()}else showToast(d.msg||'Erreur','error')})
    .catch(()=>showToast('Erreur reseau','error'))
}
function loadMidiFile(name){
  fetch('/api/midi/load',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({file:name})})
    .then(r=>r.json()).then(d=>{
      if(d.ok){showToast('Charge: '+d.events+' evt','success');loadMidiList()}
      else showToast(d.msg||'Erreur','error')
    }).catch(()=>showToast('Erreur reseau','error'))
}

// --- SEQUENCE EDITOR MODAL ---
function openSeqModal(){$('seqModal').classList.add('show');drawSeqGrid();updateSeqMemInfo()}
function closeSeqModal(){$('seqModal').classList.remove('show')}
function updateSeqMemInfo(){
  // Estimate memory: ~4 bytes per note event (step + noteIdx)
  const used=seqNotes.length*4;const max=200*1024;
  const el=$('seqMemInfo');if(el)el.textContent='Memoire: '+(used<1024?used+' B':(used/1024).toFixed(1)+' KB')+' / 200 KB';
}

// --- STEP SEQUENCER ---
let seqNotes=[];// [{step,noteIdx}]
const SEQ_NOTE_RANGE=()=>{if(!CFG||!CFG.notes||!CFG.notes.length)return{notes:[60],labels:['C4']};
  const nn=CFG.notes.map(n=>n.midi).sort((a,b)=>a-b);
  return{notes:nn,labels:nn.map(m=>N[m%12]+(Math.floor(m/12)-1))}};

function drawSeqGrid(){
  const svg=$('seqSvg');if(!svg)return;
  const nr=SEQ_NOTE_RANGE(),notes=nr.notes,labels=nr.labels;
  const bars=parseInt($('seqBars').value)||4,steps=bars*8;// 8 steps/bar (croches)
  const cw=22,ch=16,lw=36,pw=lw+steps*cw,ph=notes.length*ch+4;
  svg.setAttribute('viewBox','0 0 '+pw+' '+ph);svg.setAttribute('width',pw);
  let s='';
  // Lignes notes (de l'aigu en haut au grave en bas)
  for(let r=0;r<notes.length;r++){
    const y=r*ch,ni=notes.length-1-r;
    s+='<rect x="0" y="'+y+'" width="'+lw+'" height="'+ch+'" fill="'+(r%2?'#1a1a2e':'#16213e')+'" stroke="#333" stroke-width=".5"/>';
    s+='<text x="'+(lw-3)+'" y="'+(y+ch-4)+'" text-anchor="end" style="font-size:8px;fill:#888">'+labels[ni]+'</text>';
    for(let c=0;c<steps;c++){
      const x=lw+c*cw,isBeat=c%8===0,isHalf=c%4===0;
      const bg=isBeat?'#1a2240':isHalf?'#181e38':(r%2?'#131528':'#11132a');
      const on=seqNotes.some(n=>n.step===c&&n.noteIdx===ni);
      s+='<rect x="'+x+'" y="'+y+'" width="'+cw+'" height="'+ch+'" fill="'+(on?'#4ecdc4':bg)+'" stroke="#333" stroke-width=".3" data-s="'+c+'" data-n="'+ni+'" onclick="toggleSeqNote('+c+','+ni+')"/>';
      if(on)s+='<rect x="'+(x+3)+'" y="'+(y+3)+'" width="'+(cw-6)+'" height="'+(ch-6)+'" rx="2" fill="#3dbdb5" opacity=".6" pointer-events="none"/>'
    }
  }
  // Barres de mesure
  for(let b=0;b<=bars;b++){const x=lw+b*8*cw;
    s+='<line x1="'+x+'" y1="0" x2="'+x+'" y2="'+ph+'" stroke="#666" stroke-width="'+(b===0||b===bars?1.5:.8)+'"/>';
    if(b<bars)s+='<text x="'+(x+4)+'" y="'+(ph-1)+'" style="font-size:7px;fill:#555">'+(b+1)+'</text>'}
  svg.innerHTML=s
}

function toggleSeqNote(step,noteIdx){
  const idx=seqNotes.findIndex(n=>n.step===step&&n.noteIdx===noteIdx);
  if(idx>=0)seqNotes.splice(idx,1);else seqNotes.push({step,noteIdx});
  drawSeqGrid();updateSeqMemInfo()
}
function clearSeq(){seqNotes=[];drawSeqGrid();updateSeqMemInfo()}

function uploadSeqMidi(){
  if(!seqNotes.length){showToast('Sequence vide','error');return}
  const nr=SEQ_NOTE_RANGE(),notes=nr.notes;
  const bpm=parseInt($('seqBpm').value)||120;
  const ticksPerBeat=480,ticksPerStep=ticksPerBeat/2;// croche = 1/2 beat
  // Generer MIDI binaire (format 0, 1 piste)
  const trk=[];
  // Tempo meta event
  const usPerBeat=Math.round(60000000/bpm);
  trk.push({t:0,d:[0xFF,0x51,0x03,(usPerBeat>>16)&0xFF,(usPerBeat>>8)&0xFF,usPerBeat&0xFF]});
  // Notes triees par temps
  const evts=[];
  seqNotes.forEach(n=>{
    const tick=n.step*ticksPerStep,midi=notes[n.noteIdx];
    evts.push({tick,on:true,midi,vel:100});
    evts.push({tick:tick+ticksPerStep-10,on:false,midi,vel:0})});
  evts.sort((a,b)=>a.tick-b.tick||a.on-b.on);
  let lastTick=0;
  evts.forEach(e=>{
    const dt=e.tick-lastTick;lastTick=e.tick;
    const vlq=encVLQ(dt);
    trk.push({t:e.tick,d:[...vlq,e.on?0x90:0x80,e.midi,e.vel]})});
  // End of track
  trk.push({t:lastTick+ticksPerStep,d:[...encVLQ(ticksPerStep),0xFF,0x2F,0x00]});
  // Assembler les bytes de piste
  let trkBytes=[];trk.forEach(e=>trkBytes.push(...e.d));
  // Header MIDI
  const hdr=[0x4D,0x54,0x68,0x64,0,0,0,6,0,0,0,1,...u16(ticksPerBeat)];
  const trkHdr=[0x4D,0x54,0x72,0x6B,...u32(trkBytes.length)];
  const midi=new Uint8Array([...hdr,...trkHdr,...trkBytes]);
  // Upload comme fichier
  const blob=new Blob([midi],{type:'audio/midi'});
  const fd=new FormData();fd.append('file',blob,'sequence.mid');
  const xhr=new XMLHttpRequest();
  xhr.onload=()=>{try{const d=JSON.parse(xhr.responseText);
    if(d.ok){showToast('Sequence chargee','success');wsSend({t:'play'})}
    else showToast('Erreur: '+(d.msg||''),'error')}catch(e){showToast('Erreur','error')}};
  xhr.onerror=()=>showToast('Erreur reseau','error');
  xhr.open('POST','/api/midi');xhr.send(fd)
}
function encVLQ(v){if(v<128)return[v];const b=[];b.unshift(v&0x7F);v>>=7;
  while(v>0){b.unshift((v&0x7F)|0x80);v>>=7}return b}
function u16(v){return[(v>>8)&0xFF,v&0xFF]}
function u32(v){return[(v>>24)&0xFF,(v>>16)&0xFF,(v>>8)&0xFF,v&0xFF]}

// --- CALIBRATION ---
function buildCalibUI(){if(!CFG)return;buildFlute(CFG,'calFluteSvg',true);buildFingerCards();goStep(calibStep)}

function stepStatus(){
  if(!CFG)return[0,0,0,0];
  const s1=CFG.num_fingers>0&&CFG.fingers.length>=CFG.num_fingers;
  const s2=CFG.notes&&CFG.notes.length>0;
  const s3=s2&&CFG.notes.some(n=>n.amn>0||n.amx>0);
  const s4=CFG.air_atk_mode!=null;
  return[s1?1:0,s2?1:0,s3?1:0,s4?1:0]
}
function goStep(s){
  calibStep=s;
  ['step1','step2','step3','step4'].forEach((id,i)=>{const el=$(id);el.style.display=(i+1===s)?'':'none';
    if(i+1===s){el.classList.add('fade-in')}else{el.classList.remove('fade-in')}});
  updStepDots();
  if(s===2){buildPresetSelect();
    const iv=$('instrumentSelect');if(iv&&iv.value){$('presetSelect').value=iv.value;
      // Auto-apply si les notes ne sont pas encore remplies ou viennent d'un autre preset
      if(!CFG.notes.length||CFG._lastInst!==iv.value){applyPreset(iv.value);CFG._lastInst=iv.value}}
    buildFingeringRows();fpHistory=[];fpFuture=[];updUndoUI()}
  if(s===3)buildAirflowRows();
  if(s===4)buildExprUI()
}
function updStepDots(){
  const st=stepStatus();
  document.querySelectorAll('.step-dot').forEach((d,i)=>{
    if(i+1===calibStep)d.className='step-dot active'+(dirty?' modified':'');
    else if(st[i])d.className='step-dot done';
    else d.className='step-dot locked'
  })
}

function changeFingers(delta){
  if(!CFG)return;let nf=CFG.num_fingers+delta;
  if(nf<1)nf=1;if(nf>MAX_FINGERS)nf=MAX_FINGERS;CFG.num_fingers=nf;
  // Add defaults for new fingers
  while(CFG.fingers.length<nf)CFG.fingers.push({ch:CFG.fingers.length,a:90,d:-1,th:0});
  $('numFingersDisp').textContent=nf;buildFingerCards();buildFlute(CFG,'calFluteSvg',true);markDirty()
}

function buildFingerCards(){
  const c=$('fingerCards');c.innerHTML='';if(!CFG)return;
  $('numFingersDisp').textContent=CFG.num_fingers;
  $('angleOpen').value=CFG.angle_open||30;$('aoVal').textContent=(CFG.angle_open||30)+'deg';
  $('halfHolePct').value=CFG.half_hole_pct||50;$('hhVal').textContent=(CFG.half_hole_pct||50)+'%';
  // PCA dropdown
  const sel=$('airPca');sel.innerHTML='';
  for(let i=0;i<16;i++){const o=document.createElement('option');o.value=i;o.textContent='PCA '+i;sel.appendChild(o)}
  sel.value=CFG.air_pca||10;
  // Finger cards
  for(let i=0;i<CFG.num_fingers;i++){
    const f=CFG.fingers[i]||{ch:i,a:90,d:1,th:0};
    const d=document.createElement('div');d.className='cal-card';
    let html='<div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:6px">'+
      '<h4 style="margin:0">Doigt '+(i+1)+'</h4>'+
      '<div style="display:flex;align-items:center;gap:8px">'+
        '<span style="font-size:.75em;color:#888">Pin PCA</span>'+
        '<select id="fch'+i+'" style="max-width:70px" onchange="CFG.fingers['+i+'].ch=parseInt(this.value);checkPca();markDirty()">'+
          Array.from({length:16},(_,j)=>'<option value="'+j+'"'+(j===f.ch?' selected':'')+'>'+j+'</option>').join('')+'</select>'+
        '<select id="fd'+i+'" style="max-width:60px" onchange="CFG.fingers['+i+'].d=parseInt(this.value);markDirty()">'+
          '<option value="1"'+(f.d===1?' selected':'')+'>\u21BB</option><option value="-1"'+(f.d===-1?' selected':'')+'>\u21BA</option></select>'+
      '</div></div>';
    if(i===0&&CFG.embouchure!=='oca') html+='<div class="cfg-row"><label>Pouce (arriere)</label><input type="checkbox" id="fth'+i+'"'+(f.th?' checked':'')+
      ' style="width:auto;flex:0" onchange="CFG.fingers['+i+'].th=this.checked?1:0;buildFlute(CFG,\'calFluteSvg\',true);markDirty()"></div>';
    html+='<div style="margin:6px 0"><div style="display:flex;justify-content:space-between;font-size:.75em;color:#888;margin-bottom:2px">'+
      '<span>Angle ferme</span><span id="fav'+i+'">'+f.a+'&deg;</span></div>'+
      '<input type="range" min="0" max="180" value="'+f.a+'" id="fa'+i+'" style="width:100%"'+
        ' oninput="CFG.fingers['+i+'].a=parseInt(this.value);$(\'fav'+i+'\').textContent=this.value+\'&deg;\';testFinger('+i+',parseInt(this.value))"></div>'+
      '<div class="btn-row"><button class="btn btn-s" onclick="testPulse(this);testFinger('+i+',CFG.fingers['+i+'].a)">Fermer</button>'+
        '<button class="btn btn-s" onclick="testPulse(this);testFinger('+i+',CFG.fingers['+i+'].a+(CFG.angle_open||30)*CFG.fingers['+i+'].d)">Ouvrir</button></div>'+
      '<div class="pca-warn"></div>';
    d.innerHTML=html;c.appendChild(d)
  }
  checkPca();buildInstrumentSelect()
}

function buildInstrumentSelect(){
  const s=$('instrumentSelect');if(!s)return;
  s.innerHTML='<option value="">-- Personnalis\u00e9 --</option>';
  const emNames={trav:'Traversieres',bec:'A bec / sifflets',naf:'Am\u00e9rindiennes',end:'Emb. libre',oca:'Ocarinas'};
  const emOrder=['bec','trav','end','naf','oca'];
  const groups={};PR.forEach(p=>{const k=p.em||'trav';(groups[k]=groups[k]||[]).push(p)});
  emOrder.forEach(em=>{if(!groups[em])return;
    const og=document.createElement('optgroup');og.label=emNames[em]||em;
    groups[em].sort((a,b)=>a.h-b.h).forEach(p=>{const o=document.createElement('option');o.value=p.id;
      o.textContent=p.n+' - '+p.h+' trous';og.appendChild(o)});
    s.appendChild(og)})
}

function selectInstrument(val){
  if(!val||!CFG)return;
  const p=PR.find(x=>x.id===val);if(!p)return;
  // Step 1 = config physique seulement (trous + pouce + embouchure), pas les notes
  CFG.num_fingers=p.h;CFG.embouchure=p.em||'trav';
  while(CFG.fingers.length<p.h)CFG.fingers.push({ch:CFG.fingers.length,a:90,d:-1,th:0});
  CFG.fingers.forEach(f=>f.th=0);
  if(p.th>=0&&CFG.fingers[p.th])CFG.fingers[p.th].th=1;
  // Rebuild UI physique
  buildFingerCards();buildFlute(CFG,'calFluteSvg',true);markDirty();
  showToast(p.n+' - '+p.h+' trous'+(p.th>=0?' (pouce)':''),'success')
}

function testFinger(i,a){wsSend({t:'test_finger',i:i,a:parseInt(a)});
  // Update flute SVG
  const el=$('fh_calFluteSvg_'+i);if(el){const closed=CFG.fingers[i].a;const open=Math.abs(a-closed)>(CFG.angle_open||30)/2;
    el.setAttribute('class','flute-hole '+(open?'open':'closed')+(CFG.fingers[i].th?' thumb':''))}}

function saveStep1(){
  if(!CFG)return;btnLoad('btnSaveStep1',true);
  CFG.angle_open=parseInt($('angleOpen').value);CFG.half_hole_pct=parseInt($('halfHolePct').value);CFG.air_pca=parseInt($('airPca').value);
  for(let i=0;i<CFG.num_fingers;i++){
    CFG.fingers[i].ch=parseInt($('fch'+i).value);
    CFG.fingers[i].a=parseInt($('fa'+i).value);
    CFG.fingers[i].d=parseInt($('fd'+i).value);
    const thEl=$('fth'+i);CFG.fingers[i].th=thEl?thEl.checked?1:0:0
  }
  const body={num_fingers:CFG.num_fingers,air_pca:CFG.air_pca,angle_open:CFG.angle_open,half_hole_pct:CFG.half_hole_pct,embouchure:CFG.embouchure||'trav',fingers:CFG.fingers.slice(0,CFG.num_fingers)};
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(r=>r.json()).then(d=>{btnLoad('btnSaveStep1',false);if(d.ok){showToast('Doigts sauvegardes','success');markClean();buildFlute(CFG,'fluteSvg',false);goStep(2)}else showToast('Erreur sauvegarde','error')})
    .catch(e=>{btnLoad('btnSaveStep1',false);showToast('Erreur: '+e,'error')})
}

// --- STEP 2: FINGERINGS ---
function buildFingeringRows(){
  const c=$('fingeringRows');c.innerHTML='';if(!CFG)return;let lastOct=-1;
  CFG.notes.forEach((n,ni)=>{
    const oct=Math.floor(n.midi/12)-1;if(oct!==lastOct){lastOct=oct;
      const sep=document.createElement('div');sep.className='fg-octave';sep.textContent='Octave '+oct;c.appendChild(sep)}
    const d=document.createElement('div');d.className='fg-row fade-in fade-delay-'+(Math.min(4,(ni%4)+1));
    let dots='';for(let f=0;f<CFG.num_fingers;f++){
      const isThumb=CFG.fingers[f]&&CFG.fingers[f].th;
      dots+='<div class="fg-dot '+fpClass(n.fp[f])+(isThumb?' thumb':'')+'" data-ni="'+ni+'" data-fi="'+f+'" onclick="toggleFP('+ni+','+f+',this)"></div>'
    }
    d.innerHTML='<input type="number" class="fg-midi" style="width:48px" value="'+n.midi+'" min="0" max="127" onchange="fpSnap();CFG.notes['+ni+'].midi=parseInt(this.value);markDirty()">'+
      '<span class="fg-note">'+mn(n.midi)+'</span>'+
      '<div class="fg-dots">'+dots+'</div>'+
      '<button class="btn btn-s" style="padding:4px 8px;font-size:.75em" onclick="testPulse(this);wsSend({t:\'test_note\',n:'+n.midi+'})">Test</button>';
    c.appendChild(d)
  })
}

function fpClass(v){return v===1?'open':v===2?'half':'closed'}
function kfClass(v){return v===1?'o':v===2?'h':'c'}
function toggleFP(ni,fi,el){
  fpSnap();const v=CFG.notes[ni].fp[fi];CFG.notes[ni].fp[fi]=v===0?1:v===1?2:0;
  el.className='fg-dot '+fpClass(CFG.notes[ni].fp[fi])+(CFG.fingers[fi]&&CFG.fingers[fi].th?' thumb':'');markDirty()
}

function addNote(){
  if(!CFG)return;fpSnap();const last=CFG.notes.length?CFG.notes[CFG.notes.length-1]:null;
  const midi=last?last.midi+2:84;const fp=new Array(CFG.num_fingers).fill(0);
  CFG.notes.push({midi:midi,fp:fp,amn:20,amx:75});CFG.num_notes=CFG.notes.length;buildFingeringRows();markDirty()
}
function removeLastNote(){if(!CFG||!CFG.notes.length)return;fpSnap();CFG.notes.pop();CFG.num_notes=CFG.notes.length;buildFingeringRows();markDirty()}

function buildPresetSelect(){
  const s=$('presetSelect');if(!s||!CFG)return;
  s.innerHTML='<option value="">-- Personnalis\u00e9 --</option>';
  const nf=CFG.num_fingers;
  // Uniquement les presets compatibles (meme nombre de trous)
  const compat=PR.filter(p=>p.h===nf);
  if(compat.length){
    const og=document.createElement('optgroup');og.label='Accordages '+nf+' trous';
    compat.forEach(p=>{const o=document.createElement('option');o.value=p.id;
      o.textContent=p.n+' - '+p.d.length+' notes ('+mn(p.d[0][0])+'\u2192'+mn(p.d[p.d.length-1][0])+')';og.appendChild(o)});
    s.appendChild(og)}
  updPresetInfo()
}

function updPresetInfo(){
  let el=$('presetInfo');
  if(!el){const p=$('presetSelect');if(!p)return;el=document.createElement('div');el.id='presetInfo';
    el.style.cssText='font-size:.8em;color:#aaa;margin-top:4px;padding:6px 10px;background:rgba(255,255,255,.04);border-radius:6px';
    p.parentNode.appendChild(el)}
  const val=$('presetSelect').value;
  if(!val){el.innerHTML=CFG&&CFG.notes.length?'<b>Personnalis\u00e9</b> - '+CFG.notes.length+' notes ('+mn(CFG.notes[0].midi)+'\u2192'+mn(CFG.notes[CFG.notes.length-1].midi)+')':'Selectionnez un preset ou ajoutez des notes manuellement';return}
  const p=PR.find(x=>x.id===val);if(!p){el.innerHTML='';return}
  const lo=mn(p.d[0][0]),hi=mn(p.d[p.d.length-1][0]);
  el.innerHTML='<b>'+esc(p.n)+'</b> - '+p.d.length+' notes, de '+lo+' a '+hi+(p.th>=0?' (pouce doigt '+(p.th+1)+')':'')
}

function applyPreset(val){
  if(!val||!CFG)return;
  const p=PR.find(x=>x.id===val);if(!p)return;
  CFG.embouchure=p.em||'trav';
  // Build notes from preset data
  CFG.notes=p.d.map(n=>({midi:n[0],fp:[...n[1]],amn:n[2],amx:n[3]}));
  CFG.notes.forEach(n=>{while(n.fp.length<CFG.num_fingers)n.fp.push(0)});
  CFG.num_notes=CFG.notes.length;
  fpSnap();buildFingeringRows();buildFlute(CFG,'calFluteSvg',true);updPresetInfo();markDirty()
}

function saveStep2(){
  if(!CFG)return;btnLoad('btnSaveStep2',true);
  const body={num_fingers:CFG.num_fingers,notes:CFG.notes.map(n=>({midi:n.midi,fp:n.fp.slice(0,CFG.num_fingers),amn:n.amn,amx:n.amx}))};
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(r=>r.json()).then(d=>{btnLoad('btnSaveStep2',false);if(d.ok){showToast('Doigtes sauvegardes','success');markClean();fpHistory=[];fpFuture=[];updUndoUI();goStep(3);buildKeyboard();buildFlute(CFG,'fluteSvg',false)}else showToast('Erreur sauvegarde','error')})
    .catch(e=>{btnLoad('btnSaveStep2',false);showToast('Erreur: '+e,'error')})
}

// --- STEP 3: AIRFLOW ---
function buildAirflowRows(){
  const c=$('airflowRows');c.innerHTML='';if(!CFG)return;
  CFG.notes.forEach((n,ni)=>{
    let dots='';for(let f=0;f<CFG.num_fingers;f++)dots+='<span class="kf '+kfClass(n.fp[f])+'"></span>';
    const d=document.createElement('div');d.className='air-card fade-in fade-delay-'+(Math.min(4,(ni%4)+1));
    d.innerHTML='<span class="air-note">'+mn(n.midi)+'</span>'+
      '<span class="kf-row" style="gap:2px">'+dots+'</span>'+
      '<div class="air-sliders">'+
        '<div class="air-vals"><span>Min: <b id="amn'+ni+'">'+n.amn+'</b>%</span><span>Max: <b id="amx'+ni+'">'+n.amx+'</b>%</span></div>'+
        '<div class="dual-range"><div class="dual-range-track"></div><div class="dual-range-fill" id="drf'+ni+'"></div>'+
        '<input type="range" min="0" max="100" value="'+n.amn+'" oninput="CFG.notes['+ni+'].amn=parseInt(this.value);$(\'amn'+ni+'\').textContent=this.value;updDualFill('+ni+');markDirty()">'+
        '<input type="range" min="0" max="100" value="'+n.amx+'" oninput="CFG.notes['+ni+'].amx=parseInt(this.value);$(\'amx'+ni+'\').textContent=this.value;updDualFill('+ni+');markDirty()"></div>'+
      '</div>'+
      '<button class="btn btn-s" style="padding:4px 8px;font-size:.75em" onclick="testPulse(this);testCalNote('+n.midi+')">Test</button>';
    c.appendChild(d);updDualFill(ni)
  })
}

function testCalNote(midi){wsSend({t:'test_note',n:midi});wsSend({t:'test_sol',o:1});
  setTimeout(()=>wsSend({t:'test_sol',o:0}),TEST_SOL_MS)}

function startAutoCal(){autoCalRunning=true;$('btnAcalStart').style.display='none';$('btnAcalStop').style.display='';
  $('acalProgress').style.display='block';$('acalResults').style.display='none';wsSend({t:'auto_cal',mode:'air'})}
function stopAutoCal(){autoCalRunning=false;$('btnAcalStart').style.display='';$('btnAcalStop').style.display='none';
  wsSend({t:'auto_cal',mode:'stop'})}

function saveStep3(){
  if(!CFG)return;btnLoad('btnSaveStep3',true);
  const body={notes_air:CFG.notes.map(n=>({amn:n.amn,amx:n.amx}))};
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(r=>r.json()).then(d=>{btnLoad('btnSaveStep3',false);if(d.ok){showToast('Souffle sauvegarde','success');markClean();goStep(4)}else showToast('Erreur sauvegarde','error')})
    .catch(e=>{btnLoad('btnSaveStep3',false);showToast('Erreur: '+e,'error')})
}

// --- STEP 4: EXPRESSION ---
const EXPR_MODES=[
  {n:'Stable',d:'Le souffle atteint directement la valeur cible et reste constant pendant toute la note.'},
  {n:'Accent',d:'Le souffle demarre plus fort que la cible puis reduit progressivement (comme un accent naturel de flutiste).'},
  {n:'Crescendo',d:'Le souffle demarre plus faible que la cible puis augmente progressivement (entree douce).'}
];

function buildExprUI(){
  if(!CFG)return;
  const m=CFG.air_atk_mode||0;
  $('airAtkOff').value=CFG.air_atk_off||20;$('atkOffVal').textContent=(CFG.air_atk_off||20)+'%';
  $('airAtkMs').value=CFG.air_atk_ms||150;$('atkMsVal').textContent=(CFG.air_atk_ms||150)+'ms';
  $('airVelResp').value=CFG.air_vel_resp!=null?CFG.air_vel_resp:50;$('velRespVal').textContent=(CFG.air_vel_resp!=null?CFG.air_vel_resp:50)+'%';
  // Vibrato
  const vf=CFG.vib_freq||5;$('exVibF').value=vf;$('exVibFVal').textContent=vf+'Hz';
  const va=CFG.vib_amp!=null?CFG.vib_amp:3;$('exVibA').value=va;$('exVibAVal').textContent=va+'\u00b0';
  // Breath CC2
  $('exCC2On').checked=!!CFG.cc2_on;
  const thr=CFG.cc2_thr||5;$('exCC2Thr').value=thr;$('exCC2ThrVal').textContent=thr;
  const crv=CFG.cc2_curve||1;$('exCC2Curve').value=crv;$('exCC2CurveVal').textContent=crv;
  const to=CFG.cc2_timeout||2000;$('exCC2To').value=to;$('exCC2ToVal').textContent=to+'ms';
  $('exprParams').style.display=m===0?'none':'';
  setExprMode(m,true)
}

function setExprMode(m,noMark){
  CFG.air_atk_mode=m;
  document.querySelectorAll('.expr-mode').forEach(b=>{b.classList.toggle('active',parseInt(b.dataset.mode)===m)});
  $('exprModeDesc').textContent=EXPR_MODES[m].d;
  $('exprParams').style.display=m===0?'none':'';
  drawExprCurve();if(!noMark)markDirty()
}

function drawExprCurve(){
  const svg=$('exprCurveSvg');if(!svg||!CFG)return;
  const w=320,h=140,pad=30,gw=w-pad*2,gh=h-pad-10;
  const m=CFG.air_atk_mode||0,off=(CFG.air_atk_off||20)/100,dur=CFG.air_atk_ms||150,vr=(CFG.air_vel_resp!=null?CFG.air_vel_resp:50)/100;
  const maxMs=Math.max(500,dur*2.5);
  // Calculer le max Y dynamique (accent peut depasser 100%)
  const peakBase=40+(127/127)*60*vr+(1-vr)*60;
  const maxY=m===1?Math.min(150,Math.ceil((peakBase+peakBase*off)/10)*10):100;
  const tickStep=maxY<=100?25:maxY<=120?20:25;
  let s='<line x1="'+pad+'" y1="'+(h-pad)+'" x2="'+(w-pad)+'" y2="'+(h-pad)+'" stroke="#444" stroke-width="1"/>';
  s+='<line x1="'+pad+'" y1="10" x2="'+pad+'" y2="'+(h-pad)+'" stroke="#444" stroke-width="1"/>';
  s+='<text x="'+(w/2)+'" y="'+(h-4)+'" text-anchor="middle" style="font-size:9px;fill:#666">Temps (ms)</text>';
  s+='<text x="8" y="'+(h/2-10)+'" style="font-size:9px;fill:#666" transform="rotate(-90 8 '+(h/2-10)+')">Souffle %</text>';
  for(let t=0;t<=maxMs;t+=100){const x=pad+(t/maxMs)*gw;
    s+='<line x1="'+x+'" y1="'+(h-pad)+'" x2="'+x+'" y2="'+(h-pad+4)+'" stroke="#555" stroke-width=".5"/>';
    if(t%200===0)s+='<text x="'+x+'" y="'+(h-pad+14)+'" text-anchor="middle" style="font-size:8px;fill:#555">'+t+'</text>'}
  for(let p=0;p<=maxY;p+=tickStep){const y=(h-pad)-(p/maxY)*gh;
    s+='<text x="'+(pad-4)+'" y="'+(y+3)+'" text-anchor="end" style="font-size:8px;fill:#555">'+p+'</text>';
    if(p===100&&maxY>100)s+='<line x1="'+pad+'" y1="'+y+'" x2="'+(w-pad)+'" y2="'+y+'" stroke="#e94560" stroke-width=".5" stroke-dasharray="2 2" opacity=".3"/>'}
  // 2 courbes: vel=127 (forte, bleu) et vel=40 (douce, gris)
  [['#4ecdc4',127,.9],['#888',40,.5]].forEach(([col,vel,op])=>{
    const base=40+(vel/127)*60*vr+(1-vr)*60;
    let pts=[];
    for(let t=0;t<=maxMs;t+=2){
      let v=base;
      if(m===1&&t<dur)v=base+base*off*(1-t/dur);
      else if(m===2&&t<dur)v=base-base*off*(1-t/dur);
      v=Math.max(0,v);
      const x=pad+(t/maxMs)*gw,y=(h-pad)-(v/maxY)*gh;
      pts.push(x.toFixed(1)+','+y.toFixed(1))
    }
    s+='<polyline points="'+pts.join(' ')+'" fill="none" stroke="'+col+'" stroke-width="2" opacity="'+op+'"/>';
    const ty=(h-pad)-(base/maxY)*gh;
    s+='<line x1="'+pad+'" y1="'+ty+'" x2="'+(w-pad)+'" y2="'+ty+'" stroke="'+col+'" stroke-width=".5" stroke-dasharray="4 3" opacity=".4"/>'
  });
  svg.innerHTML=s
}

function saveStep4(){
  if(!CFG)return;btnLoad('btnSaveStep4',true);
  const body={air_atk_mode:CFG.air_atk_mode||0,air_atk_off:CFG.air_atk_off||20,air_atk_ms:CFG.air_atk_ms||150,air_vel_resp:CFG.air_vel_resp!=null?CFG.air_vel_resp:50,
    vib_freq:CFG.vib_freq||5,vib_amp:CFG.vib_amp!=null?CFG.vib_amp:3,
    cc2_on:!!CFG.cc2_on,cc2_thr:CFG.cc2_thr||5,cc2_curve:CFG.cc2_curve||1,cc2_timeout:CFG.cc2_timeout||2000};
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(r=>r.json()).then(d=>{btnLoad('btnSaveStep4',false);if(d.ok){showToast('Calibration terminee !','success');markClean();buildKeyboard();buildFlute(CFG,'fluteSvg',false)}else showToast('Erreur sauvegarde','error')})
    .catch(e=>{btnLoad('btnSaveStep4',false);showToast('Erreur: '+e,'error')})
}

// --- SETTINGS ---
function fillSettings(){
  if(!CFG)return;
  $('cfgDevice').value=CFG.device||'';
  const sel=$('cfgMidiCh');sel.innerHTML='<option value="0">Omni (tous)</option>';
  for(let i=1;i<=16;i++){const o=document.createElement('option');o.value=i;o.textContent='Canal '+i;sel.appendChild(o)}
  sel.value=CFG.midi_ch||0;
  $('cfgDelay').value=CFG.servo_delay;$('cfgValveInt').value=CFG.valve_interval;$('cfgMinNote').value=CFG.min_note_dur;
  $('cfgAirOff').value=CFG.air_off!=null?CFG.air_off:20;$('cfgAirMin').value=CFG.air_min;$('cfgAirMax').value=CFG.air_max;
  $('cfgCCVol').value=CFG.cc_vol!=null?CFG.cc_vol:127;$('cfgCCExpr').value=CFG.cc_expr!=null?CFG.cc_expr:127;
  $('cfgCCMod').value=CFG.cc_mod!=null?CFG.cc_mod:0;$('cfgCCBreath').value=CFG.cc_breath!=null?CFG.cc_breath:127;
  $('cfgCCBright').value=CFG.cc_bright!=null?CFG.cc_bright:64;
  // Solenoid pin dropdown (PWM-capable, non utilises par I2C/I2S/LED/boutons)
  const sp=$('cfgSolPin');sp.innerHTML='';
  [12,13,16,17,18,19,23,25,26,27,33].forEach(p=>{
    const o=document.createElement('option');o.value=p;o.textContent='GPIO '+p;sp.appendChild(o)});
  sp.value=CFG.sol_pin||13;
  $('cfgSolAct').value=CFG.sol_act;$('cfgSolHold').value=CFG.sol_hold;$('cfgSolTime').value=CFG.sol_time;
  $('cfgUnpower').value=CFG.time_unpower;
  $('cfgMidiLimit').value=CFG.midi_limit||500;
  $('cfgColor').value=CFG.color||'#D4B044';
  $('cfgKbdMode').value=CFG.kbd_mode||0;
  $('cfgHideCalib').checked=!!CFG.hide_calib;
  $('cfgShowAir').checked=!!CFG.show_air;
  // Appliquer visibilite onglets
  applyCalibVisibility();applyAirTabVisibility();
  $('wifiSsid').value=CFG.wifi_ssid||'';
  // WiFi status
  fetch('/api/wifi/status').then(r=>r.json()).then(d=>{
    $('wifiState').textContent=d.ap?'AP: '+d.ip:'STA: '+(d.ip||'deconnecte')+(d.rssi?' ('+d.rssi+' dBm)':'')
  }).catch(()=>{})
}

function saveSettings(){
  btnLoad('btnSaveSettings',true);
  const body={device:$('cfgDevice').value,midi_ch:parseInt($('cfgMidiCh').value),
    servo_delay:parseInt($('cfgDelay').value),valve_interval:parseInt($('cfgValveInt').value),
    min_note_dur:parseInt($('cfgMinNote').value),
    air_off:parseInt($('cfgAirOff').value),air_min:parseInt($('cfgAirMin').value),air_max:parseInt($('cfgAirMax').value),
    cc_vol:parseInt($('cfgCCVol').value),cc_expr:parseInt($('cfgCCExpr').value),
    cc_mod:parseInt($('cfgCCMod').value),cc_breath:parseInt($('cfgCCBreath').value),cc_bright:parseInt($('cfgCCBright').value),
    sol_pin:parseInt($('cfgSolPin').value),
    sol_act:parseInt($('cfgSolAct').value),sol_hold:parseInt($('cfgSolHold').value),sol_time:parseInt($('cfgSolTime').value),
    time_unpower:parseInt($('cfgUnpower').value),midi_limit:parseInt($('cfgMidiLimit').value),
    color:$('cfgColor').value,kbd_mode:parseInt($('cfgKbdMode').value),
    hide_calib:$('cfgHideCalib').checked,
    show_air:$('cfgShowAir').checked};
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(r=>r.json()).then(d=>{btnLoad('btnSaveSettings',false);
      if(d.ok){showToast('Parametres sauvegardes','success');markClean();loadConfig()}
      else showToast('Erreur sauvegarde','error');
      $('settingsMsg').textContent=d.ok?'Sauvegarde OK':'Erreur';$('settingsMsg').style.color=d.ok?'#4ecca3':'#e94560'})
    .catch(e=>{btnLoad('btnSaveSettings',false);showToast('Erreur: '+e,'error');$('settingsMsg').textContent='Erreur: '+e;$('settingsMsg').style.color='#e94560'})
}

function resetConfig(){if(!confirm('Remettre tous les parametres par defaut ?'))return;
  fetch('/api/config/reset',{method:'POST'}).then(r=>r.json()).then(d=>{if(d.ok){addLog('Reset OK');loadConfig();fillSettings()}})
    .catch(e=>addLog('Erreur: '+e))}

function factoryReset(){
  if(!confirm('Reset usine : tous les parametres seront remis par defaut et l\'assistant de configuration s\'ouvrira.\n\nContinuer ?'))return;
  fetch('/api/config/factory',{method:'POST'}).then(r=>r.json()).then(d=>{
    if(d.ok){
      addLog('Reset usine OK');
      toggleSettings();
      loadConfig();
    }
  }).catch(e=>addLog('Erreur: '+e))
}

// --- WIFI ---
let _scanRetries=0;
function startWifiScan(){$('scanStatus').textContent='Scan...';_scanRetries=0;
  fetch('/api/wifi/scan').then(()=>{setTimeout(checkScan,3000)})
    .catch(()=>{$('scanStatus').textContent='Erreur lancement scan'})}
function checkScan(){fetch('/api/wifi/results').then(r=>r.json()).then(d=>{
    if(!d.done){if(_scanRetries<10){_scanRetries++;setTimeout(checkScan,2000)}else{$('scanStatus').textContent='Timeout'}return}
    $('scanStatus').textContent='';const c=$('wifiList');c.innerHTML='';_scanRetries=0;
    if(d.networks&&d.networks.length){d.networks.forEach(n=>{
      const el=document.createElement('div');el.className='wifi-item';
      el.innerHTML='<span>'+esc(n.ssid)+'</span><span style="color:#888">'+esc(n.rssi)+' dBm</span>';
      el.onclick=()=>{$('wifiSsid').value=n.ssid};c.appendChild(el)})}
    else{$('scanStatus').textContent='Aucun reseau trouve'}
  }).catch(()=>{if(_scanRetries<5){_scanRetries++;$('scanStatus').textContent='Retry ('+_scanRetries+')...';setTimeout(checkScan,3000)}
    else{$('scanStatus').textContent='Erreur scan';_scanRetries=0}})}

function connectWifi(){const ssid=$('wifiSsid').value,pass=$('wifiPass').value;
  if(!ssid){$('wifiMsg').textContent='SSID requis';return}
  $('wifiMsg').textContent='Connexion...';
  fetch('/api/wifi/connect',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({ssid:ssid,pass:pass})})
    .then(r=>r.json()).then(d=>{$('wifiMsg').textContent=d.msg||'OK'})
    .catch(e=>{$('wifiMsg').textContent='Erreur: '+e})}

// --- INIT ---
window.addEventListener('load',()=>{$('velVal').textContent=WEB_DEF_VEL;$('velSlider').value=WEB_DEF_VEL;wsConnect();loadConfig();loadMidiList()});
window.addEventListener('beforeunload',e=>{if(dirty){e.preventDefault();e.returnValue=''}});
</script>
</body>
</html>
)rawliteral";

#endif
