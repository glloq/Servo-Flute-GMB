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
.kbd-air-flute-row{display:flex;align-items:flex-start;width:100%}
.kbd-air-flute-row svg{max-width:none}
#kbdAirBox{flex:0 0 auto;max-width:30%}
#kbdFluteWrap{flex:1;min-width:0}
#kbdFluteWrap svg{width:100%;height:auto}
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
.trav-only{display:none}
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
#kbdPumpPanel{display:none;background:#0d1b3e;border:1px solid #1a4080;border-radius:8px;padding:10px 14px;margin-top:6px}
#kbdPumpBtn{width:100%;padding:14px;font-size:1.1em;font-weight:bold;border:none;border-radius:8px;cursor:pointer;transition:background .2s,box-shadow .2s}
#kbdPumpBtn.off{background:linear-gradient(135deg,#1a6040,#2a9060);color:#fff;box-shadow:0 0 12px rgba(78,204,163,.3)}
#kbdPumpBtn.off:hover{background:linear-gradient(135deg,#1a7048,#30a070)}
#kbdPumpBtn.on{background:linear-gradient(135deg,#a02020,#e94560);color:#fff;box-shadow:0 0 12px rgba(233,69,96,.4)}
#kbdPumpBtn.on:hover{background:linear-gradient(135deg,#b02828,#f05070)}
.air-block{background:#0d1b3e;border:1px solid #1a4080;border-radius:8px;margin-bottom:10px;overflow:hidden;transition:border-color .2s,opacity .3s}
.air-block.disabled{opacity:.45;border-color:#333}
.air-block-hdr{display:flex;align-items:center;justify-content:space-between;padding:8px 12px;
background:rgba(255,255,255,.03);cursor:pointer;user-select:none}
.air-block-hdr:hover{background:rgba(255,255,255,.06)}
.air-block-hdr:focus{outline:2px solid #4ecca3;outline-offset:-2px;border-radius:7px}
.air-block-hdr h4{font-size:.85em;color:#e94560;margin:0;display:flex;align-items:center;gap:6px}
.air-block-hdr .air-block-toggle{position:relative;width:36px;height:20px;background:#333;border-radius:10px;
cursor:pointer;transition:background .2s;flex-shrink:0}
.air-block-hdr .air-block-toggle::after{content:'';position:absolute;width:16px;height:16px;
border-radius:50%;background:#888;top:2px;left:2px;transition:all .2s}
.air-block-hdr .air-block-toggle.on{background:#4ecca3}
.air-block-hdr .air-block-toggle.on::after{background:#fff;left:18px}
.air-block-body{padding:10px 12px;display:none;animation:airFadeIn .25s ease}
@keyframes airFadeIn{from{opacity:0;transform:translateY(-6px)}to{opacity:1;transform:translateY(0)}}
.air-layout-btn{display:flex;align-items:center;gap:4px;padding:6px 12px;background:#0d1b3e;border:1px solid #1a4080;
border-radius:8px;color:#9aa;font-size:.78em;cursor:pointer;transition:all .2s;flex:1;justify-content:center}
.air-layout-btn:hover{background:#162d5a;border-color:#2a5090}
.air-layout-btn.selected{background:#0f3460;border-color:#4ecca3;color:#4ecca3;font-weight:600}
@keyframes airShake{0%,100%{transform:translateX(0)}20%,60%{transform:translateX(-4px)}40%,80%{transform:translateX(4px)}}
.air-block.active .air-block-body{display:block}
.air-block.active{border-color:#1a5060}
@keyframes pistonDown{0%{transform:translateY(0)}100%{transform:translateY(10px)}}
@keyframes pistonUp{0%{transform:translateY(10px)}100%{transform:translateY(0)}}
@keyframes fanSpin{0%{transform:rotate(0deg)}100%{transform:rotate(360deg)}}
@keyframes bellowsExpand{0%{transform:scaleY(0.3)}100%{transform:scaleY(1)}}
@keyframes balloonGrow{0%{transform:scale(0.5)}100%{transform:scale(1)}}
@keyframes airFlowDash{to{stroke-dashoffset:-12}}
.airFlowAnim{stroke:#4ecca3;stroke-width:2;stroke-dasharray:4,8;fill:none;opacity:0;transition:opacity .3s}
.airFlowAnim.flowing{opacity:.6;animation:airFlowDash .6s linear infinite}
#airCtrlSection input[type=range]{accent-color:#4ecca3}
#airAngleShortcuts{padding-left:148px}
#airStatusInd,#airStatusText{transition:color .3s,background .3s}
#airDiagMsg{transition:opacity .3s,max-height .3s;overflow:hidden}
#airLiveStats>div{background:rgba(255,255,255,.03);border-radius:6px;padding:4px 10px;min-width:60px;text-align:center;transition:border-color .3s,box-shadow .3s;border:1px solid transparent}
#airLiveStats>div.active-stat{border-color:rgba(78,204,163,.3);box-shadow:0 0 6px rgba(78,204,163,.1)}
#airLiveStats>div span{display:block}
@media(max-width:768px){
  #airAngleShortcuts{padding-left:90px}
  .air-block .cfg-row label{flex:0 0 110px;font-size:.78em}
  #airCtrlSection .cfg-row label{flex:0 0 90px}
  #airSvgFull{max-height:220px}
}
@media(max-width:480px){#airLayoutSelect{flex-wrap:wrap}
  .air-layout-btn{font-size:.7em;padding:5px 8px}
  #airLiveStats{gap:6px !important}#airLiveStats>div{min-width:45px;padding:3px 5px;font-size:.85em}
  #airSvgFull{max-height:180px}
  .air-block .cfg-row label{flex:0 0 90px;font-size:.75em}
  .air-block .cfg-row input[type=number]{width:60px;font-size:.85em}
  .air-block .cfg-row select{font-size:.8em;max-width:140px}
  .air-block .btn-row{flex-wrap:wrap;gap:4px}
  #airStatusBar{flex-wrap:wrap;font-size:.72em;padding:4px 8px}
  #airCtrlSection .cfg-row label{flex:0 0 70px}
  #airCtrlSection input[type=range]{max-width:120px}
  #airMiniChart{height:45px}
  #airMiniChart canvas{height:45px}
  #airAngleShortcuts{padding-left:0;flex-wrap:wrap}
  #airHelpPanel{font-size:.68em;padding:6px 8px}
  .air-block h4{font-size:.85em}
  #tab-air .section>div>button{font-size:.72em;padding:4px 8px}
  #airDiagMsg{font-size:.68em}
  #airValidationMsg{font-size:.7em}
  #airConfigSummary{font-size:.68em}}
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
  <button id="btnTabAir" onclick="showTab('air',this)" style="display:none;position:relative"><svg viewBox="0 0 16 16" width="14" height="14"><path d="M8 1C4.5 1 2 4 2 7c0 2 1 3.5 2.5 4.5L4 15h8l-.5-3.5C13 10.5 14 9 14 7c0-3-2.5-6-6-6z" fill="none" stroke="currentColor" stroke-width="1.2"/><path d="M6 7.5c0-1.5 1-2.5 2-2.5s2 1 2 2.5" fill="none" stroke="currentColor" stroke-width="1"/></svg>Air<span id="airWarnBadge" style="display:none;position:absolute;top:-2px;right:-2px;width:8px;height:8px;border-radius:50%;background:#e94560"></span></button>
  <button id="btnTabCalib" onclick="showTab('calib',this)"><svg viewBox="0 0 16 16" width="14" height="14"><path d="M6.5 1L7 4H5L2 8h3l-.5 7 6-9H7.5l2-5z" fill="currentColor" opacity=".85"/></svg>Calibration</button>
</div>

<!-- TAB: KEYBOARD -->
<div class="tab active" id="tab-keyboard">
  <div class="flute-box">
    <div class="kbd-air-flute-row" id="kbdAirFluteRow">
      <div id="kbdAirBox" style="display:none"><svg id="kbdAirSvg" viewBox="0 0 520 280" style="width:100%" preserveAspectRatio="xMinYMid meet"></svg></div>
      <div id="kbdFluteWrap"><svg id="fluteSvg" viewBox="0 0 400 100" preserveAspectRatio="xMinYMid meet"></svg></div>
    </div>
    <div style="display:flex;gap:10px;font-size:.7em;color:#667;justify-content:center;padding:4px 0">
      <span><span class="kf o" style="display:inline-block;vertical-align:middle"></span> Ouvert</span>
      <span><span class="kf c" style="display:inline-block;vertical-align:middle"></span> Ferme</span>
      <span><span class="kf h" style="display:inline-block;vertical-align:middle"></span> Demi</span>
    </div>
    <div class="flute-info"><span id="fluteNote">-</span> <span id="fluteInfo" style="color:#555">Jouez une note</span></div>
    <div id="kbdAirStats" style="display:none;gap:8px;flex-wrap:wrap;font-size:.75em;padding:4px 8px;background:#0a0a1a;border-radius:0 0 8px 8px;justify-content:center">
      <span id="kbdStatPump" style="display:none;color:#9aa">Pompe <b id="kbdPumpVal">OFF</b></span>
      <span id="kbdStatFan" style="display:none;color:#9aa">Ventil. <b id="kbdFanVal">OFF</b></span>
      <span id="kbdStatValve" style="color:#9aa">Valve <b id="kbdValveVal">--</b></span>
      <span id="kbdStatServo" style="color:#9aa">Servo <b id="kbdServoVal">--</b></span>
      <span id="kbdStatRes" style="display:none;color:#9aa">Reservoir <b id="kbdResVal">--%</b></span>
    </div>
  </div>
  <div id="kbdPumpPanel">
    <div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:8px">
      <span style="font-size:.85em;font-weight:bold;color:#cde" id="kbdPumpTitle">Pompe</span>
      <span style="font-size:.7em;color:#667;font-style:italic" id="kbdPumpMode"></span>
    </div>
    <button id="kbdPumpBtn" class="off" onclick="kbdTogglePump()">Demarrer pompe</button>
    <div class="cfg-row" style="margin-top:8px">
      <label id="kbdPumpTargetLabel" style="font-size:.8em">Cible</label>
      <input type="range" min="0" max="100" value="50" id="kbdPumpTarget" oninput="kbdSetPumpTarget(this.value)">
      <span id="kbdPumpTargetVal" style="min-width:36px;text-align:right;font-size:.85em">50%</span>
    </div>
    <div id="kbdPumpHint" style="font-size:.6em;color:#667;margin-top:-2px"></div>
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
      <div class="cfg-row trav-only" id="calAngPcaRow"><label>Servo angle PCA</label>
        <select id="calAngPca" style="max-width:80px" onchange="checkPca()"></select>
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
        <span><span class="fg-dot half" style="display:inline-block;width:12px;height:12px;vertical-align:middle"></span> Demi</span>
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
      <p style="font-size:.78em;color:#888;margin:4px 0">Detection automatique de la plage servo (min/max) en balayant 0-180 degres.</p>
      <div class="btn-row" style="margin-bottom:8px">
        <button class="btn btn-s" id="btnRfStart" onclick="startRangeFinder()">Detecter plage servo</button>
        <button class="btn btn-p" id="btnRfStop" onclick="stopRangeFinder()" style="display:none">Arreter</button>
      </div>
      <div class="acal-progress" id="rfProgress" style="display:none">
        <div class="acal-info"><span id="rfStep">Sweep 0-180 deg...</span><span id="rfAngle">0 deg</span></div>
        <div class="acal-bar"><div class="acal-fill" id="rfFill"></div></div>
        <div id="rfResult" style="margin-top:8px;display:none">
          <div style="font-size:.85em;padding:8px;background:rgba(78,204,163,.08);border-radius:6px">
            <span>Min detecte: <b id="rfMinVal">-</b> deg</span> &mdash;
            <span>Max detecte: <b id="rfMaxVal">-</b> deg</span>
            <div class="btn-row" style="margin-top:8px">
              <button class="btn btn-g btn-s" onclick="applyRangeResult()">Appliquer</button>
              <button class="btn btn-s" onclick="dismissRangeResult()">Ignorer</button>
            </div>
          </div>
        </div>
      </div>
      <div class="btn-row">
        <button class="btn btn-g" id="btnAcalStart" onclick="startAutoCal()">Auto-calibrer toutes les notes</button>
        <button class="btn btn-p" id="btnAcalStop" onclick="stopAutoCal()" style="display:none">Arreter</button>
      </div>
      <div class="acal-progress" id="acalProgress">
        <div class="acal-info"><span id="acalStep">-</span><span id="acalState">-</span><span id="acalAngle"></span></div>
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

<!-- TAB: AIR MANAGEMENT (adaptatif) -->
<div class="tab" id="tab-air">
  <div id="airStatusBar" style="background:#0d1b3e;border:1px solid #1a4080;border-radius:6px;padding:6px 12px;margin-bottom:8px;display:flex;align-items:center;gap:12px;font-size:.8em">
    <span style="color:#e94560;font-weight:bold" id="airStatusMode">--</span>
    <span id="airStatusInd" style="width:8px;height:8px;border-radius:50%;background:#555"></span>
    <span id="airStatusText" style="color:#9aa">En attente</span>
    <span style="margin-left:auto;font-size:.75em;color:#666" id="airLastUpdate" title="Derniere mise a jour recue">--</span>
    <span style="font-size:.7em;color:#555" id="airHeapInfo" title="Memoire libre ESP32"></span>
  </div>
  <div class="section">
    <h3>Systeme d'air</h3>
    <div class="flute-box">
      <svg id="airSvgFull" viewBox="0 0 520 280" style="width:100%;max-height:280px"></svg>
    </div>
    <div id="airLiveStats" style="display:flex;gap:16px;flex-wrap:wrap;margin-top:8px">
      <div id="airStatPump" style="display:none" title="PWM actuel de la pompe (0-255). OFF = pompe arretee"><span style="font-size:.7em;color:#9aa">Pompe</span><div style="font-weight:bold" id="airPumpPwm">OFF</div></div>
      <div id="airStatActivePumps" style="display:none" title="Nombre de pompes actives (mode cascade)"><span style="font-size:.7em;color:#9aa">Actives</span><div style="font-weight:bold" id="airActivePumps">0</div></div>
      <div id="airStatFan" style="display:none" title="Vitesse ventilateur. Orange = rampe en cours"><span style="font-size:.7em;color:#9aa">Ventilateur</span><div style="font-weight:bold" id="airFanPwm">OFF</div></div>
      <div id="airStatRes" style="display:none" title="Remplissage reservoir (0-100%). Vert>80%, Orange>30%, Rouge<30%"><span style="font-size:.7em;color:#9aa">Reservoir</span><div style="font-weight:bold" id="airResPct">--%</div>
        <div style="width:40px;height:4px;background:#333;border-radius:2px;margin:2px auto 0"><div id="airResFillBar" style="height:100%;background:#e94560;border-radius:2px;width:0%;transition:width .3s"></div></div></div>
      <div id="airStatDist" style="display:none" title="Distance mesuree par le capteur ToF (mm)"><span style="font-size:.7em;color:#9aa">Distance</span><div style="font-weight:bold" id="airResMm">--mm</div></div>
      <div id="airStatHall" style="display:none" title="Valeur brute du capteur Hall (0-4095)"><span style="font-size:.7em;color:#9aa">Hall</span><div style="font-weight:bold" id="airHallVal">--</div></div>
      <div id="airStatEndstop" style="display:none" title="Etat du fin de course (actif/inactif)"><span style="font-size:.7em;color:#9aa">Fin course</span><div style="font-weight:bold" id="airEndstopState">--</div></div>
      <div id="airStatSensor" style="display:none" title="Detecte si le capteur est connecte et repond"><span style="font-size:.7em;color:#9aa">Capteur</span><div style="font-weight:bold" id="airSensorOk">--</div></div>
      <div id="airStatPidErr" style="display:none" title="Erreur PID: difference entre cible et mesure"><span style="font-size:.7em;color:#9aa">Erreur PID</span><div style="font-weight:bold" id="airPidErrVal">--</div></div>
      <div id="airStatValve" title="Etat de la valve: ouvert = air circule"><span style="font-size:.7em;color:#9aa">Valve</span><div style="font-weight:bold" id="airValveState">--</div></div>
      <div title="Angle actuel du servo flow. Gris=repos, Vert=actif, Rouge=max"><span style="font-size:.7em;color:#9aa">Servo air</span><div style="font-weight:bold" id="airServoAngle">--&deg;</div></div>
    </div>
    <div id="airMiniChart" style="display:none;margin-top:8px">
      <canvas id="airChartCanvas" width="400" height="60" style="width:100%;height:60px;background:#111;border-radius:4px"></canvas>
      <div style="display:flex;justify-content:space-between;font-size:.6em;color:#666;margin-top:2px"><span>-30s</span><span>maintenant</span></div>
    </div>
  </div>
  <div class="section" id="airCtrlSection" style="display:none">
    <h3 id="airCtrlTitle">Controle</h3>
    <div id="airCtrlPumpRow" class="cfg-row"><label id="airTargetLabel">Cible</label>
      <input type="range" min="0" max="100" value="0" id="pumpTarget" oninput="setAirTarget(this.value)">
      <span id="pumpTargetVal" style="min-width:36px;text-align:right">0%</span>
    </div>
    <div id="airTargetHint" style="font-size:.6em;color:#667;margin:-4px 0 4px;display:none"></div>
    <div class="cfg-row"><label>Ouverture flow</label>
      <input type="range" min="0" max="180" value="10" id="airFlowTest" oninput="testServoFlow(this.value)">
      <span id="airFlowTestVal" style="min-width:36px;text-align:right">0%</span>
      <button class="btn btn-s" onclick="sweepServoFlow()" title="Balaye de min a max et retour" style="padding:4px 8px;font-size:.7em">Sweep</button>
    </div>
    <div id="airAngleShortcuts" style="display:flex;gap:4px;margin:-4px 0 4px">
      <button class="btn btn-s" id="btnGotoOff" onclick="gotoServoAngle('cfgAirOff')" style="padding:2px 6px;font-size:.65em;display:none" title="Aller a la position note Off">Off</button>
      <button class="btn btn-s" onclick="gotoServoAngle('cfgAirMin')" style="padding:2px 6px;font-size:.65em" title="Aller a l'angle minimum">Min</button>
      <button class="btn btn-s" onclick="gotoServoAngle('cfgAirMax')" style="padding:2px 6px;font-size:.65em" title="Aller a l'angle maximum">Max</button>
    </div>
    <div class="btn-row">
      <button class="btn btn-p" id="btnAirStop" onclick="stopAirSource()">Arreter</button>
      <button class="btn btn-s" id="btnValveOpen" onclick="wsSend({t:'test_sol',o:1})">Ouvrir valve</button>
      <button class="btn btn-s" id="btnValveClose" onclick="wsSend({t:'test_sol',o:0})">Fermer valve</button>
      <button class="btn btn-s" id="btnAirTest" onclick="testAirSystem()" style="display:none">Test rapide</button>
      <select id="airTestDur" style="display:none;font-size:.7em;padding:2px;background:#1a1a2e;color:#eee;border:1px solid #333;border-radius:3px" title="Duree du test rapide">
        <option value="1000">1s</option><option value="2000" selected>2s</option><option value="5000">5s</option><option value="10000">10s</option></select>
      <button class="btn btn-s" id="btnAirDiag" onclick="runAirDiagnostic()" title="Teste tous les composants en sequence">Diagnostic</button>
    </div>
    <div id="airDiagMsg" style="display:none;font-size:.75em;color:#9aa;margin-top:4px;padding:4px 8px;background:rgba(255,255,255,.03);border-radius:4px"></div>
    <div id="airDiagBar" style="display:none;height:3px;background:#1a1a2e;border-radius:2px;margin-top:2px;overflow:hidden"><div id="airDiagFill" style="height:100%;width:0;background:#4ecca3;transition:width .3s ease;border-radius:2px"></div></div>
  </div>
  <div class="section">
    <div style="display:flex;justify-content:space-between;align-items:center">
      <h3>Configuration</h3>
      <div style="display:flex;gap:4px">
        <button class="btn btn-s" onclick="toggleAirHelp()" style="font-size:.65em;padding:2px 8px" title="Aide rapide">?</button>
        <button class="btn btn-s" onclick="toggleAllAirBlocks()" style="font-size:.65em;padding:2px 8px">Tout plier/deplier</button>
      </div>
    </div>
    <div id="airHelpPanel" style="display:none;font-size:.75em;color:#aaa;background:rgba(78,204,163,.05);border:1px solid rgba(78,204,163,.15);border-radius:6px;padding:8px 12px;margin:6px 0;line-height:1.5">
      <b style="color:#4ecca3">Guide rapide</b><br>
      1. Choisir le mode correspondant au materiel<br>
      2. Configurer les parametres dans chaque bloc<br>
      3. Utiliser les boutons de test pour verifier<br>
      4. Lancer le diagnostic pour valider le tout<br>
      5. Sauvegarder la configuration<br>
      <span style="color:#888">Astuce: les badges sous le mode montrent les composants necessaires</span><br>
      <span style="color:#888">Raccourcis clavier: <b>Ctrl+S</b>=Sauver, <b>T</b>=Test, <b>Esc</b>=Stop, <b>H</b>=Aide, <b>?</b>=Raccourcis</span>
    </div>
    <div class="cfg-row" style="gap:10px">
      <label>Source air</label>
      <div id="airLayoutSelect" style="display:flex;gap:6px;flex:1">
        <label class="air-layout-btn selected" id="airLayoutPump" onclick="setAirLayout(0)">
          <input type="radio" name="airLayout" value="0" checked style="display:none">
          <svg viewBox="0 0 20 20" width="14" height="14"><circle cx="10" cy="10" r="6" fill="none" stroke="currentColor" stroke-width="1.5"/><line x1="10" y1="4" x2="10" y2="16" stroke="currentColor" stroke-width="1.5"/></svg>
          Pompe / Reservoir
        </label>
        <label class="air-layout-btn" id="airLayoutFan" onclick="setAirLayout(1)">
          <input type="radio" name="airLayout" value="1" style="display:none">
          <svg viewBox="0 0 20 20" width="14" height="14"><circle cx="10" cy="10" r="7" fill="none" stroke="currentColor" stroke-width="1.2"/><path d="M10 3C10 7 13 10 10 10S4 7 4 10 7 17 10 17" fill="none" stroke="currentColor" stroke-width="1"/></svg>
          Ventilateur
        </label>
      </div>
    </div>
    <input type="hidden" id="airModeSelect" value="0">
    <div id="airModeDesc" style="font-size:.78em;color:#aaa;margin:4px 0 12px;padding:6px 10px;background:rgba(255,255,255,.04);border-radius:6px"></div>

    <!-- BLOCK: Pompe -->
    <div class="air-block" id="airBlockPump" style="display:none">
      <div class="air-block-hdr" tabindex="0" role="button" aria-label="Configuration pompe" onclick="toggleAirBlock('airBlockPump')" onkeydown="toggleAirBlock('airBlockPump',event)">
        <h4><svg viewBox="0 0 16 16" width="14" height="14"><circle cx="8" cy="8" r="5" fill="none" stroke="currentColor" stroke-width="1.5"/><line x1="8" y1="3" x2="8" y2="13" stroke="currentColor" stroke-width="1.5"/></svg>Pompe</h4>
        <div class="air-block-toggle on" id="airBlockPumpToggle" role="switch" aria-checked="true" onclick="event.stopPropagation();toggleAirBlockEnable('airBlockPump')"></div>
      </div>
      <div class="air-block-body">
        <div class="cfg-row"><label>Type moteur</label>
          <select id="airMotorType" onchange="toggleMotorType()">
            <option value="0">PWM variable</option>
            <option value="1">On/Off simple</option>
          </select>
          <div id="airMotorHelp" style="font-size:.65em;color:#888;margin-top:2px"></div>
        </div>
        <div class="cfg-row"><label>Nombre pompes</label>
          <select id="airNumPumps" onchange="buildPumpRows();toggleCascadeRow()">
            <option value="1">1 pompe</option>
            <option value="2">2 pompes</option>
            <option value="3">3 pompes</option>
          </select>
        </div>
        <div id="airCascadeRow" style="display:none;border-left:2px solid #e94560;padding-left:8px;margin:8px 0">
          <div style="font-size:.7em;color:#e94560;font-weight:bold;margin-bottom:4px">Gestion multi-pompes (cascade)</div>
          <div class="cfg-row"><label>Seuil cascade (%)</label>
            <input type="number" id="airCascadeThreshold" min="0" max="100" value="80" title="Seuil de demande (%) pour activer la pompe suivante. 0 = toutes en parallele">
            <span style="font-size:.6em;color:#888;margin-left:4px">0=parallele</span>
          </div>
          <div class="cfg-row"><label>Delai stagger (ms)</label>
            <input type="number" id="airStaggerMs" min="0" max="1000" value="150" title="Delai entre chaque demarrage de pompe (anti-pic courant)">
          </div>
        </div>
        <div id="airBangbangRow" style="display:none;border-left:2px solid #4ecca3;padding-left:8px;margin:8px 0">
          <div style="font-size:.7em;color:#4ecca3;font-weight:bold;margin-bottom:4px">Bang-bang (moteur On/Off + capteur continu)</div>
          <div class="cfg-row"><label>Hysteresis (%)</label>
            <input type="number" id="airBbHysteresis" min="1" max="50" value="5" title="Bande d'hysteresis autour de la cible. Pompe ON si fill < cible-hyst, OFF si fill > cible+hyst">
          </div>
        </div>
        <div id="airPumpRows"></div>
      </div>
    </div>

    <!-- BLOCK: Ventilateur -->
    <div class="air-block" id="airBlockFan" style="display:none">
      <div class="air-block-hdr" tabindex="0" role="button" aria-label="Configuration ventilateur" onclick="toggleAirBlock('airBlockFan')" onkeydown="toggleAirBlock('airBlockFan',event)">
        <h4><svg viewBox="0 0 16 16" width="14" height="14"><circle cx="8" cy="8" r="6" fill="none" stroke="currentColor" stroke-width="1.2"/><path d="M8 2C8 5 10 8 8 8S4 5 4 8 6 14 8 14" fill="none" stroke="currentColor" stroke-width="1"/></svg>Ventilateur</h4>
        <div class="air-block-toggle on" id="airBlockFanToggle" role="switch" aria-checked="true" onclick="event.stopPropagation();toggleAirBlockEnable('airBlockFan')"></div>
      </div>
      <div class="air-block-body">
        <div class="cfg-row"><label>GPIO ventilateur</label>
          <select id="airFanPin"></select>
        </div>
        <div class="cfg-row"><label>PWM min</label>
          <input type="number" id="airFanMin" min="0" max="255" value="60">
        </div>
        <div class="cfg-row"><label>PWM max</label>
          <input type="number" id="airFanMax" min="0" max="255" value="255">
        </div>
        <div style="font-size:.7em;color:#4ecca3;padding:6px 0 2px;font-weight:600">Mode idle (entre notes)</div>
        <div class="cfg-row"><label>Vitesse idle (%)</label>
          <input type="number" id="airFanIdlePct" min="0" max="100" value="20" title="Vitesse du ventilateur entre les notes (0=couper immediatement). Garder une rotation basse permet un redemarrage plus rapide.">
        </div>
        <div class="cfg-row"><label>Timeout idle (ms)</label>
          <input type="number" id="airFanIdleTimeout" min="0" max="30000" value="5000" title="Couper le ventilateur apres ce delai sans note On (0=ne jamais couper, rester en idle)">
        </div>
        <div style="font-size:.65em;color:#888;padding:2px 0">Idle: reduit la vitesse a X% entre les notes. Apres le timeout sans note, coupe completement.</div>
      </div>
    </div>

    <!-- BLOCK: Reservoir -->
    <div class="air-block" id="airBlockRes" style="display:none">
      <div class="air-block-hdr" tabindex="0" role="button" aria-label="Configuration reservoir" onclick="toggleAirBlock('airBlockRes')" onkeydown="toggleAirBlock('airBlockRes',event)">
        <h4><svg viewBox="0 0 16 16" width="14" height="14"><rect x="3" y="4" width="10" height="10" rx="2" fill="none" stroke="currentColor" stroke-width="1.2"/><path d="M5 8h6" stroke="currentColor" stroke-width="1" stroke-dasharray="2,1"/></svg>Reservoir</h4>
        <div class="air-block-toggle on" id="airBlockResToggle" role="switch" aria-checked="true" onclick="event.stopPropagation();toggleAirBlockEnable('airBlockRes')"></div>
      </div>
      <div class="air-block-body">
        <div class="cfg-row"><label>Format</label>
          <select id="airResFormat" onchange="buildAirSvg('airSvgFull',true)">
            <option value="balloon">Ballon</option>
            <option value="bellows">Soufflet</option>
          </select>
        </div>
        <div class="cfg-row"><label>Capteur reservoir</label>
          <select id="airSensorType" onchange="toggleSensorParams()">
            <optgroup label="Distance (I2C)">
              <option value="0">VL53L0X (50-1200mm)</option>
              <option value="1">VL6180X (10-300mm)</option>
            </optgroup>
            <optgroup label="Analogique">
              <option value="2">Hall KY-024</option>
            </optgroup>
            <optgroup label="Contact">
              <option value="3">Endstop mecanique</option>
              <option value="4">Endstop optique</option>
            </optgroup>
          </select>
        </div>
        <div id="airSensLive" style="display:none;background:rgba(78,204,163,.08);border:1px solid rgba(78,204,163,.2);border-radius:6px;padding:6px 10px;margin:6px 0">
          <span style="font-size:.75em;color:#4ecca3" id="airSensLiveLabel">Lecture live</span>
          <span style="font-weight:bold;margin-left:8px" id="airSensLiveVal">--</span>
          <span style="font-size:.7em;margin-left:4px;color:#e44;display:none" id="airSensLiveWarn">capteur absent</span>
        </div>
        <div id="airSensParamsTof">
          <div class="cfg-row"><label>Cible (mm)</label>
            <input type="number" id="airSensTarget" min="1" max="300" value="50" title="Distance cible du capteur ToF. La pompe maintient cette distance. Typique: 30-80mm">
          </div>
          <div class="cfg-row"><label>Min (mm)</label>
            <input type="number" id="airSensMin" min="1" max="300" value="10" title="Distance = reservoir plein. Pompe s'arrete a cette distance.">
          </div>
          <div class="cfg-row"><label>Max (mm)</label>
            <input type="number" id="airSensMax" min="1" max="300" value="150" title="Distance = reservoir vide. Pompe demarre a cette distance.">
          </div>
        </div>
        <div id="airSensParamsHall" style="display:none">
          <div class="cfg-row"><label>GPIO analogique</label>
            <select id="airHallPin"><option value="34">GPIO 34</option><option value="35">GPIO 35</option><option value="36">GPIO 36</option><option value="39">GPIO 39</option></select>
          </div>
          <div class="cfg-row"><label>Seuil bas</label>
            <input type="number" id="airHallLow" min="0" max="4095" value="1500" title="Lecture Hall = vide. Ajuster quand le reservoir est vide." oninput="updateHallBar()">
          </div>
          <div class="cfg-row"><label>Seuil haut</label>
            <input type="number" id="airHallHigh" min="0" max="4095" value="2500" title="Lecture Hall = plein. Ajuster quand le reservoir est plein." oninput="updateHallBar()">
          </div>
          <div id="airHallBar" style="margin:4px 0;height:12px;background:#222;border-radius:6px;position:relative;overflow:hidden">
            <div id="airHallThreshLow" style="position:absolute;top:0;height:100%;width:1px;background:#e94560"></div>
            <div id="airHallThreshHigh" style="position:absolute;top:0;height:100%;width:1px;background:#e94560"></div>
            <div id="airHallCursor" style="position:absolute;top:0;height:100%;width:3px;background:#4ecca3;border-radius:2px"></div>
          </div>
        </div>
        <div id="airSensParamsEndstop" style="display:none">
          <div class="cfg-row"><label>GPIO fin course</label>
            <select id="airEndstopPin"><option value="34">GPIO 34</option><option value="35">GPIO 35</option><option value="36">GPIO 36</option><option value="39">GPIO 39</option></select>
          </div>
          <div class="cfg-row"><label>Logique active</label>
            <select id="airEndstopHigh"><option value="0">Actif LOW (NC recommande)</option><option value="1">Actif HIGH</option></select>
          </div>
          <div class="cfg-row"><label>Pompe si capteur</label>
            <select id="airEndstopPumpOn" title="Quand activer/arreter la pompe par rapport au capteur">
              <option value="0">Pompe ON quand capteur inactif (remplir)</option>
              <option value="1">Pompe ON quand capteur actif (vider)</option>
            </select>
          </div>
          <div style="font-size:.65em;color:#888;padding:2px 0">NC (normalement ferme) recommande pour les endstops mecaniques/optiques : detecte aussi le fil coupe.</div>
        </div>
        <div id="airSensParamsPid" style="display:none">
          <div class="cfg-row"><label>PID Kp (x10)</label>
            <input type="number" id="airPidKp" min="1" max="100" value="30" title="Proportionnel: reactivite aux ecarts. Plus haut = correction rapide mais risque oscillation">
          </div>
          <div class="cfg-row"><label>PID Ki (x10)</label>
            <input type="number" id="airPidKi" min="0" max="50" value="5" title="Integral: corrige les erreurs persistantes. Plus haut = plus precis mais risque depassement">
          </div>
          <div style="font-size:.7em;color:#888;padding:2px 0">Kp: reactivite, Ki: precision long terme. Valeurs /10 (ex: 30 = 3.0)</div>
          <div class="btn-row" style="margin-top:4px">
            <button class="btn btn-s" onclick="$('airPidKp').value=45;$('airPidKi').value=8;markDirty();validateAirConfig()" style="font-size:.7em;padding:3px 8px" title="Reponse rapide, risque leger depassement">Rapide</button>
            <button class="btn btn-s" onclick="$('airPidKp').value=30;$('airPidKi').value=5;markDirty();validateAirConfig()" style="font-size:.7em;padding:3px 8px" title="Bon compromis reactivite/stabilite">Equilibre</button>
            <button class="btn btn-s" onclick="$('airPidKp').value=15;$('airPidKi').value=2;markDirty();validateAirConfig()" style="font-size:.7em;padding:3px 8px" title="Montee lente et stable, sans depassement">Doux</button>
          </div>
        </div>
      </div>
    </div>

    <!-- BLOCK: Valve -->
    <div class="air-block" id="airBlockValve">
      <div class="air-block-hdr" tabindex="0" role="button" aria-label="Configuration valve" onclick="toggleAirBlock('airBlockValve')" onkeydown="toggleAirBlock('airBlockValve',event)">
        <h4><svg viewBox="0 0 16 16" width="14" height="14"><rect x="4" y="2" width="8" height="12" rx="1" fill="none" stroke="currentColor" stroke-width="1.2"/><rect x="6" y="4" width="4" height="4" rx="0.5" fill="currentColor" opacity=".5"/></svg>Valve</h4>
        <div class="air-block-toggle on" id="airBlockValveToggle" role="switch" aria-checked="true" onclick="event.stopPropagation();toggleAirBlockEnable('airBlockValve')"></div>
      </div>
      <div class="air-block-body">
        <div class="cfg-row"><label>Type valve</label>
          <select id="airValveType" onchange="toggleValveParams()">
            <option value="0">Solenoide</option>
            <option value="1">Servo</option>
          </select>
        </div>
        <div id="airValveSolParams">
          <div class="cfg-row"><label>GPIO Pin</label><select id="cfgSolPin" title="Pin GPIO connectee au MOSFET/relais du solenoide"></select></div>
          <div class="cfg-row"><label>PWM activation</label><input type="number" id="cfgSolAct" min="0" max="255" title="PWM pour ouvrir la valve (180-255 typique)" oninput="updPwmPct(this)"><span class="pwm-pct" style="font-size:.65em;color:#888;min-width:30px;text-align:right"></span></div>
          <div class="cfg-row"><label>PWM maintien</label><input type="number" id="cfgSolHold" min="0" max="255" title="PWM pour maintenir ouvert (60-120 typique, economise courant)" oninput="updPwmPct(this)"><span class="pwm-pct" style="font-size:.65em;color:#888;min-width:30px;text-align:right"></span></div>
          <div class="cfg-row"><label>Temps actif avant maintien (ms)</label><input type="number" id="cfgSolTime" min="0" max="500" title="Duree en ms de la phase activation avant passage au maintien (20-50 typique)"></div>
          <div class="cfg-row"><label>Temps inter-note valve ouverte (ms)</label><input type="number" id="cfgSolInter" min="0" max="2000" value="0" title="Si l'ecart entre 2 notes est inferieur a cette valeur, la valve reste ouverte (0=toujours fermer)"></div>
          <div style="font-size:.65em;color:#888;padding:2px 0">Activation: impulsion forte pour ouvrir. Maintien: courant reduit pour garder ouvert.</div>
          <button class="btn btn-s" onclick="wsSend({t:'test_sol',o:1});setTimeout(()=>wsSend({t:'test_sol',o:0}),500)" style="font-size:.65em;padding:2px 8px;margin-top:4px" title="Ouvre le solenoide pendant 0.5s">Test solenoide</button>
        </div>
        <div id="airValveServoParams" style="display:none">
          <div class="cfg-row"><label>Canal PCA valve</label>
            <input type="number" id="airValveCh" min="0" max="15" value="11" title="Canal sur le PCA9685 (0-15). Ne pas utiliser le meme canal que le servo flow.">
          </div>
          <div class="cfg-row"><label>Angle ferme</label><input type="number" id="cfgVlvClose" min="0" max="180" value="0" title="Angle servo quand la valve est fermee"></div>
          <div class="cfg-row"><label>Angle ouvert</label><input type="number" id="cfgVlvOpen" min="0" max="180" value="90" title="Angle servo quand la valve est ouverte"></div>
          <div class="cfg-row"><label>Sens ouverture</label>
            <select id="cfgVlvDir">
              <option value="0">Horaire</option>
              <option value="1">Anti-horaire</option>
            </select>
          </div>
        </div>
      </div>
    </div>

    <!-- BLOCK: Servo Flow -->
    <div class="air-block active" id="airBlockServo">
      <div class="air-block-hdr" tabindex="0" role="button" aria-label="Configuration servo flow" onclick="toggleAirBlock('airBlockServo')" onkeydown="toggleAirBlock('airBlockServo',event)">
        <h4><svg viewBox="0 0 16 16" width="14" height="14"><rect x="2" y="5" width="12" height="6" rx="2" fill="none" stroke="currentColor" stroke-width="1.2"/><line x1="8" y1="5" x2="12" y2="2" stroke="currentColor" stroke-width="1.2" stroke-linecap="round"/></svg>Servo Flow</h4>
        <div class="air-block-toggle on" id="airBlockServoToggle" role="switch" aria-checked="true" onclick="event.stopPropagation();toggleAirBlockEnable('airBlockServo')"></div>
      </div>
      <div class="air-block-body">
        <div class="cfg-row" id="cfgAirOffRow" style="display:none"><label>Position note Off</label><input type="number" id="cfgAirOff" min="0" max="180" title="Angle quand aucune note ne joue (coupe l'air). Typique: 0-20°" oninput="updateFlowSliderRange()"></div>
        <div class="cfg-row"><label id="cfgAirMinLabel">Angle min</label><input type="number" id="cfgAirMin" min="0" max="180" title="Angle minimal pour les notes les plus douces (pp). Typique: 5-15°" oninput="updateFlowSliderRange()"></div>
        <div class="cfg-row"><label id="cfgAirMaxLabel">Angle max</label><input type="number" id="cfgAirMax" min="0" max="180" title="Angle maximal pour les notes les plus fortes (ff). Typique: 60-120°" oninput="updateFlowSliderRange()"></div>
        <div id="airServoOffHint" style="font-size:.65em;color:#888;padding:2px 0"></div>
        <div class="btn-row" style="margin-top:4px">
          <button class="btn btn-s" onclick="applyServoPreset(5,60)" style="font-size:.7em;padding:3px 8px" title="Souffle leger, ideal pour flute a bec">Doux</button>
          <button class="btn btn-s" onclick="applyServoPreset(10,90)" style="font-size:.7em;padding:3px 8px" title="Bon compromis pour la plupart des flutes">Standard</button>
          <button class="btn btn-s" onclick="applyServoPreset(15,120)" style="font-size:.7em;padding:3px 8px" title="Souffle fort, flute traversiere ou gros volume">Puissant</button>
        </div>
      </div>
    </div>

    <!-- BLOCK: Angle Servo (trav only) -->
    <div class="air-block trav-only" id="airBlockAngle">
      <div class="air-block-hdr" tabindex="0" role="button" aria-label="Configuration servo angle" onclick="toggleAirBlock('airBlockAngle')" onkeydown="toggleAirBlock('airBlockAngle',event)">
        <h4><svg viewBox="0 0 16 16" width="14" height="14"><path d="M8 2L2 14h12L8 2z" fill="none" stroke="currentColor" stroke-width="1.2" stroke-linejoin="round"/><line x1="8" y1="6" x2="8" y2="10" stroke="currentColor" stroke-width="1.2"/></svg>Servo Angle (traversiere)</h4>
        <span class="air-block-toggle" id="airBlockAngleToggle" role="switch" aria-checked="false" onclick="event.stopPropagation();toggleAngleServo()" title="Activer/desactiver le servo angle"></span>
      </div>
      <div class="air-block-body">
        <p style="font-size:.72em;color:#888;margin:0 0 8px">Angle du jet d'air par rapport au biseau. Visible uniquement pour les flutes traversieres. CC74 (Brightness) module l'angle en temps reel.</p>
        <div class="cfg-row"><label>Canal PCA angle</label><select id="cfgAngPca" style="max-width:80px" onchange="syncAngPca(this.value);checkPca()"></select></div>
        <div class="cfg-row"><label>Angle repos</label><input type="number" id="cfgAngOff" min="0" max="180" title="Position au repos (centre)"></div>
        <div class="cfg-row"><label>Angle min</label><input type="number" id="cfgAngMin" min="0" max="180" title="Angle minimum calibre"></div>
        <div class="cfg-row"><label>Angle max</label><input type="number" id="cfgAngMax" min="0" max="180" title="Angle maximum calibre"></div>
        <div class="cfg-row"><label>Test angle</label><input type="range" min="0" max="180" value="90" id="testAngSlider" oninput="$('testAngVal').textContent=this.value+'&deg;';wsSend({t:'test_angle',a:parseInt(this.value)})"><span id="testAngVal" style="min-width:30px;font-size:.8em">90&deg;</span></div>
      </div>
    </div>

    <!-- Options affichage -->
    <div style="margin-top:12px">
      <div class="cfg-row"><label>Afficher schemas air dans onglet clavier</label><input type="checkbox" id="cfgShowAir" style="width:auto;flex:0" title="Affiche le schema pneumatique en miniature sur l'onglet clavier"></div>
    </div>
    <div id="airValidationMsg" style="display:none;font-size:.78em;color:#e94560;background:rgba(233,69,96,.08);border:1px solid rgba(233,69,96,.25);border-radius:6px;padding:8px 10px;margin:8px 0"></div>
    <div class="btn-row" style="margin-top:12px;gap:8px">
      <button class="btn btn-g" id="btnAirSave" onclick="saveAirSettings()">Sauvegarder</button>
      <button class="btn btn-s" onclick="resetAirDefaults()" style="font-size:.8em" title="Reinitialiser les valeurs par defaut pour le mode actuel">Defauts</button>
      <button class="btn btn-s" onclick="copyAirConfig()" style="font-size:.8em" title="Copier la configuration air au presse-papier (JSON)">Copier</button>
      <button class="btn btn-s" onclick="importAirConfig()" style="font-size:.8em" title="Importer une configuration depuis le presse-papier (JSON)">Importer</button>
    </div>
    <div id="airSettingsMsg" style="font-size:.75em;color:#0f0;margin-top:6px"></div>
    <div id="airConfigSummary" style="display:none;font-size:.7em;color:#888;margin-top:6px;padding:4px 8px;background:rgba(255,255,255,.02);border-radius:4px"></div>
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
    <div style="display:flex;flex-direction:column;gap:8px;max-height:55vh;overflow-y:auto">
      <label class="wiz-card" onclick="wizSetAir(0)"><input type="radio" name="wizAir" value="0" checked>
        <span><b>Solenoide + servo flow</b><br><span style="font-size:.75em;color:#9aa">Valve solenoide coupe l'air, servo flow regle le debit. Classique.</span></span></label>
      <label class="wiz-card" onclick="wizSetAir(1)"><input type="radio" name="wizAir" value="1">
        <span><b>Servo-valve (PCA)</b><br><span style="font-size:.75em;color:#9aa">Un servo PCA9685 remplace le solenoide pour couper l'air.</span></span></label>
      <label class="wiz-card" onclick="wizSetAir(2)"><input type="radio" name="wizAir" value="2">
        <span><b>Servo flow seul</b><br><span style="font-size:.75em;color:#9aa">Juste un servo flow, pas de valve. Le servo passe a l'angle off entre les notes.</span></span></label>
      <label class="wiz-card" onclick="wizSetAir(3)"><input type="radio" name="wizAir" value="3">
        <span><b>Ventilateur + servo flow</b><br><span style="font-size:.75em;color:#9aa">Ventilateur PWM souffle en continu, servo flow dirige vers le bec.</span></span></label>
      <label class="wiz-card" onclick="wizSetAir(4)"><input type="radio" name="wizAir" value="4">
        <span><b>Pompe(s) + valve</b><br><span style="font-size:.75em;color:#9aa">1 a 3 pompes PWM + valve ON/OFF. Souffle direct dans la flute.</span></span></label>
      <label class="wiz-card" onclick="wizSetAir(5)"><input type="radio" name="wizAir" value="5">
        <span><b>Pompe(s) + reservoir + valve</b><br><span style="font-size:.75em;color:#9aa">Pompe remplit un reservoir, capteur (ToF/Hall/endstop) regule le niveau.</span></span></label>
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

  <div class="section"><h3>MIDI Serial (DIN)</h3>
    <div class="cfg-row"><label>Activer</label><input type="checkbox" id="cfgSmidiOn" style="width:auto;flex:0"></div>
    <div class="cfg-row"><label>Pin RX (GPIO)</label><select id="cfgSmidiRx" style="width:auto"><option value="16">16 (RX2)</option><option value="17">17</option><option value="18">18</option><option value="19">19</option><option value="23">23</option><option value="25">25</option><option value="26">26</option><option value="27">27</option><option value="33">33</option><option value="34">34 (input only)</option><option value="35">35 (input only)</option><option value="36">36 (input only)</option><option value="39">39 (input only)</option></select></div>
    <div style="font-size:.7em;color:#666;margin-top:2px">Connecter un circuit optocoupler MIDI DIN au GPIO choisi. Les pins 34-39 sont en entree uniquement (ideal pour RX). Redemarrage requis.</div>
  </div>

  <div class="section"><h3>Timing des notes</h3>
    <div class="cfg-row"><label>Delai doigts &rarr; souffle (ms)</label><input type="number" id="cfgDelay" min="0" max="1000"></div>
    <div style="font-size:.7em;color:#666;margin:-4px 0 6px 148px">Temps entre le positionnement des doigts et l'ouverture de la valve</div>
    <div class="cfg-row"><label>Fermeture valve inter-notes (ms)</label><input type="number" id="cfgValveInt" min="0" max="500"></div>
    <div style="font-size:.7em;color:#666;margin:-4px 0 6px 148px">Intervalle minimum entre 2 notes pour fermer la valve (0 = legato)</div>
    <div class="cfg-row"><label>Duree minimum d'une note (ms)</label><input type="number" id="cfgMinNote" min="0" max="500"></div>
  </div>

  <div class="section"><h3>Valeurs MIDI par defaut</h3>
    <div style="font-size:.7em;color:#666;margin-bottom:6px">Valeurs initiales au demarrage (0-127). Modifiables en temps reel via MIDI CC.</div>
    <div class="cfg-row"><label>Volume (CC7)</label><input type="number" id="cfgCCVol" min="0" max="127"></div>
    <div class="cfg-row"><label>Expression (CC11)</label><input type="number" id="cfgCCExpr" min="0" max="127"></div>
    <div class="cfg-row"><label>Vibrato (CC1)</label><input type="number" id="cfgCCMod" min="0" max="127"></div>
    <div class="cfg-row"><label>Souffle (CC2)</label><input type="number" id="cfgCCBreath" min="0" max="127"></div>
    <div class="cfg-row"><label>Brillance (CC74)</label><input type="number" id="cfgCCBright" min="0" max="127"></div>
  </div>

  <div class="section"><h3>Economie d'energie</h3>
    <div class="cfg-row"><label>Eteindre servos apres (ms)</label><input type="number" id="cfgUnpower" min="0" max="60000"></div>
    <div style="font-size:.7em;color:#666;margin:-4px 0 6px 148px">Coupe l'alimentation des servos apres inactivite (0 = toujours actifs)</div>
  </div>

  <div class="section"><h3>Stockage fichiers MIDI</h3>
    <div class="cfg-row"><label>Espace maximum (KB)</label><input type="number" id="cfgMidiLimit" min="50" max="2000" step="50"></div>
    <div style="font-size:.7em;color:#666;margin:-4px 0 6px 148px">Limite de stockage pour les fichiers .mid uploades</div>
  </div>

  <div class="section"><h3>Affichage</h3>
    <div class="cfg-row"><label>Couleur de l'instrument</label><input type="color" id="cfgColor" value="#D4B044" style="width:48px;height:48px;flex:0;padding:0;border:2px solid #555;border-radius:6px;cursor:pointer"></div>
    <div class="cfg-row"><label>Masquer onglet Calibration</label><input type="checkbox" id="cfgHideCalib" style="width:auto;flex:0"></div>
    <div class="cfg-row"><label>Masquer onglet Air</label><input type="checkbox" id="cfgHideAir" style="width:auto;flex:0"></div>
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
const MIDI_CC_MAX=127,MIDI_VEL_MAX=127,MAX_FINGERS=31;
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
[82,[0,1,1,1,1,1],10,60,40],[83,[1,1,1,1,1,1],0,50,40],
[84,[0,0,0,0,0,0],20,75,42],[86,[0,0,0,0,0,1],15,70,44],[88,[0,0,0,0,1,1],10,65,46],[89,[0,0,0,1,1,1],10,60,48],
[91,[0,0,1,1,1,1],5,55,50],[93,[0,1,1,1,1,1],5,50,52],[95,[1,1,1,1,1,1],0,45,54],
[96,[0,0,0,0,0,0],50,100,55],[98,[0,0,0,0,0,1],45,95,57],[100,[0,0,0,0,1,1],40,90,59],[101,[0,0,0,1,1,1],35,85,61],
[103,[0,0,1,1,1,1],30,80,63]]},
{id:'bansuri_a',n:'Bansuri (La)',h:6,th:-1,em:'trav',d:[
[64,[0,0,0,0,0,0],5,45,40],[66,[0,0,0,0,0,1],5,45,42],[68,[0,0,0,0,1,1],5,50,44],
[69,[0,0,0,1,1,1],10,50,46],[71,[0,0,1,1,1,1],10,55,48],[73,[0,1,1,1,1,1],10,55,50],
[74,[1,0,0,0,0,0],15,60,52],[76,[0,0,0,0,0,0],20,65,55],[78,[0,0,0,0,0,1],20,65,57],[80,[0,0,0,0,1,1],20,70,59],
[81,[0,0,0,1,1,1],35,80,60],[83,[0,0,1,1,1,1],35,85,62],[85,[0,1,1,1,1,1],40,85,65]]},
{id:'dizi_d',n:'Dizi (R\u00e9)',h:6,th:-1,em:'trav',d:[
[69,[0,0,0,0,0,0],5,45,42],[71,[0,0,0,0,0,1],5,45,44],[73,[0,0,0,0,1,1],5,50,46],
[74,[0,0,0,1,1,1],10,50,48],[76,[0,0,1,1,1,1],10,55,50],[78,[0,1,1,1,1,1],10,55,52],
[81,[0,0,0,0,0,0],30,75,55],[83,[0,0,0,0,0,1],30,75,57],[85,[0,0,0,0,1,1],30,80,59],
[86,[0,0,0,1,1,1],35,80,60],[88,[0,0,1,1,1,1],35,85,62],[90,[0,1,1,1,1,1],40,85,65]]},
{id:'fife_bb',n:'Fifre (Sib)',h:6,th:-1,em:'trav',d:[
[70,[0,0,0,0,0,0],10,55,42],[72,[0,0,0,0,0,1],10,55,44],[74,[0,0,0,0,1,1],10,55,46],[75,[0,0,0,1,1,1],10,60,48],
[77,[0,0,1,1,1,1],10,60,50],[79,[0,1,1,1,1,1],10,60,52],[81,[1,1,1,1,1,1],10,65,54],
[82,[0,0,0,0,0,0],35,80,55],[84,[0,0,0,0,0,1],35,80,57],[86,[0,0,0,0,1,1],35,85,59],[87,[0,0,0,1,1,1],40,85,60],
[89,[0,0,1,1,1,1],40,90,62],[91,[0,1,1,1,1,1],40,90,64],[93,[1,1,1,1,1,1],45,95,65]]},
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
function markDirty(){dirty=true;$('unsavedBadge').classList.add('show');updStepDots();const sb=$('btnAirSave');if(sb)sb.style.boxShadow='0 0 8px #4ecca3';updateConfigSummary()}
function markClean(){dirty=false;$('unsavedBadge').classList.remove('show');updStepDots();const sb=$('btnAirSave');if(sb)sb.style.boxShadow='';const cs=$('airConfigSummary');if(cs)cs.style.display='none'}
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
  if(CFG.embouchure==='trav'&&CFG.angle_on){const angP=$('cfgAngPca');if(angP){const av=parseInt(angP.value);used[av]='Servo Angle'}}
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
  // Refresh flute SVG + air diagram on keyboard tab entry
  if(id==='keyboard'){if(CFG){buildFlute(CFG,'fluteSvg',false);refreshKbdAir()}if(lastAirData)updateAirDiagram(lastAirData);}
}
// --- Air System (modulaire) ---
const AIR_LAYOUT_DESCS=[
  'Source air par pompe(s). Activez/desactivez chaque composant avec le bouton toggle. Desactivez pompe et reservoir pour une alimentation externe (compresseur, bouche).',
  'Ventilateur radial PWM souffle en continu. Le servo flow dirige le flux vers la flute.'
];
const AIR_DESCS=[
  'Valve solenoide coupe l\'air, servo flow regle le debit. Ideal pour alimentation externe (compresseur, bouche).',
  'Valve servo PCA coupe l\'air, servo flow regle le debit. Alternative silencieuse au solenoide.',
  'Servo flow seul. L\'angle min coupe l\'air entre les notes. Simple, un seul servo suffit.',
  'Ventilateur PWM souffle en continu. Le servo flow dirige le flux vers la flute. Bonne puissance.',
  'Pompe(s) directe(s) + valve. Souffle direct sans reservoir. 1-3 pompes en parallele.',
  'Pompe(s) + reservoir + capteur. Regulation PID automatique de la pression. Configuration la plus complete.'
];
const AIR_PARTS=[
  ['Servo flow PCA','Valve solenoide'],
  ['Servo flow PCA','Valve servo PCA'],
  ['Servo flow PCA'],
  ['Servo flow PCA','Ventilateur PWM'],
  ['Servo flow PCA','Pompe(s)','Valve'],
  ['Servo flow PCA','Pompe(s)','Valve','Reservoir','Capteur']
];
const PWM_GPIOS=[2,4,5,12,13,14,15,16,17,18,19,21,22,23,25,26,27,32,33];
let lastAirData=null;
function applyAirTabVisibility(){
  const airBtn=$('btnTabAir');
  if(CFG&&CFG.hide_air){airBtn.style.display='none';
    if($('tab-air').classList.contains('active')){showTab('keyboard',document.querySelector('.tabs button'))}
  }else{airBtn.style.display=''}
  if(CFG){buildFlute(CFG,'fluteSvg',false);refreshKbdAir()}
}
function refreshKbdAir(){
  const box=$('kbdAirBox');if(!box)return;
  const fw=$('kbdFluteWrap');
  if(CFG&&CFG.show_air){
    box.style.display='';buildAirSvg('kbdAirSvg',true);
    if(fw){fw.style.paddingTop='0';box.style.paddingTop='0';requestAnimationFrame(alignFluteWithAir)}
  }else{
    box.style.display='none';
    if(fw){fw.style.paddingTop='0';box.style.paddingTop='0'}
  }
  refreshKbdPumpPanel();
}
function alignFluteWithAir(){
  var airSvg=$('kbdAirSvg'),fs=$('fluteSvg'),fw=$('kbdFluteWrap'),box=$('kbdAirBox');
  if(!airSvg||!fs||!fw||!box||!CFG||_kbdPipeExitY<=0)return;
  // Skip alignment on narrow screens (column layout)
  var airRect=airSvg.getBoundingClientRect(),fluteRect=fs.getBoundingClientRect();
  if(!airRect.height||!fluteRect.height)return;
  var airScale=airRect.height/_kbdAirVBH;
  var fvb=fs.viewBox.baseVal;
  var fluteScale=fluteRect.height/(fvb?fvb.height:100);
  var em=CFG.embouchure||'bec';
  var embY=(em==='trav')?38:50;
  var pipePixY=_kbdPipeExitY*airScale;
  var embPixY=embY*fluteScale;
  var offset=Math.round(pipePixY-embPixY);
  if(offset>=0){fw.style.paddingTop=offset+'px';box.style.paddingTop='0'}
  else{box.style.paddingTop=(-offset)+'px';fw.style.paddingTop='0'}
}
let _kbdPumpOn=false;
function refreshKbdPumpPanel(){
  const p=$('kbdPumpPanel');if(!p||!CFG)return;
  const m=CFG.air_mode||0;
  if(m<4){p.style.display='none';return}
  p.style.display='';
  const hasRes=(m===5),st=CFG.sens_type||0,mt=CFG.motor_type||0;
  const lbl=$('kbdPumpTargetLabel'),hint=$('kbdPumpHint'),mode=$('kbdPumpMode'),title=$('kbdPumpTitle');
  if(hasRes){
    if(st>=3){lbl.textContent='Remplissage';hint.textContent='Fin de course: pompe ON/OFF selon capteur';
      mode.textContent='Endstop '+(st===3?'mecanique':'optique')}
    else if(st===2){lbl.textContent='Niveau cible';hint.textContent='Hall: regulation '+(mt===0?'bang-bang':'PID');
      mode.textContent='Hall + '+(mt===0?'Bang-bang':'PID')}
    else{lbl.textContent='Hauteur cible';hint.textContent='ToF: regulation '+(mt===0?'bang-bang':'PID');
      mode.textContent='ToF + '+(mt===0?'Bang-bang':'PID')}
    title.textContent='Pompe + Reservoir';
  }else{
    lbl.textContent='Puissance pompe';hint.textContent='PWM proportionnel direct';
    mode.textContent='Direct';title.textContent='Pompe directe';
  }
}
function kbdTogglePump(){
  const btn=$('kbdPumpBtn');if(!btn)return;
  _kbdPumpOn=!_kbdPumpOn;
  if(_kbdPumpOn){
    const v=parseInt($('kbdPumpTarget').value)||50;
    wsSend({t:'pump_target',v:v});
    btn.textContent='Arreter pompe';btn.className='on';
  }else{
    wsSend({t:'pump_stop'});
    btn.textContent='Demarrer pompe';btn.className='off';
  }
}
function kbdSetPumpTarget(v){
  const pv=$('kbdPumpTargetVal');pv.textContent=v+'%';
  pv.style.color=v>70?'#e94560':v>30?'#e9a645':v>0?'#4ecca3':'';
  if(_kbdPumpOn)wsSend({t:'pump_target',v:parseInt(v)});
}
// Compute firmware air_mode from layout + component toggles
function getAirMode(){
  const layout=parseInt(document.querySelector('input[name=airLayout]:checked').value)||0;
  if(layout===1)return 3; // fan mode
  // Pump/reservoir layout: check which components are enabled
  const pumpOn=!$('airBlockPump').classList.contains('disabled');
  const resOn=!$('airBlockRes').classList.contains('disabled');
  const valveOn=!$('airBlockValve').classList.contains('disabled');
  if(pumpOn&&resOn)return 5;
  if(pumpOn)return 4;
  if(valveOn){
    // Mode 0 = solenoide, mode 1 = servo-valve
    const vt=parseInt($('airValveType').value)||0;
    return vt===1?1:0;
  }
  return 2; // servo flow only
}
function getAirLayout(){
  const r=document.querySelector('input[name=airLayout]:checked');
  return r?parseInt(r.value)||0:0;
}
function setAirTarget(v){
  const pv=$('pumpTargetVal');pv.textContent=v+'%';
  pv.style.color=v>70?'#e94560':v>30?'#e9a645':v>0?'#4ecca3':'';
  const m=getAirMode();
  if(m===3)wsSend({t:'fan_target',v:parseInt(v)});
  else wsSend({t:'pump_target',v:parseInt(v)});
}
function stopAirSource(){
  const m=getAirMode();
  if(m===3)wsSend({t:'fan_stop'});
  else wsSend({t:'pump_stop'});
  $('pumpTarget').value=0;$('pumpTargetVal').textContent='0%';$('pumpTargetVal').style.color='';
}
function testAirSystem(){
  const m=getAirMode();
  const dur=parseInt($('airTestDur').value)||2000;
  if(m===3)wsSend({t:'fan_target',v:30});
  else wsSend({t:'pump_target',v:30});
  $('pumpTarget').value=30;$('pumpTargetVal').textContent='30%';
  $('btnAirTest').classList.add('test-pulse');
  setTimeout(()=>{stopAirSource();$('btnAirTest').classList.remove('test-pulse')},dur);
}
function testSinglePump(idx){
  wsSend({t:'pump_target',v:30,pump:idx});
  setTimeout(()=>wsSend({t:'pump_stop',pump:idx}),2000);
  showToast('Test pompe '+(idx+1)+' en cours...','success');
}
function rotateServoNeedle(angle){
  const mn=CFG?(CFG.air_min||0):0,mx=CFG?(CFG.air_max||180):180;
  const pcta=Math.min(1,Math.max(0,(angle-mn)/(mx-mn||1)));
  const deg=-60+pcta*120;
  document.querySelectorAll('[id=airServoNeedle]').forEach(nd=>{
    const dm=nd.getAttribute('d');
    const mp=dm?dm.match(/M([\d.]+),([\d.]+)/):null;
    const cx=mp?parseFloat(mp[1]):0;
    const cy2=mp?parseFloat(mp[2]):0;
    nd.setAttribute('transform','rotate('+deg+','+cx+','+cy2+')');
  });
  // Fan duct: immediate rotation feedback from slider
  const off=CFG?(CFG.air_off||mn):mn;
  const pctDuct=Math.min(1,Math.max(0,(angle-off)/(mx-off||1)));
  document.querySelectorAll('[id=airFanDuct]').forEach(fd=>{
    fd.style.transition='transform 0.15s ease';
    fd.style.transform='rotate('+(-30+pctDuct*30)+'deg)';
  });
}
let _servoFlowTimer=null;
function testServoFlow(v){
  const sl=$('airFlowTest');if(!sl)return;
  const mn=parseInt(sl.min)||0,mx=parseInt(sl.max)||180;
  const a=Math.max(mn,Math.min(mx,parseInt(v)||0));
  const pct=Math.round((a-mn)/(mx-mn||1)*100);
  $('airFlowTestVal').textContent=pct+'%';
  rotateServoNeedle(a);
  if(_servoFlowTimer)clearTimeout(_servoFlowTimer);
  _servoFlowTimer=setTimeout(()=>{wsSend({t:'test_air',a:a});_servoFlowTimer=null},30);
}
function updateFlowSliderRange(){
  const sl=$('airFlowTest');if(!sl)return;
  const m=getAirMode();const needsOff=(m===2||m===3);
  const off=needsOff?(parseInt($('cfgAirOff').value)||0):0;
  const mn=parseInt($('cfgAirMin').value)||0,mx=parseInt($('cfgAirMax').value)||180;
  const lo=needsOff?Math.min(off,mn):mn;
  sl.min=lo;sl.max=mx;
  const cur=parseInt(sl.value)||0;
  if(cur<lo){sl.value=lo;testServoFlow(lo)}
  else if(cur>mx){sl.value=mx;testServoFlow(mx)}
}
function applyServoPreset(mn,mx){
  const m=getAirMode();const needsOff=(m===2||m===3);
  const offEl=$('cfgAirOff');
  if(needsOff&&offEl)offEl.value=Math.max(0,mn-5);
  $('cfgAirMin').value=mn;$('cfgAirMax').value=mx;
  markDirty();validateAirConfig();updateFlowSliderRange();buildAirSvg('airSvgFull',true);
}
function updPwmPct(el){const pct=el.nextElementSibling;if(pct)pct.textContent=Math.round(parseInt(el.value||0)/255*100)+'%'}
function flashSvgElement(id){
  const el=document.getElementById(id);if(!el)return;
  el.style.transition='opacity .15s';el.style.opacity='.4';
  setTimeout(()=>{el.style.opacity='1'},150);
}
function gotoServoAngle(inputId){
  const v=parseInt($(inputId).value)||0;
  $('airFlowTest').value=v;testServoFlow(v);
}
let _diagRunning=false,_diagTimeouts=[];
function cancelDiagnostic(){
  _diagTimeouts.forEach(id=>clearTimeout(id));_diagTimeouts=[];
  _diagRunning=false;
  const dm=$('airDiagMsg'),db=$('btnAirDiag'),dbar=$('airDiagBar');
  if(dm){dm.textContent='Diagnostic annule';dm.style.color='#e9a645';
    setTimeout(()=>{dm.style.opacity='0';setTimeout(()=>{dm.style.display='none';dm.style.opacity='1'},300)},2000)}
  if(db){db.disabled=false;db.textContent='Diagnostic'}
  if(dbar)dbar.style.display='none';
  stopAirSource();
}
function runAirDiagnostic(){
  if(_diagRunning){cancelDiagnostic();return}
  if(!(ws&&ws.readyState===1)){showToast('Non connecte - diagnostic impossible','error');return}
  const dm=$('airDiagMsg');if(!dm)return;
  const db=$('btnAirDiag'),dbar=$('airDiagBar'),dfill=$('airDiagFill');
  _diagRunning=true;_diagTimeouts=[];
  dm.style.display='';dm.style.opacity='1';dm.style.color='#9aa';
  if(db){db.disabled=false;db.textContent='Annuler'}
  if(dbar)dbar.style.display='';
  if(dfill)dfill.style.width='0';
  const m=getAirMode();
  const hasValve=(m===0||m===1||m>=4),hasPump=m>=4,hasFan=m===3,hasRes=m===5;
  const needsOff=(m===2||m===3);
  const steps=[];let t=0;
  // Build test sequence based on mode
  if(needsOff){
    steps.push({t:t,msg:'Servo flow -> Off...',fn:()=>testServoFlow(parseInt($('cfgAirOff').value)||0)});t+=800;
  }
  steps.push({t:t,msg:'Servo flow -> min...',fn:()=>testServoFlow(parseInt($('cfgAirMin').value)||0)});t+=800;
  steps.push({t:t,msg:'Servo flow -> max...',fn:()=>testServoFlow(parseInt($('cfgAirMax').value)||180)});t+=800;
  if(needsOff){
    steps.push({t:t,msg:'Servo flow -> Off',fn:()=>testServoFlow(parseInt($('cfgAirOff').value)||0)});t+=800;
  }else{
    steps.push({t:t,msg:'Servo flow -> min',fn:()=>testServoFlow(parseInt($('cfgAirMin').value)||0)});t+=800;
  }
  if(hasValve){
    steps.push({t:t,msg:'Valve -> ouvrir...',fn:()=>wsSend({t:'test_sol',o:1})});t+=800;
    steps.push({t:t,msg:'Valve -> fermer',fn:()=>wsSend({t:'test_sol',o:0})});t+=800;
  }
  if(hasPump){
    steps.push({t:t,msg:'Pompe -> 30%...',fn:()=>{wsSend({t:'pump_target',v:30});const pt2=$('pumpTarget');if(pt2)pt2.value=30;const pv=$('pumpTargetVal');if(pv)pv.textContent='30%'}});
    steps.push({t:t+1300,msg:'Pompe -> arret',fn:()=>stopAirSource()});t+=2100;
  }
  if(hasFan){
    steps.push({t:t,msg:'Ventilateur -> 30%...',fn:()=>{wsSend({t:'fan_target',v:30});const pt2=$('pumpTarget');if(pt2)pt2.value=30;const pv=$('pumpTargetVal');if(pv)pv.textContent='30%'}});
    steps.push({t:t+1300,msg:'Ventilateur -> arret',fn:()=>stopAirSource()});t+=2100;
  }
  if(hasRes){
    steps.push({t:t,msg:'Capteur reservoir -> verification...',fn:()=>{
      const sok=$('airSensorOk');
      if(sok&&sok.textContent==='OK'){dm.style.color='#4ecca3'}
      else{dm.style.color='#e9a645'}
    }});
  }
  const last=steps[steps.length-1];
  steps.push({t:last.t+800,msg:'Diagnostic termine !',fn:()=>{
    dm.style.color='#4ecca3';_diagRunning=false;
    if(db){db.disabled=false;db.textContent='Diagnostic'}
    setTimeout(()=>{dm.style.opacity='0';setTimeout(()=>{dm.style.display='none';dm.style.opacity='1';if(dbar)dbar.style.display='none'},300)},5000)}});
  const total=steps.length;
  steps.forEach((s,i)=>{
    const tid=setTimeout(()=>{
      if(!_diagRunning)return;
      dm.textContent='['+(i+1)+'/'+total+'] '+s.msg;
      if(dfill)dfill.style.width=((i+1)/total*100)+'%';
      s.fn();
    },s.t);
    _diagTimeouts.push(tid);
  });
}
let sweepTimer=null;
function sweepServoFlow(){
  const btn=event&&event.target;
  if(sweepTimer){clearInterval(sweepTimer);sweepTimer=null;if(btn)btn.style.background='';return}
  const sl=$('airFlowTest');if(!sl)return;
  if(btn)btn.style.background='rgba(78,204,163,.25)';
  const m=getAirMode();const needsOff=(m===2||m===3);
  const aOff=needsOff?(parseInt($('cfgAirOff').value)||0):null;
  const aMin=parseInt($('cfgAirMin').value)||0,aMax=parseInt($('cfgAirMax').value)||180;
  const steps=[],stepMs=80,range=aMax-aMin;
  const nUp=Math.max(1,Math.round(range/4)),nDown=nUp;
  const startEnd=aOff!=null?aOff:aMin;
  steps.push(startEnd);
  for(let i=0;i<=nUp;i++)steps.push(aMin+Math.round(range*i/nUp));
  for(let i=nDown-1;i>=0;i--)steps.push(aMin+Math.round(range*i/nDown));
  steps.push(startEnd);
  let idx=0;
  sweepTimer=setInterval(()=>{
    if(idx>=steps.length){clearInterval(sweepTimer);sweepTimer=null;if(btn)btn.style.background='';return}
    const a=steps[idx++];sl.value=a;testServoFlow(a);
  },stepMs);
}
let _prevAirMode=0;
// Switch between pump/reservoir and fan layout
function setAirLayout(layout){
  // Update radio buttons visual
  const btns=document.querySelectorAll('.air-layout-btn');
  btns.forEach(b=>b.classList.remove('selected'));
  const sel=layout===1?$('airLayoutFan'):$('airLayoutPump');
  if(sel)sel.classList.add('selected');
  const radio=sel?sel.querySelector('input[type=radio]'):null;
  if(radio)radio.checked=true;
  applyAirLayout(layout);
  markDirty();
}
function applyAirLayout(layout){
  const isFan=(layout===1);
  // Fan layout: show fan block, hide pump+reservoir blocks
  // Pump layout: show pump+reservoir+valve blocks (always visible, greyed if disabled)
  const bp=$('airBlockPump'),bf=$('airBlockFan'),br=$('airBlockRes'),bv=$('airBlockValve');
  if(isFan){
    // Fan mode: show fan, hide pump+reservoir, hide valve (fan has no valve)
    if(bf){bf.style.display='';bf.classList.add('active');setBlockEnabled(bf,true)}
    if(bp)bp.style.display='none';
    if(br)br.style.display='none';
    if(bv)bv.style.display='none';
  }else{
    // Pump/reservoir layout: always show pump, reservoir, valve (enabled/disabled via toggle)
    if(bf)bf.style.display='none';
    if(bp){bp.style.display='';bp.classList.add('active')}
    if(br){br.style.display='';br.classList.add('active')}
    if(bv){bv.style.display='';bv.classList.add('active')}
  }
  // Servo flow always visible
  const bs=$('airBlockServo');if(bs){bs.style.display='';bs.classList.add('active')}
  // Update computed mode and refresh UI
  syncAirModeFromToggles();
}
// Helper: set a block enabled or disabled
function setBlockEnabled(bl,enabled){
  if(!bl)return;
  const tg=bl.querySelector('.air-block-toggle');
  if(tg){
    if(enabled){tg.classList.add('on');tg.setAttribute('aria-checked','true')}
    else{tg.classList.remove('on');tg.setAttribute('aria-checked','false')}
  }
  bl.classList.toggle('disabled',!enabled);
  if(!enabled)bl.classList.remove('active');
  else bl.classList.add('active');
}
// Recompute firmware air_mode from layout + toggles, update all dependent UI
function syncAirModeFromToggles(){
  const m=getAirMode();
  if(CFG)CFG.air_mode=m;
  $('airModeSelect').value=m;
  const hasPump=m>=4,hasFan=m===3,hasValve=(m===0||m===1||m>=4),hasRes=m===5;
  // Servo flow: show note Off angle only for modes without valve
  const needsOff=!hasValve;
  const offRow=$('cfgAirOffRow');if(offRow)offRow.style.display=needsOff?'':'none';
  const offHint=$('airServoOffHint');
  if(offHint)offHint.textContent=needsOff?'Position Off = coupure d\'air entre les notes. Min/Max = plage souffle note active.':'La valve coupe l\'air. Min/Max = plage de debit du servo flow.';
  const mnLabel=$('cfgAirMinLabel');if(mnLabel)mnLabel.textContent=needsOff?'Position note On min':'Angle min';
  const mxLabel=$('cfgAirMaxLabel');if(mxLabel)mxLabel.textContent=needsOff?'Position note On max':'Angle max';
  const bgo=$('btnGotoOff');if(bgo)bgo.style.display=needsOff?'':'none';
  toggleValveParams(true);
  // Live stats visibility
  const sp=$('airStatPump');if(sp)sp.style.display=hasPump?'':'none';
  const sap=$('airStatActivePumps');if(sap)sap.style.display=(hasPump&&CFG&&CFG.num_pumps>1)?'':'none';
  const sf=$('airStatFan');if(sf)sf.style.display=hasFan?'':'none';
  const sr=$('airStatRes');if(sr)sr.style.display=hasRes?'':'none';
  const ss=$('airStatSensor');if(ss)ss.style.display=hasRes?'':'none';
  const spe=$('airStatPidErr');if(spe)spe.style.display=hasRes?'':'none';
  if(hasRes)toggleSensorStats();
  else{const sd=$('airStatDist');if(sd)sd.style.display='none';
    const sh=$('airStatHall');if(sh)sh.style.display='none';
    const se=$('airStatEndstop');if(se)se.style.display='none';}
  const sv=$('airStatValve');if(sv)sv.style.display=(hasValve)?'':'none';
  // Control section
  const cs=$('airCtrlSection');if(cs){cs.style.display='';
    $('airCtrlTitle').textContent=(hasPump||hasFan)?
      (hasFan?'Controle ventilateur':'Controle pompe'):'Controle servo flow';
    $('btnAirStop').textContent=hasFan?'Arreter ventilateur':'Arreter pompe';
    // Adapter label slider selon contexte mesure
    if(hasFan){$('airTargetLabel').textContent='Vitesse'}
    else if(hasRes){
      const st=CFG?CFG.sens_type:0;
      if(st>=3)$('airTargetLabel').textContent='Remplissage';
      else if(st===2)$('airTargetLabel').textContent='Niveau cible';
      else $('airTargetLabel').textContent='Hauteur cible';
    }else if(hasPump){$('airTargetLabel').textContent='Puissance pompe'}
    // Hint contextuel sous le slider
    const th=$('airTargetHint');if(th){
      if(hasRes){const st=CFG?CFG.sens_type:0;
        if(st>=3){th.textContent='Fin de course: pompe ON tant que non declenche';th.style.display=''}
        else if(st===2){th.textContent='Capteur Hall: regulation niveau par bang-bang ou PID';th.style.display=''}
        else{th.textContent='Capteur ToF: regulation distance par bang-bang ou PID';th.style.display=''}
      }else if(hasPump){th.textContent='Mode direct: PWM proportionnel a la demande';th.style.display=''}
      else if(hasFan){th.textContent='PWM ventilateur proportionnel';th.style.display=''}
      else{th.style.display='none'}
    }
    $('btnAirStop').style.display=(hasPump||hasFan)?'':'none';
    const pr=$('airCtrlPumpRow');if(pr)pr.style.display=(hasPump||hasFan)?'':'none';}
  const vo=$('btnValveOpen'),vc=$('btnValveClose'),bt=$('btnAirTest');
  if(vo)vo.style.display=(hasValve)?'':'none';
  if(vc)vc.style.display=(hasValve)?'':'none';
  if(bt)bt.style.display=(hasPump||hasFan)?'':'none';
  const td=$('airTestDur');if(td)td.style.display=(hasPump||hasFan)?'':'none';
  if(hasRes)toggleSensorParams();
  // Mode description
  const md=$('airModeDesc');if(md){
    const layout=getAirLayout();
    const activeParts=[];
    if(hasFan)activeParts.push('Ventilateur PWM');
    if(hasPump)activeParts.push('Pompe(s)');
    if(hasRes)activeParts.push('Reservoir + capteur');
    if(hasValve)activeParts.push('Valve');
    activeParts.push('Servo flow PCA');
    const badges=activeParts.map(p=>'<span style="display:inline-block;font-size:.7em;background:#0f3460;color:#4ecca3;padding:1px 6px;border-radius:8px;margin:2px 2px 0 0">'+p+'</span>').join('');
    md.innerHTML=(AIR_LAYOUT_DESCS[layout]||'')+'<div style="margin-top:4px">'+badges+'</div>';
  }
  // Update status bar
  const modeNames=['Valve+flow','Servo-valve','Servo seul','Ventilateur','Pompe directe','Pompe+reservoir'];
  const sm=$('airStatusMode');if(sm)sm.textContent=modeNames[m]||'?';
  validateAirConfig();
  buildAirSvg('airSvgFull',true);
  if(CFG){buildFlute(CFG,'fluteSvg',false);refreshKbdAir()}
}
// Legacy compat wrapper
function setAirMode(v){
  const m=parseInt(v);
  // Map firmware mode to layout + toggles
  if(m===3){
    // Fan layout
    setAirLayout(1);
  }else{
    // Pump/reservoir layout — set toggles based on mode
    setAirLayout(0);
    const bp=$('airBlockPump'),br=$('airBlockRes'),bv=$('airBlockValve');
    setBlockEnabled(bp,m>=4); // pump enabled for modes 4,5
    setBlockEnabled(br,m===5); // reservoir enabled for mode 5
    setBlockEnabled(bv,m===0||m===1||m>=4); // valve enabled for modes 0,1,4,5
    // Mode 1 = servo-valve: set valve type to servo
    if(m===1){const vt=$('airValveType');if(vt)vt.value='1';toggleValveParams()}
  }
  syncAirModeFromToggles();
}
let _allBlocksExpanded=true;
function toggleAirHelp(){const hp=$('airHelpPanel');if(hp)hp.style.display=hp.style.display==='none'?'':'none'}
function toggleAllAirBlocks(){
  _allBlocksExpanded=!_allBlocksExpanded;
  document.querySelectorAll('.air-block').forEach(bl=>{
    if(bl.style.display==='none'||bl.classList.contains('disabled'))return;
    if(_allBlocksExpanded)bl.classList.add('active');
    else bl.classList.remove('active');
  });
}
function toggleAirBlock(id,e){
  if(e&&e.type==='keydown'&&e.key!=='Enter'&&e.key!==' ')return;
  if(e&&e.key===' ')e.preventDefault();
  const bl=$(id);if(!bl||bl.classList.contains('disabled'))return;
  bl.classList.toggle('active');
}
function toggleAirBlockEnable(id){
  const bl=$(id);if(!bl)return;
  const tg=bl.querySelector('.air-block-toggle');if(!tg)return;
  const on=tg.classList.toggle('on');
  tg.setAttribute('aria-checked',on?'true':'false');
  bl.classList.toggle('disabled',!on);
  if(!on)bl.classList.remove('active');
  else bl.classList.add('active');
  // Recompute firmware mode from new toggle state
  syncAirModeFromToggles();
  markDirty();
}
function updateAngleBlockVisibility(){
  const bl=$('airBlockAngle');if(!bl||!CFG)return;
  const isTrav=(CFG.embouchure==='trav');
  bl.style.display=isTrav?'block':'none';
  if(!isTrav){
    // Disable angle servo when switching away from traversiere
    const tg=$('airBlockAngleToggle');
    if(tg){tg.classList.remove('on');tg.setAttribute('aria-checked','false')}
    bl.classList.add('disabled');bl.classList.remove('active');
  }
}
function toggleAngleServo(){
  const tg=$('airBlockAngleToggle');if(!tg)return;
  const on=tg.classList.toggle('on');
  tg.setAttribute('aria-checked',on?'true':'false');
  const bl=$('airBlockAngle');
  if(bl){bl.classList.toggle('disabled',!on);if(on)bl.classList.add('active');else bl.classList.remove('active')}
  if(CFG)CFG.angle_on=on;
  validateAirConfig();buildAirSvg('airSvgFull',true);markDirty();
}
function syncAngPca(v){
  const val=parseInt(v);if(CFG)CFG.ang_pca=val;
  const a=$('cfgAngPca'),b=$('calAngPca');
  if(a)a.value=val;if(b)b.value=val;
  markDirty();
}
function toggleValveParams(noMark){
  const isSol=$('airValveType').value==='0';
  const solP=$('airValveSolParams');if(solP)solP.style.display=isSol?'':'none';
  const srvP=$('airValveServoParams');if(srvP)srvP.style.display=isSol?'none':'';
  if(!noMark)markDirty();
}
function toggleMotorType(){
  buildPumpRows();
  toggleCascadeRow();
  const mh=$('airMotorHelp');if(mh)mh.textContent=$('airMotorType').value==='1'?'On/Off: pompe demarre/arrete uniquement':'PWM: controle vitesse 0-255';
}
function toggleCascadeRow(){
  const np=parseInt($('airNumPumps').value)||1;
  const mt=parseInt($('airMotorType').value)||0;
  const m=getAirMode();
  const cr=$('airCascadeRow');if(cr)cr.style.display=(np>1&&m>=4)?'':'none';
  const br=$('airBangbangRow');if(br)br.style.display=(mt===1&&m===5)?'':'none';
  const ap=$('airStatActivePumps');if(ap)ap.style.display=(np>1&&m>=4)?'':'none';
}
function toggleSensorParams(){
  const st=parseInt($('airSensorType').value);
  const isTof=(st<=1),isHall=(st===2),isEndstop=(st>=3);
  const hasPid=(isTof||isHall);
  $('airSensParamsTof').style.display=isTof?'':'none';
  $('airSensParamsHall').style.display=isHall?'':'none';
  $('airSensParamsEndstop').style.display=isEndstop?'':'none';
  $('airSensParamsPid').style.display=hasPid?'':'none';
  // Show live sensor value panel
  const lv=$('airSensLive');
  if(lv){
    lv.style.display='';
    const ll=$('airSensLiveLabel');
    if(ll)ll.textContent=isTof?'Distance live':isHall?'Valeur Hall live':'Etat fin course';
  }
  updateHallBar();
}
function toggleSensorStats(){
  const st=parseInt($('airSensorType').value);
  const sd=$('airStatDist');if(sd)sd.style.display=(st<=1)?'':'none';
  const sh=$('airStatHall');if(sh)sh.style.display=(st===2)?'':'none';
  const se=$('airStatEndstop');if(se)se.style.display=(st>=3)?'':'none';
}
function updateHallBar(){
  const lo=$('airHallLow'),hi=$('airHallHigh');
  if(!lo||!hi)return;
  const low=parseInt(lo.value)||0,high=parseInt(hi.value)||4095;
  const tl=$('airHallThreshLow'),th=$('airHallThreshHigh');
  if(tl)tl.style.left=(low/4095*100)+'%';
  if(th)th.style.left=(high/4095*100)+'%';
}
function populateGpioSelect(sel,gpios,val){
  if(!sel)return;
  sel.innerHTML='';
  gpios.forEach(g=>{const o=document.createElement('option');o.value=g;o.textContent='GPIO '+g;sel.appendChild(o)});
  if(val!=null)sel.value=val;
}
function buildPumpRows(){
  const n=parseInt($('airNumPumps').value)||1;
  const mt=parseInt($('airMotorType').value)||0;
  const isPwm=(mt===0);
  const c=$('airPumpRows');if(!c)return;
  let h='';
  for(let i=0;i<n;i++){
    const label=n>1?'Pompe '+(i+1)+' ':'';
    h+='<div style="border-left:2px solid #0f3460;padding-left:8px;margin:8px 0">';
    if(n>1)h+='<div style="font-size:.78em;color:#e94560;font-weight:bold;margin-bottom:4px">Pompe '+(i+1)+'</div>';
    h+='<div class="cfg-row"><label>'+label+'GPIO</label><select id="airPumpPin'+i+'">';
    PWM_GPIOS.forEach(g=>{h+='<option value="'+g+'">GPIO '+g+'</option>'});
    h+='</select></div>';
    if(isPwm){
      h+='<div class="cfg-row"><label>'+label+'PWM min</label><input type="number" id="airPumpMin'+i+'" min="0" max="255" value="80" title="PWM minimum pour demarrer la pompe (seuil de rotation)" oninput="updPwmPct(this)"><span class="pwm-pct" style="font-size:.65em;color:#888;min-width:30px;text-align:right">31%</span></div>';
      h+='<div class="cfg-row"><label>'+label+'PWM max</label><input type="number" id="airPumpMax'+i+'" min="0" max="255" value="255" title="PWM maximum (pleine puissance)" oninput="updPwmPct(this)"><span class="pwm-pct" style="font-size:.65em;color:#888;min-width:30px;text-align:right">100%</span></div>';
    }
    if(n>1)h+='<button class="btn btn-s" onclick="testSinglePump('+i+')" style="font-size:.65em;padding:2px 8px;margin-top:4px" title="Teste cette pompe a 30% pendant 2s">Test pompe '+(i+1)+'</button>';
    h+='</div>';
  }
  c.innerHTML=h;
  // Fill values from config
  if(CFG){
    const pins=CFG.pump_pins||[25,26,27],mins=CFG.pump_mins||[80,80,80],maxs=CFG.pump_maxs||[255,255,255];
    for(let i=0;i<n;i++){
      const pp=$('airPumpPin'+i);if(pp)pp.value=pins[i]||25;
      const pm=$('airPumpMin'+i);if(pm){pm.value=mins[i]||80;updPwmPct(pm)}
      const px=$('airPumpMax'+i);if(px){px.value=maxs[i]||255;updPwmPct(px)}
    }
  }
  validateAirConfig();
}
function validateAirConfig(){
  const m=getAirMode();
  const warns=[];const errBlocks=new Set();
  const hasValve=(m===0||m===1||m>=4);
  // Servo flow angle validation (all modes)
  const needsOff=(m===2||m===3);
  const aOff=needsOff?parseInt($('cfgAirOff').value):0;
  const aMin=parseInt($('cfgAirMin').value),aMax=parseInt($('cfgAirMax').value);
  if(isNaN(aMin)||isNaN(aMax)||(needsOff&&isNaN(aOff))){warns.push('Servo flow: angles manquants');errBlocks.add('airBlockServo')}
  else{
    if(aMin<0||aMin>180||aMax<0||aMax>180){warns.push('Servo flow: angles doivent etre entre 0 et 180');errBlocks.add('airBlockServo')}
    if(needsOff&&(aOff<0||aOff>180)){warns.push('Servo flow: angle Off doit etre entre 0 et 180');errBlocks.add('airBlockServo')}
    if(aMin>=aMax){warns.push('Servo flow: angle min doit etre inferieur a max');errBlocks.add('airBlockServo')}
  }
  // Angle servo validation (trav only)
  if(CFG&&CFG.embouchure==='trav'){
    const angMin=parseInt($('cfgAngMin').value),angMax=parseInt($('cfgAngMax').value),angOff=parseInt($('cfgAngOff').value);
    if(!isNaN(angMin)&&!isNaN(angMax)&&angMin>=angMax){warns.push('Angle servo: min doit etre inferieur a max');errBlocks.add('airBlockAngle')}
    if(!isNaN(angOff)&&(angOff<0||angOff>180)){warns.push('Angle servo: angle repos doit etre entre 0 et 180');errBlocks.add('airBlockAngle')}
    if(!isNaN(angMin)&&!isNaN(angMax)&&(angMin<0||angMin>180||angMax<0||angMax>180)){warns.push('Angle servo: angles doivent etre entre 0 et 180');errBlocks.add('airBlockAngle')}
  }
  // Fan PWM validation
  if(m===3){
    const fmin=parseInt($('airFanMin').value)||0,fmax=parseInt($('airFanMax').value)||255;
    if(fmin>=fmax){warns.push('Ventilateur: PWM min doit etre inferieur a max');errBlocks.add('airBlockFan')}
    if(fmin<0||fmax>255){warns.push('Ventilateur: PWM doit etre entre 0 et 255');errBlocks.add('airBlockFan')}
  }
  // Pump validation
  if(m>=4){
    const n=parseInt($('airNumPumps').value)||1;
    const mt=parseInt($('airMotorType').value)||0;
    const usedPins=[];
    for(let i=0;i<n;i++){
      const pe=$('airPumpPin'+i);if(!pe)continue;
      const p=parseInt(pe.value);
      if(usedPins.includes(p)){warns.push('Pompe '+(i+1)+': GPIO '+p+' deja utilise');errBlocks.add('airBlockPump')}
      usedPins.push(p);
      if(mt===0){
        const mn=parseInt($('airPumpMin'+i).value)||0,mx=parseInt($('airPumpMax'+i).value)||255;
        if(mn>=mx){warns.push('Pompe '+(i+1)+': PWM min >= max');errBlocks.add('airBlockPump')}
      }
    }
  }
  // Valve channel conflict with servo flow PCA channel
  if(hasValve&&parseInt($('airValveType').value)===1){
    const vch=parseInt($('airValveCh').value)||11;
    const ach=CFG?CFG.air_pca||10:10;
    if(vch===ach){warns.push('Valve: canal PCA '+vch+' deja utilise par servo flow');errBlocks.add('airBlockValve')}
  }
  // Servo angle conflict (traversiere, only if enabled)
  const _angleTg=$('airBlockAngleToggle');
  const _angleOn=_angleTg&&_angleTg.classList.contains('on');
  if(CFG&&CFG.embouchure==='trav'&&_angleOn){
    const angleCh=parseInt($('cfgAngPca').value)||12;
    const ach=CFG.air_pca||10;
    if(angleCh===ach){warns.push('Servo Angle: canal PCA '+angleCh+' deja utilise par servo flow');errBlocks.add('airBlockAngle')}
    if(hasValve&&parseInt($('airValveType').value)===1){
      const vch=parseInt($('airValveCh').value)||11;
      if(angleCh===vch){warns.push('Servo Angle: canal PCA '+angleCh+' deja utilise par valve servo');errBlocks.add('airBlockAngle')}
    }
  }
  // Reservoir sensor validation
  if(m===5){
    const st=parseInt($('airSensorType').value);
    if(st===2){
      const lo=parseInt($('airHallLow').value)||0,hi=parseInt($('airHallHigh').value)||4095;
      if(lo>=hi){warns.push('Hall: seuil bas >= haut');errBlocks.add('airBlockRes')}
    }
    if(st<=1){
      const smin=parseInt($('airSensMin').value)||0,smax=parseInt($('airSensMax').value)||300,stgt=parseInt($('airSensTarget').value)||50;
      if(smin>=smax){warns.push('ToF: min >= max');errBlocks.add('airBlockRes')}
      if(stgt<smin||stgt>smax){warns.push('ToF: cible hors plage min-max');errBlocks.add('airBlockRes')}
    }
  }
  // Highlight error blocks and auto-scroll to first error
  let firstErrBlock=null;
  document.querySelectorAll('.air-block').forEach(bl=>{
    const isErr=errBlocks.has(bl.id);
    bl.style.borderColor=isErr?'#e94560':'';
    if(isErr&&!firstErrBlock)firstErrBlock=bl;
  });
  if(firstErrBlock&&warns.length>0){
    firstErrBlock.classList.add('active');
    firstErrBlock.scrollIntoView({behavior:'smooth',block:'center'});
  }
  const box=$('airValidationMsg');
  if(box){
    if(warns.length>0){box.style.display='';box.innerHTML=warns.join('<br>')}
    else{box.style.display='none';box.innerHTML=''}
  }
  const wb=$('airWarnBadge');if(wb)wb.style.display=warns.length>0?'':'none';
  return warns.length===0;
}
function saveAirSettings(){
  if(!validateAirConfig()){
    const msg=$('airSettingsMsg');if(msg){msg.textContent='Corriger les erreurs avant de sauver';msg.style.color='#e94560';
      setTimeout(()=>{msg.textContent='';msg.style.color='#0f0'},3000)}
    const vm=$('airValidationMsg');if(vm){vm.style.animation='airShake .4s';setTimeout(()=>vm.style.animation='',400)}
    return;
  }
  const m=getAirMode();
  const showAir=$('cfgShowAir').checked;
  const rf=$('airResFormat');
  const vt=parseInt($('airValveType').value)||0;
  const hasValve=(m===0||m===1||m>=4);
  const needsOff=(m===2||m===3);
  const d={air_mode:m,valve_type:vt,show_air:showAir,
    res_format:rf?rf.value:'balloon',
    air_min:parseInt($('cfgAirMin').value)||0,air_max:parseInt($('cfgAirMax').value)||180};
  if(needsOff)d.air_off=parseInt($('cfgAirOff').value)||0;
  // Valve params depending on type
  if(hasValve&&vt===0){d.sol_pin=parseInt($('cfgSolPin').value)||13;
    d.sol_act=parseInt($('cfgSolAct').value)||255;d.sol_hold=parseInt($('cfgSolHold').value)||80;
    d.sol_time=parseInt($('cfgSolTime').value)||30;d.sol_inter=parseInt($('cfgSolInter').value)||0}
  if(hasValve&&vt===1){d.valve_ch=parseInt($('airValveCh').value)||11;
    d.vlv_close=parseInt($('cfgVlvClose').value)||0;d.vlv_open=parseInt($('cfgVlvOpen').value)||90;
    d.vlv_dir=parseInt($('cfgVlvDir').value)||0}
  // Servo angle (traversiere)
  if(CFG&&CFG.embouchure==='trav'){
    const tg=$('airBlockAngleToggle');
    const angleOn=tg&&tg.classList.contains('on');
    d.angle_on=!!angleOn;
  }
  if(m===3){d.fan_pin=parseInt($('airFanPin').value)||26;d.fan_min=parseInt($('airFanMin').value)||60;d.fan_max=parseInt($('airFanMax').value)||255;
    d.fan_idle_pct=parseInt($('airFanIdlePct').value)||0;d.fan_idle_timeout=parseInt($('airFanIdleTimeout').value)||0}
  if(m>=4){
    d.motor_type=parseInt($('airMotorType').value)||0;
    const np=parseInt($('airNumPumps').value)||1;d.num_pumps=np;
    d.pump_pins=[];d.pump_mins=[];d.pump_maxs=[];
    for(let i=0;i<np;i++){
      d.pump_pins.push(parseInt($('airPumpPin'+i).value)||25);
      const pmn=$('airPumpMin'+i),pmx=$('airPumpMax'+i);
      d.pump_mins.push(pmn?parseInt(pmn.value)||80:80);
      d.pump_maxs.push(pmx?parseInt(pmx.value)||255:255);
    }
    if(np>1){
      d.pump_cascade=parseInt($('airCascadeThreshold').value)||80;
      d.pump_stagger=parseInt($('airStaggerMs').value)||150;
    }
    if(d.motor_type===1&&m===5){
      d.bb_hyst=parseInt($('airBbHysteresis').value)||5;
    }
  }
  if(m===5){
    d.sens_type=parseInt($('airSensorType').value)||0;
    const st=d.sens_type;
    if(st<=1){d.sens_target=parseInt($('airSensTarget').value)||50;d.sens_min=parseInt($('airSensMin').value)||10;
      d.sens_max=parseInt($('airSensMax').value)||150}
    if(st<=2){d.pid_kp=parseFloat($('airPidKp').value)||30;d.pid_ki=parseFloat($('airPidKi').value)||5}
    if(st===2){d.hall_pin=parseInt($('airHallPin').value)||36;d.hall_low=parseInt($('airHallLow').value)||1500;d.hall_high=parseInt($('airHallHigh').value)||2500}
    if(st>=3){d.endstop_pin=parseInt($('airEndstopPin').value)||34;d.endstop_high=$('airEndstopHigh').value==='1';
      d.endstop_pump_on=$('airEndstopPumpOn').value==='1'}
  }
  // Angle servo (trav only)
  if(CFG&&CFG.embouchure==='trav'&&d.angle_on){
    const _ap=parseInt($('cfgAngPca').value),_ao=parseInt($('cfgAngOff').value),_an=parseInt($('cfgAngMin').value),_ax=parseInt($('cfgAngMax').value);
    d.ang_pca=isNaN(_ap)?12:_ap;d.ang_off=isNaN(_ao)?90:_ao;d.ang_min=isNaN(_an)?60:_an;d.ang_max=isNaN(_ax)?120:_ax;
    d.angle_ch=d.ang_pca}
  const sb=$('btnAirSave');if(sb){sb.disabled=true;sb.textContent='Sauvegarde...'}
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)})
    .then(r=>r.json()).then(j=>{
    if(sb){sb.disabled=false;sb.textContent='Sauvegarder'}
    if(j.ok){showToast('Configuration air sauvegardee','success');
      Object.assign(CFG,d);markClean();applyAirTabVisibility();buildAirSvg('airSvgFull',true);
      const ts=$('airSettingsMsg');if(ts){ts.textContent='Sauvegarde a '+new Date().toLocaleTimeString();ts.style.color='#4ecca3';setTimeout(()=>{ts.textContent=''},5000)}}
    else{showToast('Erreur sauvegarde air','error')}
    }).catch(()=>{if(sb){sb.disabled=false;sb.textContent='Sauvegarder'};showToast('Erreur reseau','error')})
}
function resetAirDefaults(){
  if(!confirm('Reinitialiser les valeurs par defaut pour le mode actuel ?'))return;
  const layout=getAirLayout();
  $('cfgAirMin').value=10;$('cfgAirMax').value=90;
  $('airValveType').value='0';$('airValveCh').value=11;
  $('cfgVlvClose').value=0;$('cfgVlvOpen').value=90;$('cfgVlvDir').value='0';
  // Solenoid defaults
  $('cfgSolPin').value=13;$('cfgSolAct').value=255;$('cfgSolHold').value=80;$('cfgSolTime').value=30;$('cfgSolInter').value=0;
  if(layout===1){
    // Fan defaults
    $('cfgAirOff').value=5;
    $('airFanMin').value=60;$('airFanMax').value=255;$('airFanIdlePct').value=20;$('airFanIdleTimeout').value=5000;
  }else{
    // Pump/reservoir defaults — enable all by default
    const bp=$('airBlockPump'),br=$('airBlockRes'),bv=$('airBlockValve');
    setBlockEnabled(bp,true);setBlockEnabled(br,true);setBlockEnabled(bv,true);
    $('airMotorType').value='0';$('airNumPumps').value=1;toggleMotorType();buildPumpRows();
    $('airCascadeThreshold').value=80;$('airStaggerMs').value=150;$('airBbHysteresis').value=5;toggleCascadeRow();
    $('airSensorType').value='0';$('airSensTarget').value=50;$('airSensMin').value=10;$('airSensMax').value=150;
    $('airPidKp').value=30;$('airPidKi').value=5;toggleSensorParams();
  }
  // Servo angle defaults (traversiere)
  const angleTg=$('airBlockAngleToggle');
  if(angleTg){angleTg.classList.remove('on');angleTg.setAttribute('aria-checked','false')}
  const angleBlk=$('airBlockAngle');if(angleBlk){angleBlk.classList.add('disabled');angleBlk.classList.remove('active')}
  const ap=$('cfgAngPca');if(ap)ap.value=12;
  const ao=$('cfgAngOff');if(ao)ao.value=90;
  const an=$('cfgAngMin');if(an)an.value=60;
  const ax=$('cfgAngMax');if(ax)ax.value=120;
  toggleValveParams();syncAirModeFromToggles();markDirty();
  showToast('Valeurs par defaut appliquees','success');
}
function copyAirConfig(){
  const keys=['air_mode','valve_type','valve_ch','vlv_close','vlv_open','vlv_dir','air_off','air_min','air_max','motor_type','num_pumps',
    'fan_pin','fan_min','fan_max','fan_idle_pct','fan_idle_timeout','sol_pin','sol_act','sol_hold','sol_time','sol_inter','sens_type','sens_target',
    'sens_min','sens_max','pid_kp','pid_ki','hall_pin','hall_low','hall_high','endstop_pin','endstop_high','endstop_pump_on','res_format','show_air',
    'angle_on','angle_ch','ang_pca','ang_off','ang_min','ang_max'];
  const out={};keys.forEach(k=>{if(CFG[k]!=null)out[k]=CFG[k]});
  const json=JSON.stringify(out,null,2);
  if(navigator.clipboard)navigator.clipboard.writeText(json).then(()=>showToast('Config copiee','success'));
  else{const ta=document.createElement('textarea');ta.value=json;document.body.appendChild(ta);ta.select();document.execCommand('copy');ta.remove();showToast('Config copiee','success')}
}
const CFG_LABELS={air_mode:'Mode',air_off:'Off',air_min:'Min',air_max:'Max',fan_min:'Fan min',fan_max:'Fan max',fan_idle_pct:'Fan idle%',fan_idle_timeout:'Fan timeout',pid_kp:'Kp',pid_ki:'Ki',
  valve_type:'Valve',valve_ch:'Canal valve',vlv_close:'Vlv ferme',vlv_open:'Vlv ouvert',vlv_dir:'Vlv sens',
  sol_pin:'Sol GPIO',sol_act:'Sol act',sol_hold:'Sol hold',sol_time:'Sol temps',sol_inter:'Sol inter-note',
  sens_type:'Capteur',sens_target:'Cible',sens_min:'Sens min',sens_max:'Sens max',show_air:'Afficher air'};
function updateConfigSummary(){
  const cs=$('airConfigSummary');if(!cs||!CFG)return;
  const fields=[['air_mode','airModeSelect'],['air_off','cfgAirOff'],['air_min','cfgAirMin'],['air_max','cfgAirMax'],
    ['fan_min','airFanMin'],['fan_max','airFanMax'],['fan_idle_pct','airFanIdlePct'],['fan_idle_timeout','airFanIdleTimeout'],['pid_kp','airPidKp'],['pid_ki','airPidKi'],
    ['valve_type','airValveType'],['valve_ch','airValveCh'],['vlv_close','cfgVlvClose'],['vlv_open','cfgVlvOpen'],['vlv_dir','cfgVlvDir'],
    ['sol_inter','cfgSolInter'],['sens_type','airSensorType'],['sens_target','airSensTarget'],
    ['show_air','cfgShowAir']];
  const changed=[];
  fields.forEach(([k,id])=>{
    const el=$(id);if(!el)return;
    const cur=el.type==='checkbox'?el.checked:parseFloat(el.value);
    const saved=CFG[k];
    if(saved!=null&&cur!=saved)changed.push(CFG_LABELS[k]||k);
  });
  if(changed.length>0){
    cs.style.display='';
    const list=changed.length<=4?changed.join(', '):changed.slice(0,3).join(', ')+' +'+( changed.length-3);
    cs.textContent=changed.length+' modif: '+list;
  }else cs.style.display='none';
}
function importAirConfig(){
  const input=prompt('Coller le JSON de configuration air:');
  if(!input)return;
  try{
    const d=JSON.parse(input);
    if(typeof d!=='object'||d===null){showToast('JSON invalide','error');return}
    if(d.air_mode!=null&&(d.air_mode<0||d.air_mode>5)){showToast('Mode air invalide','error');return}
    // Remap legacy mode 1
    if(d.air_mode===1){d.air_mode=0;d.valve_type=1}
    Object.assign(CFG,d);
    fillAirSettings();buildAirUI();markDirty();
    showToast('Config importee - Sauvegarder pour appliquer','success');
  }catch(e){showToast('JSON invalide: '+e.message,'error')}
}
function fillAirSettings(){
  if(!CFG)return;
  // Remap legacy mode 1 (servo-valve) to mode 0 with valve_type=1
  if(CFG.air_mode===1){CFG.air_mode=0;CFG.valve_type=1}
  $('airValveType').value=(CFG.valve_type||0).toString();
  $('airValveCh').value=CFG.valve_ch||11;
  $('airMotorType').value=(CFG.motor_type||0).toString();
  // Fan GPIO (dynamic populate)
  populateGpioSelect($('airFanPin'),PWM_GPIOS,CFG.fan_pin||26);
  $('airFanMin').value=CFG.fan_min||60;$('airFanMax').value=CFG.fan_max||255;
  $('airFanIdlePct').value=CFG.fan_idle_pct!=null?CFG.fan_idle_pct:20;
  $('airFanIdleTimeout').value=CFG.fan_idle_timeout!=null?CFG.fan_idle_timeout:5000;
  $('airNumPumps').value=CFG.num_pumps||1;
  $('airCascadeThreshold').value=CFG.pump_cascade!=null?CFG.pump_cascade:80;
  $('airStaggerMs').value=CFG.pump_stagger!=null?CFG.pump_stagger:150;
  $('airBbHysteresis').value=CFG.bb_hyst!=null?CFG.bb_hyst:5;
  $('airSensorType').value=CFG.sens_type!=null?CFG.sens_type:1;
  $('airSensTarget').value=CFG.sens_target||50;$('airSensMin').value=CFG.sens_min||10;$('airSensMax').value=CFG.sens_max||150;
  $('airPidKp').value=CFG.pid_kp||30;$('airPidKi').value=CFG.pid_ki||5;
  $('airEndstopPin').value=CFG.endstop_pin||34;$('airEndstopHigh').value=(CFG.endstop_high?'1':'0');
  $('airEndstopPumpOn').value=(CFG.endstop_pump_on?'1':'0');
  $('airHallPin').value=CFG.hall_pin||36;$('airHallLow').value=CFG.hall_low||1500;$('airHallHigh').value=CFG.hall_high||2500;
  // Reservoir format
  const rf=$('airResFormat');if(rf)rf.value=CFG.res_format||'balloon';
  // Servo airflow angles
  $('cfgAirOff').value=CFG.air_off!=null?CFG.air_off:0;
  $('cfgAirMin').value=CFG.air_min!=null?CFG.air_min:0;$('cfgAirMax').value=CFG.air_max!=null?CFG.air_max:180;
  // Solenoid settings
  const sp=$('cfgSolPin');sp.innerHTML='';
  [12,13,16,17,18,19,23,25,26,27,33].forEach(p=>{
    const o=document.createElement('option');o.value=p;o.textContent='GPIO '+p;sp.appendChild(o)});
  sp.value=CFG.sol_pin||13;
  const sa=$('cfgSolAct'),sh=$('cfgSolHold');
  sa.value=CFG.sol_act!=null?CFG.sol_act:255;updPwmPct(sa);
  sh.value=CFG.sol_hold!=null?CFG.sol_hold:80;updPwmPct(sh);
  $('cfgSolTime').value=CFG.sol_time!=null?CFG.sol_time:30;
  $('cfgSolInter').value=CFG.sol_inter!=null?CFG.sol_inter:0;
  // Servo valve settings
  $('cfgVlvClose').value=CFG.vlv_close!=null?CFG.vlv_close:0;
  $('cfgVlvOpen').value=CFG.vlv_open!=null?CFG.vlv_open:90;
  $('cfgVlvDir').value=(CFG.vlv_dir||0).toString();
  // Servo angle (traversiere only)
  const angleBlock=$('airBlockAngle');
  if(angleBlock){
    const isTrav=(CFG.embouchure==='trav');
    angleBlock.style.display=isTrav?'block':'none';
    if(isTrav){
      const ap=$('cfgAngPca');if(ap){ap.innerHTML='';for(let i=0;i<16;i++){const o=document.createElement('option');o.value=i;o.textContent='PCA '+i;ap.appendChild(o)}ap.value=CFG.ang_pca!=null?CFG.ang_pca:12}
      const ao=$('cfgAngOff');if(ao)ao.value=CFG.ang_off!=null?CFG.ang_off:90;
      const an=$('cfgAngMin');if(an)an.value=CFG.ang_min!=null?CFG.ang_min:60;
      const ax=$('cfgAngMax');if(ax)ax.value=CFG.ang_max!=null?CFG.ang_max:120;
      const aOn=!!CFG.angle_on;
      const tg=$('airBlockAngleToggle');
      if(tg){tg.classList.toggle('on',aOn);tg.setAttribute('aria-checked',aOn?'true':'false')}
      angleBlock.classList.toggle('disabled',!aOn);
      angleBlock.classList.toggle('active',aOn);
    }
  }
  // Show air checkbox
  $('cfgShowAir').checked=!!CFG.show_air;
  // Attach validation listeners + dirty tracking (once only)
  if(!window._airListenersAttached){
    window._airListenersAttached=true;
    $('cfgShowAir').addEventListener('change',function(){if(CFG)CFG.show_air=this.checked;applyAirTabVisibility();markDirty()});
    ['airFanMin','airFanMax','airFanIdlePct','airFanIdleTimeout','airHallLow','airHallHigh','airSensMin','airSensMax','cfgAirOff','cfgAirMin','cfgAirMax'].forEach(id=>{
      const el=$(id);if(el)el.addEventListener('input',()=>{validateAirConfig();updateHallBar();markDirty()})});
    document.querySelectorAll('#tab-air select,#tab-air input[type=number],#tab-air input[type=checkbox]').forEach(el=>{
      el.addEventListener('change',()=>markDirty())});
    document.querySelectorAll('#tab-air input[type=number]').forEach(el=>{
      el.addEventListener('blur',()=>{
        const v=parseFloat(el.value),mn=parseFloat(el.min),mx=parseFloat(el.max);
        if(!isNaN(v)&&!isNaN(mn)&&v<mn)el.value=mn;
        if(!isNaN(v)&&!isNaN(mx)&&v>mx)el.value=mx;
      })});
  }
  buildPumpRows();
  _prevAirMode=CFG.air_mode||0;
  setAirMode(CFG.air_mode||0);toggleValveParams(true);toggleSensorParams();toggleMotorType();toggleCascadeRow();
}
function buildAirUI(){
  fillAirSettings();
  // Set flow test slider: start at off angle (modes 2,3) or min angle
  updateFlowSliderRange();
  const ft=$('airFlowTest');if(ft&&CFG){
    const m=CFG.air_mode||0;const needsOff=(m===2||m===3); // modes without valve
    const startA=needsOff?(CFG.air_off!=null?CFG.air_off:0):(CFG.air_min!=null?CFG.air_min:0);
    ft.value=startA;$('airFlowTestVal').textContent=startA+'°'}
  // Redraw diagram with last known data on tab re-entry
  if(lastAirData)updateAirDiagram(lastAirData);
  drawMiniChart();
  // Scroll to top on tab entry
  const tab=$('tab-air');if(tab)tab.scrollTop=0;
}
let _kbdPipeExitRatio=0,_kbdPipeExitY=0,_kbdAirVBW=440,_kbdAirVBH=155;
function svgServoAngle(cx,cy){
  const ach=CFG.ang_pca!=null?CFG.ang_pca:(CFG.angle_ch!=null?CFG.angle_ch:12);
  const sw=26,sh=18;
  let s='<rect x="'+(cx-sw/2)+'" y="'+(cy-sh/2)+'" width="'+sw+'" height="'+sh+'" rx="4" fill="url(#agMetal)" stroke="#569" stroke-width="1.2"/>';
  s+='<path id="airAngleNeedle" d="M'+cx+','+cy+' L'+(cx+8)+','+(cy-6)+'" stroke="#f76" stroke-width="1.8" stroke-linecap="round"/>';
  s+='<circle cx="'+cx+'" cy="'+cy+'" r="2" fill="#f76"/>';
  s+='<text x="'+cx+'" y="'+(cy-sh/2-3)+'" text-anchor="middle" style="font-size:6px;fill:#9aa">Servo Angle</text>';
  s+='<text x="'+(cx+sw/2+2)+'" y="'+(cy+3)+'" style="font-size:5px;fill:#569">CH'+ach+'</text>';
  return s;
}
function buildAirSvg(svgId,full){
  const svg=$(svgId);if(!svg||!CFG)return;
  const isKbd=(svgId==='kbdAirSvg');
  const m=CFG.air_mode||0;
  const hasPump=m>=4,hasFan=m===3,hasValve=(m===0||m===1||m>=4),hasRes=(m===5);
  const np=(hasPump&&CFG.num_pumps>1)?CFG.num_pumps:1;
  const st=CFG.sens_type||0;
  const resFormat=(CFG.res_format||'balloon');
  const isServoValve=(CFG.valve_type===1);
  const isTrav=(CFG.embouchure==='trav');
  const w=full?(isTrav&&CFG.angle_on?530:480):440;
  const h=full?240:155;
  svg.setAttribute('viewBox','0 0 '+w+' '+h);
  let s='<defs>'+
    '<linearGradient id="agMetal" x1="0" y1="0" x2="0" y2="1"><stop offset="0%" stop-color="#8899aa"/><stop offset="100%" stop-color="#556677"/></linearGradient>'+
    '<linearGradient id="agBalloon" x1="0" y1="0" x2="0" y2="1"><stop offset="0%" stop-color="#e07050"/><stop offset="100%" stop-color="#a03020"/></linearGradient>'+
    '<linearGradient id="agPiston" x1="0" y1="0" x2="0" y2="1"><stop offset="0%" stop-color="#99aacc"/><stop offset="100%" stop-color="#667799"/></linearGradient>'+
    '<linearGradient id="agCylinder" x1="0" y1="0" x2="1" y2="0"><stop offset="0%" stop-color="#444"/><stop offset="20%" stop-color="#555"/><stop offset="80%" stop-color="#555"/><stop offset="100%" stop-color="#444"/></linearGradient>'+
    '<linearGradient id="agFanBody" x1="0" y1="0" x2="0" y2="1"><stop offset="0%" stop-color="#667"/><stop offset="100%" stop-color="#445"/></linearGradient>'+
    '</defs>';

  // --- MODE 3: Fan (radial/centrifugal) + Servo Flow ---
  if(hasFan){
    const fanCx=120,fanCy=h/2,fanR=44;
    const em=CFG.embouchure||'bec';
    const isTrav=(em==='trav');
    // === Radial fan chassis (square housing) ===
    const chPad=12;
    const chL=fanCx-fanR-chPad,chT=fanCy-fanR-chPad;
    const chW=(fanR+chPad)*2,chH=(fanR+chPad)*2;
    const chR=chL+chW,chB=chT+chH;
    // Chassis body
    s+='<rect x="'+chL+'" y="'+chT+'" width="'+chW+'" height="'+chH+'" rx="6" fill="url(#agFanBody)" stroke="#889" stroke-width="2.5"/>';
    // Mounting bolts at corners
    for(let b=0;b<4;b++){const bx=b<2?chL+8:chR-8,by=b%2===0?chT+8:chB-8;
      s+='<circle cx="'+bx+'" cy="'+by+'" r="3.5" fill="#334" stroke="#667" stroke-width="1"/>';
      s+='<circle cx="'+bx+'" cy="'+by+'" r="1.2" fill="#889"/>';}
    // Circular intake (visible through chassis)
    s+='<circle cx="'+fanCx+'" cy="'+fanCy+'" r="'+(fanR+2)+'" fill="#1a1a2e" stroke="#556" stroke-width="1.5"/>';
    // Internal volute scroll (spiral guide from fan to outlet at top-right)
    s+='<path d="M'+(fanCx+fanR)+','+fanCy+
      ' C'+(fanCx+fanR+6)+','+(fanCy+fanR*0.7)+' '+(fanCx-fanR*0.4)+','+(fanCy+fanR+5)+' '+(fanCx-fanR)+','+fanCy+
      ' C'+(fanCx-fanR)+','+(fanCy-fanR*0.8)+' '+(fanCx+fanR*0.3)+','+(fanCy-fanR-5)+' '+(chR-4)+','+(chT+8)+
      '" fill="none" stroke="#555" stroke-width="1.5" stroke-dasharray="3,4" opacity=".5"/>';
    // Fan blades (curved, animated)
    s+='<g id="airFanBlades" style="transform-origin:'+fanCx+'px '+fanCy+'px">';
    for(let a=0;a<7;a++){
      const ra=a*(360/7)*Math.PI/180;
      const x1=fanCx+10*Math.cos(ra),y1=fanCy+10*Math.sin(ra);
      const x2=fanCx+(fanR-6)*Math.cos(ra),y2=fanCy+(fanR-6)*Math.sin(ra);
      const cpx=fanCx+(fanR-6)*Math.cos(ra+0.35),cpy=fanCy+(fanR-6)*Math.sin(ra+0.35);
      s+='<path d="M'+x1+','+y1+' Q'+cpx+','+cpy+' '+x2+','+y2+'" stroke="#8af" stroke-width="4" fill="none" stroke-linecap="round" opacity=".8"/>';
    }
    s+='</g>';
    // Center hub
    s+='<circle cx="'+fanCx+'" cy="'+fanCy+'" r="8" fill="#556" stroke="#889" stroke-width="1"/>';
    s+='<circle cx="'+fanCx+'" cy="'+fanCy+'" r="3" fill="#334"/>';
    // === Outlet at top-right of chassis ===
    const outH=22;
    const pivotX=chR,pivotY=chT+outH/2;
    // Outlet flange (outer border + dark opening)
    s+='<rect x="'+(chR-2)+'" y="'+(chT-2)+'" width="8" height="'+(outH+4)+'" rx="2" fill="#556" stroke="#889" stroke-width="1"/>';
    s+='<rect x="'+chR+'" y="'+(chT+2)+'" width="3" height="'+(outH-4)+'" rx="1" fill="#222"/>';
    // === Static blue pipe: outlet to flute (reference path) ===
    const ductLen=80;
    const flX=pivotX+ductLen+16;
    const fluteW=70,fluteH=24;
    if(isKbd){
      // Kbd mode: horizontal pipe + arrow, flute is in separate SVG below
      s+='<line x1="'+(pivotX+4)+'" y1="'+pivotY+'" x2="'+flX+'" y2="'+pivotY+'" stroke="#7799bb" stroke-width="3"/>';
      s+='<line class="airFlowAnim" x1="'+(pivotX+ductLen)+'" y1="'+pivotY+'" x2="'+flX+'" y2="'+pivotY+'"/>';
      s+='<polygon points="'+(flX-4)+','+(pivotY-3)+' '+(flX+1)+','+pivotY+' '+(flX-4)+','+(pivotY+3)+'" fill="#7799bb"/>';
      _kbdPipeExitRatio=flX/w;_kbdPipeExitY=pivotY;
    }else if(isTrav){
      // Full mode + traversiere: pipe horizontal then curves down above blow hole
      const hasAng=!!CFG.angle_on;
      const angW=30,angH=20,angGap=6;
      const pipeStartX=pivotX+ductLen+4;
      const angX=hasAng?pipeStartX+angGap:0;
      const pipeAfterAng=hasAng?angX+angW+angGap:pipeStartX;
      const blowX=pipeAfterAng+Math.round(fluteW*0.35);
      const fluteY=pivotY+16;
      // Pipe from servo flow to angle servo (or directly to blow hole)
      if(hasAng){
        s+='<line x1="'+pipeStartX+'" y1="'+pivotY+'" x2="'+angX+'" y2="'+pivotY+'" stroke="#7799bb" stroke-width="3"/>';
        s+='<line class="airFlowAnim" x1="'+pipeStartX+'" y1="'+pivotY+'" x2="'+angX+'" y2="'+pivotY+'"/>';
        s+=svgServoAngle(angX+angW/2,pivotY);
        s+='<line x1="'+(angX+angW)+'" y1="'+pivotY+'" x2="'+blowX+'" y2="'+pivotY+'" stroke="#7799bb" stroke-width="3"/>';
        s+='<line class="airFlowAnim" x1="'+(angX+angW)+'" y1="'+pivotY+'" x2="'+blowX+'" y2="'+pivotY+'"/>';
      }else{
        s+='<line x1="'+pipeStartX+'" y1="'+pivotY+'" x2="'+blowX+'" y2="'+pivotY+'" stroke="#7799bb" stroke-width="3"/>';
        s+='<line class="airFlowAnim" x1="'+pipeStartX+'" y1="'+pivotY+'" x2="'+blowX+'" y2="'+pivotY+'"/>';
      }
      // Vertical pipe down to blow hole
      s+='<path d="M'+blowX+','+pivotY+' L'+blowX+','+(fluteY-1)+'" stroke="#7799bb" stroke-width="3" fill="none"/>';
      s+='<line class="airFlowAnim" x1="'+blowX+'" y1="'+(pivotY+4)+'" x2="'+blowX+'" y2="'+(fluteY-2)+'"/>';
      s+='<polygon points="'+(blowX-3)+','+(fluteY-5)+' '+blowX+','+fluteY+' '+(blowX+3)+','+(fluteY-5)+'" fill="#7799bb"/>';
      const fluteStartX=blowX-Math.round(fluteW*0.35);
      s+='<rect x="'+fluteStartX+'" y="'+fluteY+'" width="'+fluteW+'" height="'+fluteH+'" rx="10" fill="'+(CFG.color||'#D4B044')+'" stroke="#a89030" stroke-width="1" opacity=".8"/>';
      s+='<ellipse cx="'+blowX+'" cy="'+(fluteY+3)+'" rx="5" ry="2.5" fill="#5C4A0A" opacity=".6"/>';
      s+='<text x="'+(fluteStartX+fluteW/2)+'" y="'+(fluteY+fluteH/2+4)+'" text-anchor="middle" style="font-size:9px;fill:#333;font-weight:bold">Flute</text>';
    }else{
      // Full mode + bec/naf/oca: horizontal pipe to flute left side
      s+='<line x1="'+(pivotX+4)+'" y1="'+pivotY+'" x2="'+flX+'" y2="'+pivotY+'" stroke="#7799bb" stroke-width="3"/>';
      s+='<line class="airFlowAnim" x1="'+(pivotX+ductLen)+'" y1="'+pivotY+'" x2="'+flX+'" y2="'+pivotY+'"/>';
      s+='<polygon points="'+(flX-4)+','+(pivotY-3)+' '+(flX+1)+','+pivotY+' '+(flX-4)+','+(pivotY+3)+'" fill="#7799bb"/>';
      s+='<rect x="'+(flX+4)+'" y="'+(pivotY-fluteH/2)+'" width="'+fluteW+'" height="'+fluteH+'" rx="10" fill="'+(CFG.color||'#D4B044')+'" stroke="#a89030" stroke-width="1" opacity=".8"/>';
      s+='<text x="'+(flX+4+fluteW/2)+'" y="'+(pivotY+4)+'" text-anchor="middle" style="font-size:9px;fill:#333;font-weight:bold">Flute</text>';
    }
    // === Air duct (servo flow) - pivots at outlet, ON TOP of blue pipe ===
    const ach=CFG.air_pca||10;
    s+='<g id="airFanDuct" style="transform-origin:'+pivotX+'px '+pivotY+'px">';
    s+='<rect x="'+pivotX+'" y="'+(pivotY-9)+'" width="'+ductLen+'" height="18" rx="3" fill="url(#agMetal)" stroke="#556" stroke-width="1" opacity=".9"/>';
    s+='<line class="airFlowAnim" x1="'+(pivotX+4)+'" y1="'+pivotY+'" x2="'+(pivotX+ductLen-4)+'" y2="'+pivotY+'"/>';
    s+='<rect x="'+(pivotX+ductLen-2)+'" y="'+(pivotY-7)+'" width="4" height="14" rx="1" fill="#222" stroke="#556" stroke-width=".5"/>';
    s+='</g>';
    // Labels (on top of everything)
    s+='<text x="'+fanCx+'" y="'+(chB+14)+'" text-anchor="middle" style="font-size:8px;fill:#9aa">Ventilateur</text>';
    s+='<text x="'+fanCx+'" y="'+(chT-6)+'" text-anchor="middle" style="font-size:6px;fill:#667">Aspiration</text>';
    s+='<text x="'+(pivotX+ductLen/2)+'" y="'+(pivotY-14)+'" text-anchor="middle" style="font-size:7px;fill:#9aa">Servo Flow</text>';
    s+='<text x="'+(pivotX+ductLen)+'" y="'+(pivotY+22)+'" text-anchor="middle" style="font-size:5px;fill:#569">CH'+ach+'</text>';
    var fanW=w;
    if(isKbd&&_kbdPipeExitRatio>0){fanW=Math.ceil(_kbdPipeExitRatio*w)+12;_kbdAirVBW=fanW;_kbdAirVBH=h}
    svg.setAttribute('viewBox','0 0 '+fanW+' '+h);
    svg.innerHTML=s;
    return;
  }

  // --- MODES 0,2,4,5: T-connector layout ---
  // Layout:
  //        [Reservoir/Balloon]     [Servo Flow]
  //              |                      |
  //  [Pompe] -- T --------[Valve] --> [Flute]
  //      or [Air] if no pump
  //
  // Positions:
  const colX=80; // pump column (bottom-left)
  const teeY=full?h/2:h/2; // T-connector vertical center
  const teeX=colX; // T-connector X = same as pump column

  // ===== PUMP or AIR SOURCE (bottom-left) =====
  if(hasPump){
    const pumpY=h-32;
    for(let i=0;i<np;i++){
      const px=colX+(i*50-(np-1)*25);
      s+='<rect x="'+(px-22)+'" y="'+(pumpY-14)+'" width="44" height="28" rx="6" fill="url(#agMetal)" stroke="#334" stroke-width="1.5"/>';
      s+='<circle id="airPumpIcon" cx="'+px+'" cy="'+pumpY+'" r="9" fill="none" stroke="#dde" stroke-width="1.5"/>';
      s+='<line x1="'+px+'" y1="'+(pumpY-6)+'" x2="'+px+'" y2="'+(pumpY+6)+'" stroke="#dde" stroke-width="1.5"/>';
      s+='<line x1="'+(px-6)+'" y1="'+pumpY+'" x2="'+(px+6)+'" y2="'+pumpY+'" stroke="#dde" stroke-width="1.5"/>';
      if(np>1)s+='<text x="'+px+'" y="'+(pumpY+22)+'" text-anchor="middle" style="font-size:7px;fill:#9aa">P'+(i+1)+'</text>';
    }
    if(np===1)s+='<text x="'+colX+'" y="'+(pumpY+24)+'" text-anchor="middle" style="font-size:8px;fill:#9aa">'+(CFG.motor_type===1?'On/Off':'Pompe')+'</text>';
    // Pipe UP from pump to T-connector
    s+='<line x1="'+colX+'" y1="'+(pumpY-14)+'" x2="'+colX+'" y2="'+(teeY+6)+'" stroke="#7799bb" stroke-width="3"/>';
    s+='<line class="airFlowAnim" x1="'+colX+'" y1="'+(pumpY-14)+'" x2="'+colX+'" y2="'+(teeY+6)+'"/>';
    s+='<polygon points="'+(colX-3)+','+(teeY+10)+' '+colX+','+(teeY+4)+' '+(colX+3)+','+(teeY+10)+'" fill="#7799bb"/>';
  }else{
    // Air source label (modes 0, 2)
    const airY=h-32;
    s+='<text x="'+colX+'" y="'+(airY+4)+'" text-anchor="middle" style="font-size:9px;fill:#9aa">Air</text>';
    s+='<line x1="'+colX+'" y1="'+(airY-10)+'" x2="'+colX+'" y2="'+(teeY+6)+'" stroke="#7799bb" stroke-width="3"/>';
    s+='<line class="airFlowAnim" x1="'+colX+'" y1="'+(airY-10)+'" x2="'+colX+'" y2="'+(teeY+6)+'"/>';
    s+='<polygon points="'+(colX-3)+','+(teeY+10)+' '+colX+','+(teeY+4)+' '+(colX+3)+','+(teeY+10)+'" fill="#7799bb"/>';
  }

  // ===== T-CONNECTOR =====
  const tR=8;
  s+='<circle cx="'+teeX+'" cy="'+teeY+'" r="'+tR+'" fill="#556" stroke="#778" stroke-width="1.5"/>';
  s+='<circle cx="'+teeX+'" cy="'+teeY+'" r="'+(tR-3)+'" fill="#334"/>';

  // ===== RESERVOIR (above T-connector) =====
  if(hasRes){
    const resY=teeY-50;
    const rW=50,rH=30;
    // Pipe UP from T to reservoir
    s+='<line x1="'+teeX+'" y1="'+(teeY-tR)+'" x2="'+teeX+'" y2="'+(resY+rH/2)+'" stroke="#7799bb" stroke-width="3"/>';
    s+='<line class="airFlowAnim" x1="'+teeX+'" y1="'+(teeY-tR)+'" x2="'+teeX+'" y2="'+(resY+rH/2)+'"/>';
    // Reservoir body
    s+='<rect x="'+(teeX-rW/2)+'" y="'+(resY-rH/2)+'" width="'+rW+'" height="'+rH+'" rx="6" fill="#334" stroke="#556" stroke-width="1.5"/>';
    s+='<text x="'+teeX+'" y="'+(resY+4)+'" text-anchor="middle" style="font-size:8px;fill:#dde;font-weight:bold">Reservoir</text>';
    // Sensor indicator
    if(st<=1){
      s+='<rect x="'+(teeX+rW/2+4)+'" y="'+(resY-6)+'" width="8" height="12" rx="2" fill="#334" stroke="#5af" stroke-width=".8"/>';
      s+='<text x="'+(teeX+rW/2+14)+'" y="'+(resY+4)+'" style="font-size:5px;fill:#5af">ToF</text>';
    }else if(st===2){
      s+='<text x="'+(teeX+rW/2+6)+'" y="'+(resY+4)+'" style="font-size:6px;fill:#fa0">Hall</text>';
    }else{
      s+='<circle cx="'+(teeX+rW/2+8)+'" cy="'+resY+'" r="3" fill="#4e4" id="airEndstopLed"/>';
      s+='<text x="'+(teeX+rW/2+8)+'" y="'+(resY+12)+'" text-anchor="middle" style="font-size:5px;fill:#9aa">'+(st===3?'Meca':'Opt')+'</text>';
    }
    // Bellows/balloon ABOVE reservoir
    const vizTop=resY-rH/2-5;
    const vizH=40;
    if(resFormat==='bellows'){
      s+='<g id="airBellowsGroup">';
      const bW=40,folds=5;
      const bellowsBottom=vizTop,bellowsTop=bellowsBottom-vizH;
      for(let f=0;f<folds;f++){
        const fy=bellowsTop+(vizH/folds)*f,fh=vizH/folds;
        s+='<path class="airBellowsFold" d="M'+(teeX-bW/2)+','+fy+' L'+(teeX-bW/2-6)+','+(fy+fh/2)+' L'+(teeX-bW/2)+','+(fy+fh)+'" stroke="#a87040" stroke-width="1.5" fill="none"/>';
        s+='<path class="airBellowsFold" d="M'+(teeX+bW/2)+','+fy+' L'+(teeX+bW/2+6)+','+(fy+fh/2)+' L'+(teeX+bW/2)+','+(fy+fh)+'" stroke="#a87040" stroke-width="1.5" fill="none"/>';
      }
      s+='<rect id="airBellowsTop" x="'+(teeX-bW/2-4)+'" y="'+bellowsTop+'" width="'+(bW+8)+'" height="6" rx="2" fill="#886644" stroke="#664422" stroke-width="1"/>';
      s+='<rect x="'+(teeX-bW/2-4)+'" y="'+(bellowsBottom-6)+'" width="'+(bW+8)+'" height="6" rx="2" fill="#886644" stroke="#664422" stroke-width="1"/>';
      s+='<rect id="airBellowsFill" x="'+(teeX-bW/2)+'" y="'+bellowsTop+'" width="'+bW+'" height="'+vizH+'" rx="0" fill="url(#agBalloon)" opacity=".25"/>';
      s+='<text x="'+teeX+'" y="'+(bellowsTop+vizH/2+4)+'" text-anchor="middle" style="font-size:10px;fill:#fff;font-weight:bold" id="airBalloonPct">--%</text>';
      s+='</g>';
    }else{
      s+='<g id="airBalloonGroup">';
      const balloonCy=vizTop-vizH/2+5;
      s+='<ellipse id="airBalloon" cx="'+teeX+'" cy="'+balloonCy+'" rx="28" ry="'+(vizH/2)+'" fill="url(#agBalloon)" stroke="#802010" stroke-width="1.5" opacity=".85"/>';
      s+='<path d="M'+(teeX-4)+','+(vizTop-2)+' L'+teeX+','+(vizTop+3)+' L'+(teeX+4)+','+(vizTop-2)+'" fill="#a03020" stroke="#802010" stroke-width="1"/>';
      s+='<text x="'+teeX+'" y="'+(balloonCy+4)+'" text-anchor="middle" style="font-size:10px;fill:#fff;font-weight:bold" id="airBalloonPct">--%</text>';
      s+='</g>';
    }
  }

  // ===== Pipe RIGHT from T-connector =====
  let rightX=teeX+tR; // start of rightward pipe
  const targetRight=hasValve?teeX+65:teeX+60; // where next element starts
  s+='<line x1="'+rightX+'" y1="'+teeY+'" x2="'+targetRight+'" y2="'+teeY+'" stroke="#7799bb" stroke-width="3"/>';
  s+='<line class="airFlowAnim" x1="'+rightX+'" y1="'+teeY+'" x2="'+targetRight+'" y2="'+teeY+'"/>';
  s+='<polygon points="'+(targetRight-4)+','+(teeY-3)+' '+(targetRight+1)+','+teeY+' '+(targetRight-4)+','+(teeY+3)+'" fill="#7799bb"/>';

  // ===== VALVE + SERVO FLOW (stacked vertically) =====
  if(hasValve){
    const vx=teeX+80,vy=teeY;
    const cylW=30,cylH=36;
    const cylStroke=isServoValve?'#569':'#556';
    // Valve cylinder
    s+='<rect id="airValveRect" x="'+(vx-cylW/2)+'" y="'+(vy-cylH/2)+'" width="'+cylW+'" height="'+cylH+'" rx="3" fill="url(#agCylinder)" stroke="'+cylStroke+'" stroke-width="1.5"/>';
    s+='<rect x="'+(vx-cylW/2+4)+'" y="'+(vy-cylH/2+3)+'" width="'+(cylW-8)+'" height="'+(cylH-6)+'" rx="1" fill="#1a1a2e" opacity=".6"/>';
    s+='<g id="airPistonGroup">';
    const pistonFill=isServoValve?'url(#agMetal)':'url(#agPiston)';
    s+='<rect id="airPiston" x="'+(vx-cylW/2+5)+'" y="'+(vy-cylH/2+4)+'" width="'+(cylW-10)+'" height="12" rx="1.5" fill="'+pistonFill+'" stroke="'+(isServoValve?'#6688aa':'#8899bb')+'" stroke-width="1"/>';
    s+='<line x1="'+vx+'" y1="'+(vy-cylH/2-8)+'" x2="'+vx+'" y2="'+(vy-cylH/2+10)+'" stroke="'+(isServoValve?'#6688aa':'#8899bb')+'" stroke-width="2" stroke-linecap="round"/>';
    s+='</g>';
    // Valve LED indicator
    s+='<rect id="airValveInd" x="'+(vx-cylW/2-3)+'" y="'+(vy+cylH/2-8)+'" width="6" height="6" rx="1" fill="#e44"/>';
    const vl=isServoValve?'Servo':'Solenoide';
    s+='<text x="'+vx+'" y="'+(vy+cylH/2+12)+'" text-anchor="middle" style="font-size:7px;fill:#9aa">'+vl+'</text>';
    if(isServoValve){const vch=CFG.valve_ch||11;s+='<text x="'+(vx+cylW/2+2)+'" y="'+(vy-cylH/2+8)+'" style="font-size:5px;fill:#569">CH'+vch+'</text>'}
    else{const sp=CFG.sol_pin||13;s+='<text x="'+(vx+cylW/2+2)+'" y="'+(vy-cylH/2+8)+'" style="font-size:5px;fill:#e9a645">G'+sp+'</text>'}
    // === Servo Flow ABOVE valve ===
    const ach=CFG.air_pca||10;
    const svW=36,svH=26;
    const svCy=vy-cylH/2-svH/2-12;
    s+='<rect x="'+(vx-svW/2)+'" y="'+(svCy-svH/2)+'" width="'+svW+'" height="'+svH+'" rx="5" fill="url(#agMetal)" stroke="#334" stroke-width="1.2"/>';
    s+='<path id="airServoNeedle" d="M'+vx+','+svCy+' L'+vx+','+(svCy-10)+'" stroke="#e94" stroke-width="2" stroke-linecap="round"/>';
    s+='<circle cx="'+vx+'" cy="'+svCy+'" r="2.5" fill="#e94"/>';
    s+='<text x="'+vx+'" y="'+(svCy-svH/2-4)+'" text-anchor="middle" style="font-size:7px;fill:#9aa">Servo Flow</text>';
    s+='<text x="'+(vx+svW/2+2)+'" y="'+(svCy-4)+'" style="font-size:5px;fill:#569">CH'+ach+'</text>';
    // Flow animation valve -> servo flow (vertical)
    s+='<line class="airFlowAnim" x1="'+vx+'" y1="'+(vy-cylH/2)+'" x2="'+vx+'" y2="'+(svCy+svH/2)+'"/>';
    s+='<polygon points="'+(vx-3)+','+(svCy+svH/2+4)+' '+vx+','+(svCy+svH/2-1)+' '+(vx+3)+','+(svCy+svH/2+4)+'" fill="#7799bb"/>';
    // Pipe from servo flow RIGHT to flute (traversiere-aware)
    const flX=vx+svW/2+16;
    const _isTrav=(CFG.embouchure==='trav');
    if(isKbd){
      // Kbd mode: horizontal pipe + arrow, flute is separate SVG below
      s+='<line x1="'+(vx+svW/2)+'" y1="'+svCy+'" x2="'+flX+'" y2="'+svCy+'" stroke="#7799bb" stroke-width="3"/>';
      s+='<line class="airFlowAnim" x1="'+(vx+svW/2)+'" y1="'+svCy+'" x2="'+flX+'" y2="'+svCy+'"/>';
      s+='<polygon points="'+(flX-4)+','+(svCy-3)+' '+(flX+1)+','+svCy+' '+(flX-4)+','+(svCy+3)+'" fill="#7799bb"/>';
      _kbdPipeExitRatio=(flX+1)/w;_kbdPipeExitY=svCy;
    }else if(_isTrav){
      const _hasAng=!!CFG.angle_on;
      const _angW=30,_angGap=6;
      const _pipeStart=vx+svW/2;
      const _angX=_hasAng?_pipeStart+_angGap+4:0;
      const _afterAng=_hasAng?_angX+_angW+_angGap:_pipeStart+4;
      const _bx=_afterAng+Math.round(70*0.35),_fy=svCy+14;
      // Horizontal pipe with optional angle servo block
      if(_hasAng){
        s+='<line x1="'+_pipeStart+'" y1="'+svCy+'" x2="'+_angX+'" y2="'+svCy+'" stroke="#7799bb" stroke-width="3"/>';
        s+='<line class="airFlowAnim" x1="'+_pipeStart+'" y1="'+svCy+'" x2="'+_angX+'" y2="'+svCy+'"/>';
        s+=svgServoAngle(_angX+_angW/2,svCy);
        s+='<line x1="'+(_angX+_angW)+'" y1="'+svCy+'" x2="'+_bx+'" y2="'+svCy+'" stroke="#7799bb" stroke-width="3"/>';
        s+='<line class="airFlowAnim" x1="'+(_angX+_angW)+'" y1="'+svCy+'" x2="'+_bx+'" y2="'+svCy+'"/>';
      }else{
        s+='<line x1="'+_pipeStart+'" y1="'+svCy+'" x2="'+_bx+'" y2="'+svCy+'" stroke="#7799bb" stroke-width="3"/>';
        s+='<line class="airFlowAnim" x1="'+_pipeStart+'" y1="'+svCy+'" x2="'+_bx+'" y2="'+svCy+'"/>';
      }
      // Vertical pipe down to blow hole
      s+='<path d="M'+_bx+','+svCy+' L'+_bx+','+(_fy-1)+'" stroke="#7799bb" stroke-width="3" fill="none"/>';
      s+='<line class="airFlowAnim" x1="'+_bx+'" y1="'+(svCy+4)+'" x2="'+_bx+'" y2="'+(_fy-2)+'"/>';
      s+='<polygon points="'+(_bx-3)+','+(_fy-5)+' '+_bx+','+_fy+' '+(_bx+3)+','+(_fy-5)+'" fill="#7799bb"/>';
      const _fluteStartX=_bx-Math.round(70*0.35);
      s+='<rect x="'+_fluteStartX+'" y="'+_fy+'" width="70" height="24" rx="10" fill="'+(CFG.color||'#D4B044')+'" stroke="#a89030" stroke-width="1" opacity=".8"/>';
      s+='<ellipse cx="'+_bx+'" cy="'+(_fy+3)+'" rx="5" ry="2.5" fill="#5C4A0A" opacity=".6"/>';
      s+='<text x="'+(_fluteStartX+35)+'" y="'+(_fy+16)+'" text-anchor="middle" style="font-size:9px;fill:#333;font-weight:bold">Flute</text>';
    }else{
      s+='<line x1="'+(vx+svW/2)+'" y1="'+svCy+'" x2="'+flX+'" y2="'+svCy+'" stroke="#7799bb" stroke-width="3"/>';
      s+='<line class="airFlowAnim" x1="'+(vx+svW/2)+'" y1="'+svCy+'" x2="'+flX+'" y2="'+svCy+'"/>';
      s+='<polygon points="'+(flX-4)+','+(svCy-3)+' '+(flX+1)+','+svCy+' '+(flX-4)+','+(svCy+3)+'" fill="#7799bb"/>';
      s+='<rect x="'+(flX+4)+'" y="'+(svCy-12)+'" width="70" height="24" rx="10" fill="'+(CFG.color||'#D4B044')+'" stroke="#a89030" stroke-width="1" opacity=".8"/>';
      s+='<text x="'+(flX+39)+'" y="'+(svCy+4)+'" text-anchor="middle" style="font-size:9px;fill:#333;font-weight:bold">Flute</text>';
    }
  }else{
    // No valve (mode 2): T-connector goes directly to servo flow
    const svX=targetRight+8;
    const ach=CFG.air_pca||10;
    s+='<rect x="'+svX+'" y="'+(teeY-14)+'" width="36" height="28" rx="5" fill="url(#agMetal)" stroke="#334" stroke-width="1.2"/>';
    s+='<path id="airServoNeedle" d="M'+(svX+18)+','+teeY+' L'+(svX+18)+','+(teeY-10)+'" stroke="#e94" stroke-width="2" stroke-linecap="round"/>';
    s+='<circle cx="'+(svX+18)+'" cy="'+teeY+'" r="2.5" fill="#e94"/>';
    s+='<text x="'+(svX+18)+'" y="'+(teeY-18)+'" text-anchor="middle" style="font-size:7px;fill:#9aa">Servo Flow</text>';
    s+='<text x="'+(svX+36)+'" y="'+(teeY-6)+'" style="font-size:5px;fill:#569">CH'+ach+'</text>';
    const flX=svX+46;
    const _isTrav2=(CFG.embouchure==='trav');
    if(isKbd){
      // Kbd mode: horizontal pipe + arrow, flute is separate SVG below
      s+='<line x1="'+(svX+36)+'" y1="'+teeY+'" x2="'+flX+'" y2="'+teeY+'" stroke="#7799bb" stroke-width="3"/>';
      s+='<line class="airFlowAnim" x1="'+(svX+36)+'" y1="'+teeY+'" x2="'+flX+'" y2="'+teeY+'"/>';
      s+='<polygon points="'+(flX-4)+','+(teeY-3)+' '+(flX+1)+','+teeY+' '+(flX-4)+','+(teeY+3)+'" fill="#7799bb"/>';
      _kbdPipeExitRatio=(flX+1)/w;_kbdPipeExitY=teeY;
    }else if(_isTrav2){
      const _hasAng2=!!CFG.angle_on;
      const _angW2=30,_angGap2=6;
      const _pipeStart2=svX+36;
      const _angX2=_hasAng2?_pipeStart2+_angGap2:0;
      const _afterAng2=_hasAng2?_angX2+_angW2+_angGap2:_pipeStart2+4;
      const _bx2=_afterAng2+Math.round(70*0.35),_fy2=teeY+14;
      if(_hasAng2){
        s+='<line x1="'+_pipeStart2+'" y1="'+teeY+'" x2="'+_angX2+'" y2="'+teeY+'" stroke="#7799bb" stroke-width="3"/>';
        s+='<line class="airFlowAnim" x1="'+_pipeStart2+'" y1="'+teeY+'" x2="'+_angX2+'" y2="'+teeY+'"/>';
        s+=svgServoAngle(_angX2+_angW2/2,teeY);
        s+='<line x1="'+(_angX2+_angW2)+'" y1="'+teeY+'" x2="'+_bx2+'" y2="'+teeY+'" stroke="#7799bb" stroke-width="3"/>';
        s+='<line class="airFlowAnim" x1="'+(_angX2+_angW2)+'" y1="'+teeY+'" x2="'+_bx2+'" y2="'+teeY+'"/>';
      }else{
        s+='<line x1="'+_pipeStart2+'" y1="'+teeY+'" x2="'+_bx2+'" y2="'+teeY+'" stroke="#7799bb" stroke-width="3"/>';
        s+='<line class="airFlowAnim" x1="'+_pipeStart2+'" y1="'+teeY+'" x2="'+_bx2+'" y2="'+teeY+'"/>';
      }
      s+='<path d="M'+_bx2+','+teeY+' L'+_bx2+','+(_fy2-1)+'" stroke="#7799bb" stroke-width="3" fill="none"/>';
      s+='<line class="airFlowAnim" x1="'+_bx2+'" y1="'+(teeY+4)+'" x2="'+_bx2+'" y2="'+(_fy2-2)+'"/>';
      s+='<polygon points="'+(_bx2-3)+','+(_fy2-5)+' '+_bx2+','+_fy2+' '+(_bx2+3)+','+(_fy2-5)+'" fill="#7799bb"/>';
      const _fluteStartX2=_bx2-Math.round(70*0.35);
      s+='<rect x="'+_fluteStartX2+'" y="'+_fy2+'" width="70" height="24" rx="10" fill="'+(CFG.color||'#D4B044')+'" stroke="#a89030" stroke-width="1" opacity=".8"/>';
      s+='<ellipse cx="'+_bx2+'" cy="'+(_fy2+3)+'" rx="5" ry="2.5" fill="#5C4A0A" opacity=".6"/>';
      s+='<text x="'+(_fluteStartX2+35)+'" y="'+(_fy2+16)+'" text-anchor="middle" style="font-size:9px;fill:#333;font-weight:bold">Flute</text>';
    }else{
      s+='<line x1="'+(svX+36)+'" y1="'+teeY+'" x2="'+flX+'" y2="'+teeY+'" stroke="#7799bb" stroke-width="3"/>';
      s+='<line class="airFlowAnim" x1="'+(svX+36)+'" y1="'+teeY+'" x2="'+flX+'" y2="'+teeY+'"/>';
      s+='<polygon points="'+(flX-4)+','+(teeY-3)+' '+(flX+1)+','+teeY+' '+(flX-4)+','+(teeY+3)+'" fill="#7799bb"/>';
      s+='<rect x="'+(flX+4)+'" y="'+(teeY-12)+'" width="70" height="24" rx="10" fill="'+(CFG.color||'#D4B044')+'" stroke="#a89030" stroke-width="1" opacity=".8"/>';
      s+='<text x="'+(flX+39)+'" y="'+(teeY+4)+'" text-anchor="middle" style="font-size:9px;fill:#333;font-weight:bold">Flute</text>';
    }
  }
  var finalW=w;
  if(isKbd&&_kbdPipeExitRatio>0){finalW=Math.ceil(_kbdPipeExitRatio*w)+12;_kbdAirVBW=finalW;_kbdAirVBH=h}
  svg.setAttribute('viewBox','0 0 '+finalW+' '+h);
  svg.innerHTML=s;
}
function updateAirDiagram(d){
  // Update valve piston position (piston descends when valve open / note on)
  document.querySelectorAll('[id=airPistonGroup]').forEach(pg=>{
    const piston=pg.querySelector('[id=airPiston]');
    if(!piston)return;
    if(d.valve_open){
      pg.style.transition='transform 0.15s ease';
      pg.style.transform='translateY(14px)';
    }else{
      pg.style.transition='transform 0.2s ease';
      pg.style.transform='translateY(0)';
    }
  });
  // Update valve indicator color
  document.querySelectorAll('[id=airValveInd]').forEach(vi=>vi.setAttribute('fill',d.valve_open?'#4e4':'#e44'));
  // Update balloon/bellows based on res_pct
  const pct=d.res_pct!=null?d.res_pct:0;
  // Balloon: scale based on fill level
  document.querySelectorAll('[id=airBalloon]').forEach(bl=>{
    const minScale=0.4,maxScale=1.0;
    const sc=minScale+(maxScale-minScale)*(pct/100);
    bl.setAttribute('ry',Math.round((45/2)*sc));
    bl.setAttribute('rx',Math.round(24*sc));
  });
  // Bellows: adjust top plate position and fill height
  document.querySelectorAll('[id=airBellowsTop]').forEach(bt=>{
    const maxShift=35;
    const shift=maxShift*(1-pct/100);
    bt.style.transition='transform 0.3s ease';
    bt.style.transform='translateY('+shift+'px)';
  });
  document.querySelectorAll('[id=airBellowsFill]').forEach(bf=>{
    const fullH=45;
    const fillH=Math.max(4,fullH*(pct/100));
    // Store original Y on first call to prevent drift on repeated updates
    if(!bf.dataset.origY)bf.dataset.origY=bf.getAttribute('y');
    const origY=parseFloat(bf.dataset.origY);
    bf.setAttribute('height',fillH);
    bf.setAttribute('y',origY+fullH-fillH);
  });
  // Update percentage text
  document.querySelectorAll('[id=airBalloonPct]').forEach(bp=>{bp.textContent=(d.res_pct!=null?d.res_pct:'--')+'%'});
  // Update servo air needle rotation on +/-60° arc centered at vertical (mapped to configured min/max)
  if(d.air_angle!=null)rotateServoNeedle(d.air_angle);
  // Fan blade animation
  document.querySelectorAll('[id=airFanBlades]').forEach(fb=>{
    if(d.fan_pwm>0||d.pump_pwm>0){
      fb.style.animation='fanSpin '+(2-Math.min(1.8,(d.fan_pwm||d.pump_pwm||0)/255*1.8))+'s linear infinite';
    }else{
      fb.style.animation='none';
    }
  });
  // Fan duct rotation (servo flow angle rotates the output duct)
  document.querySelectorAll('[id=airFanDuct]').forEach(fd=>{
    if(d.air_angle==null)return;
    // Map servo angle: noteOff -> -30° (tilted up), max -> 0° (horizontal toward flute)
    const mn=CFG?(CFG.air_off||CFG.air_min||0):0,mx=CFG?(CFG.air_max||180):180;
    const pct=Math.min(1,Math.max(0,(d.air_angle-mn)/(mx-mn||1)));
    const ductAngle=-30+pct*30;
    fd.style.transition='transform 0.2s ease';
    fd.style.transform='rotate('+ductAngle+'deg)';
  });
  // Update pump icon state
  if(d.pump_pwm!=null){
    document.querySelectorAll('[id=airPumpIcon]').forEach(ic=>{
      ic.setAttribute('stroke',d.pump_pwm>0?'#4ecca3':'#dde')});
  }
  // Animate flow pipes when air is active
  const airActive=(d.air_angle!=null&&d.air_angle>0)||(d.pump_pwm>0)||(d.fan_pwm>0)||(d.valve_open);
  document.querySelectorAll('.airFlowAnim').forEach(fl=>{
    if(airActive)fl.classList.add('flowing');else fl.classList.remove('flowing');
  });
  // Update text stats in Air tab
  const pp=$('airPumpPwm');const sp2=$('airStatPump');
  if(pp){
    if(d.pump_pwm>0){const pct=Math.round(d.pump_pwm/255*100);pp.textContent=pct+'% ('+d.pump_pwm+')';pp.style.color='#4ecca3';if(sp2)sp2.classList.add('active-stat')}
    else{pp.textContent='OFF';pp.style.color='';if(sp2)sp2.classList.remove('active-stat')}
  }
  const apEl=$('airActivePumps');if(apEl&&d.active_pumps!=null){
    apEl.textContent=d.active_pumps+'/'+((CFG&&CFG.num_pumps)||1);
    apEl.style.color=d.active_pumps>0?'#4ecca3':'';
  }
  const fp=$('airFanPwm'),sf2=$('airStatFan');if(fp){
    if(d.fan_pwm>0){
      let t=d.fan_speed!=null?(d.fan_speed+'%'):d.fan_pwm;
      if(d.fan_idle)t+=' (idle)';
      else if(d.fan_ready===false)t+=' ...';
      fp.textContent=t;fp.style.color=d.fan_idle?'#e9a645':d.fan_ready===false?'#e9a645':'#4ecca3';if(sf2)sf2.classList.add('active-stat')}
    else{fp.textContent='OFF';fp.style.color='';if(sf2)sf2.classList.remove('active-stat')}
  }
  const rp=$('airResPct');if(rp)rp.textContent=(d.res_pct!=null?d.res_pct:'-')+'%';
  const rfb=$('airResFillBar');if(rfb&&d.res_pct!=null){rfb.style.width=d.res_pct+'%';rfb.style.background=d.res_pct>80?'#4ecca3':d.res_pct>30?'#e9a645':'#e94560'}
  const rm=$('airResMm');if(rm)rm.textContent=(d.res_mm!=null?d.res_mm:'-')+'mm';
  const vs=$('airValveState');if(vs){vs.textContent=d.valve_open?'OUVERT':'FERME';vs.style.color=d.valve_open?'#4ecca3':'#e94560'}
  const sa=$('airServoAngle');if(sa){
    sa.textContent=(d.air_angle!=null?d.air_angle:'-')+'°';
    if(d.air_angle!=null&&CFG){const m=CFG.air_mode||0;const off=(m===2||m===3)?(CFG.air_off||0):(CFG.air_min||0);const mx=CFG.air_max||180;
      sa.style.color=d.air_angle<=off?'#888':d.air_angle>=mx?'#e94560':'#4ecca3'}
  }
  const hv=$('airHallVal');if(hv)hv.textContent=(d.hall_val!=null?d.hall_val:'--');
  const es=$('airEndstopState');if(es){es.textContent=(d.endstop_st?'ACTIF':'inactif');es.style.color=d.endstop_st?'#4ecca3':'#888'}
  document.querySelectorAll('[id=airEndstopLed]').forEach(el=>{el.setAttribute('fill',d.endstop_st?'#4e4':'#a33')});
  // PID error display (mode 5 with ToF or Hall)
  const pidEv=$('airPidErrVal');
  if(pidEv&&d.res_pct!=null&&CFG&&CFG.air_mode===5){
    const target=CFG.sens_target||50;
    const err=d.res_pct-target;
    const absErr=Math.abs(err);
    pidEv.textContent=(err>=0?'+':'')+err.toFixed(0)+'%';
    pidEv.style.color=absErr<=5?'#4ecca3':absErr<=15?'#e9a645':'#e94560';
  }
  // Sensor detected status
  const so=$('airSensorOk');
  if(so&&d.sens_ok!=null){so.textContent=d.sens_ok?'OK':'ABSENT';so.style.color=d.sens_ok?'#4ecca3':'#e94560'}
  const sw=$('airSensLiveWarn');if(sw)sw.style.display=(d.sens_ok===false)?'':'none';
  // Live sensor values in config panel
  const slv=$('airSensLiveVal');
  if(slv){
    const st=CFG?CFG.sens_type:0;
    if(st<=1&&d.res_mm!=null)slv.textContent=d.res_mm+' mm ('+(d.res_pct!=null?d.res_pct:'--')+'%)';
    else if(st===2&&d.hall_val!=null){slv.textContent=d.hall_val+' ('+(d.res_pct!=null?d.res_pct:'--')+'%)';
      const hc=$('airHallCursor');if(hc)hc.style.left=(d.hall_val/4095*100)+'%'}
    else if(st>=3)slv.textContent=d.endstop_st?'ACTIF':'inactif';
  }
  // Status bar indicator
  const si=$('airStatusInd'),stt=$('airStatusText');
  if(si&&stt){
    const active=(d.pump_pwm>0||d.fan_pwm>0||d.valve_open);
    const warn=(d.sens_ok===false);
    si.style.background=warn?'#e94560':active?'#4ecca3':'#555';
    stt.textContent=warn?'Capteur absent':active?'Actif':'Repos';
    stt.style.color=warn?'#e94560':active?'#4ecca3':'#9aa';
  }
  // Update keyboard tab compact air stats
  const kas=$('kbdAirStats');
  if(kas&&CFG&&CFG.show_air){
    const m=CFG.air_mode||0;
    kas.style.display='flex';
    const ksp=$('kbdStatPump');if(ksp)ksp.style.display=(m>=4)?'':'none';
    const ksf=$('kbdStatFan');if(ksf)ksf.style.display=(m===3)?'':'none';
    const ksv=$('kbdStatValve');if(ksv)ksv.style.display=(m===0||m===1||m>=4)?'':'none';
    const ksr=$('kbdStatRes');if(ksr)ksr.style.display=(m===5)?'':'none';
    const kpv=$('kbdPumpVal');if(kpv){kpv.textContent=d.pump_pwm>0?Math.round(d.pump_pwm/255*100)+'%':'OFF';kpv.style.color=d.pump_pwm>0?'#4ecca3':''}
    const kfv=$('kbdFanVal');if(kfv){kfv.textContent=d.fan_pwm>0?(d.fan_speed!=null?d.fan_speed+'%':d.fan_pwm):'OFF';kfv.style.color=d.fan_pwm>0?'#4ecca3':''}
    const kvv=$('kbdValveVal');if(kvv){kvv.textContent=d.valve_open?'OUVERT':'FERME';kvv.style.color=d.valve_open?'#4ecca3':'#e94560'}
    const ksv2=$('kbdServoVal');if(ksv2){ksv2.textContent=(d.air_angle!=null?d.air_angle:'--')+'°';ksv2.style.color=d.air_angle>0?'#4ecca3':'#888'}
    const krv=$('kbdResVal');if(krv&&d.res_pct!=null){krv.textContent=d.res_pct+'%';krv.style.color=d.res_pct>80?'#4ecca3':d.res_pct>30?'#e9a645':'#e94560'}
  }else if(kas){kas.style.display='none'}
  // Last update timestamp
  const lu=$('airLastUpdate');if(lu)lu.textContent=new Date().toLocaleTimeString();
  // Mini chart for reservoir modes
  if(d.res_pct!=null)pushChartData(d.res_pct,d.pump_pwm||0);
}
// Mini pressure history chart (rolling 30s)
const chartData={pct:[],pwm:[],max:60};
function pushChartData(pct,pwm){
  chartData.pct.push(pct);chartData.pwm.push(pwm);
  if(chartData.pct.length>chartData.max){chartData.pct.shift();chartData.pwm.shift()}
  drawMiniChart();
}
function drawMiniChart(){
  const cv=$('airChartCanvas');if(!cv)return;
  const mc=$('airMiniChart');if(mc)mc.style.display=(CFG&&CFG.air_mode===5)?'':'none';
  if(!CFG||CFG.air_mode!==5)return;
  const ctx=cv.getContext('2d');if(!ctx)return;
  initChartTooltip();
  // Retina scaling
  const dpr=window.devicePixelRatio||1;
  const rect=cv.getBoundingClientRect();
  if(cv.width!==rect.width*dpr||cv.height!==rect.height*dpr){
    cv.width=rect.width*dpr;cv.height=rect.height*dpr;
    ctx.scale(dpr,dpr);
  }
  const w=rect.width,h=rect.height;
  ctx.clearRect(0,0,w*dpr,h*dpr);
  const n=chartData.pct.length;
  // Y-axis labels
  ctx.fillStyle='#555';ctx.font='7px sans-serif';ctx.textAlign='left';
  ctx.fillText('100%',1,9);ctx.fillText('50%',1,h/2+3);ctx.fillText('0%',1,h-2);
  if(n<2)return;
  const lm=22;// left margin for labels
  const pw=w-lm;
  const dx=pw/(chartData.max-1);
  // Sensor type for labels
  const st=CFG?CFG.sens_type||0:0;
  // Target line (continuous sensors only)
  if(st<3){
    const target=CFG.sens_target||50;
    const tY=h-target/100*h;
    ctx.setLineDash([4,4]);ctx.strokeStyle='#4ecca355';ctx.lineWidth=1;
    ctx.beginPath();ctx.moveTo(lm,tY);ctx.lineTo(w,tY);ctx.stroke();ctx.setLineDash([]);
    ctx.fillStyle='#4ecca355';ctx.font='7px sans-serif';ctx.textAlign='left';
    ctx.fillText('cible '+target+'%',lm+2,tY-2);
  }
  // Pump PWM bars (background, faint)
  ctx.fillStyle='rgba(78,204,163,0.08)';
  for(let i=0;i<n;i++){
    const bh=chartData.pwm[i]/255*h;
    ctx.fillRect(lm+(chartData.max-n+i)*dx,h-bh,dx,bh);
  }
  // Pressure line
  ctx.strokeStyle='#e94560';ctx.lineWidth=1.5;ctx.beginPath();
  let lastY=h;
  for(let i=0;i<n;i++){
    const x=lm+(chartData.max-n+i)*dx,y=h-chartData.pct[i]/100*h;
    i===0?ctx.moveTo(x,y):ctx.lineTo(x,y);
    lastY=y;
  }
  ctx.stroke();
  // Current value label at right edge
  const lastPct=chartData.pct[n-1];
  ctx.fillStyle='#e94560';ctx.font='bold 9px sans-serif';ctx.textAlign='right';
  ctx.fillText(lastPct+'%',w-2,lastY-4);
  // Legend
  ctx.font='7px sans-serif';ctx.textAlign='right';
  const sLabel=st>=3?'Etat':st===2?'Niveau Hall':'Remplissage';
  const pLabel=(CFG&&CFG.motor_type===1)?'Pompe On/Off':'Pompe PWM';
  ctx.fillStyle='#e94560';ctx.fillText(sLabel,w-2,9);
  ctx.fillStyle='rgba(78,204,163,0.4)';ctx.fillText(pLabel,w-2,18);
  // Draw tooltip crosshair if hovering
  if(_chartHoverIdx>=0&&_chartHoverIdx<n){
    const hx=lm+(chartData.max-n+_chartHoverIdx)*dx;
    ctx.strokeStyle='rgba(255,255,255,0.3)';ctx.lineWidth=1;ctx.setLineDash([2,2]);
    ctx.beginPath();ctx.moveTo(hx,0);ctx.lineTo(hx,h);ctx.stroke();ctx.setLineDash([]);
    const hp=chartData.pct[_chartHoverIdx],hw=chartData.pwm[_chartHoverIdx];
    const hy=h-hp/100*h;
    ctx.fillStyle='#fff';ctx.font='bold 8px sans-serif';ctx.textAlign='center';
    ctx.fillStyle='rgba(0,0,0,0.7)';ctx.fillRect(hx-28,hy-22,56,20);
    ctx.fillStyle='#e94560';ctx.fillText(hp+'%',hx,hy-14);
    ctx.fillStyle='#4ecca3';ctx.fillText('PWM:'+hw,hx,hy-6);
  }
}
let _chartHoverIdx=-1;
function initChartTooltip(){
  const cv=$('airChartCanvas');if(!cv||cv._ttInit)return;cv._ttInit=true;
  function chartHover(mx){
    const rect=cv.getBoundingClientRect();
    const lm=22;const pw=rect.width-lm;
    const n=chartData.pct.length;if(n<2){_chartHoverIdx=-1;return}
    const dx=pw/(chartData.max-1);
    const idx=Math.round((mx-lm)/dx-(chartData.max-n));
    _chartHoverIdx=Math.max(0,Math.min(n-1,idx));
    drawMiniChart();
  }
  cv.addEventListener('mousemove',e=>{chartHover(e.clientX-cv.getBoundingClientRect().left)});
  cv.addEventListener('mouseleave',()=>{_chartHoverIdx=-1;drawMiniChart()});
  cv.addEventListener('touchmove',e=>{e.preventDefault();const t=e.touches[0];chartHover(t.clientX-cv.getBoundingClientRect().left)},{passive:false});
  cv.addEventListener('touchend',()=>{_chartHoverIdx=-1;drawMiniChart()});
  cv.style.cursor='crosshair';
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
  const body={air_mode:wizAirMode,valve_type:0,show_air:true};
  // Apply preset if selected
  if(presetIdx>=0&&presetIdx<PR.length){
    const p=PR[presetIdx];
    body.num_fingers=p.h;body.embouchure=p.em;
    body.fingers=[];
    for(let i=0;i<p.h;i++){body.fingers.push({ch:i,a:90,d:-1,th:p.th===i?1:0})}
    body.notes=[];
    p.d.forEach(nd=>{body.notes.push({midi:nd[0],fp:nd[1],amn:nd[2],amx:nd[3],ang:nd[4]||50})});
    body.num_notes=p.d.length;body.angle_open=30;body.half_hole_pct=50;
  }
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(r=>r.json()).then(d=>{
      $('wizardOverlay').classList.remove('open');
      if(d.ok){showToast('Configuration sauvegardee','success');loadConfig()}
      else{showToast('Erreur sauvegarde','error')}
    }).catch(()=>{$('wizardOverlay').classList.remove('open');showToast('Erreur reseau','error')});
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
  ws.onopen=()=>{wsRetry=0;$('sDot').className='dot on';$('sText').textContent='Connecte';addLog('WS connecte');
    const si=$('airStatusInd');if(si)si.style.outline=''};
  ws.onclose=()=>{$('sDot').className='dot off';$('sText').textContent='Deconnecte';
    const si=$('airStatusInd');if(si){si.style.background='#e94560';si.style.outline='2px solid rgba(233,69,96,.3)'}
    const st=$('airStatusText');if(st){st.textContent='Deconnecte';st.style.color='#e94560'};
    if(_diagRunning)cancelDiagnostic();
    const d=Math.min(30000,2000*Math.pow(1.5,wsRetry));wsRetry++;setTimeout(wsConnect,d)};
  ws.onerror=()=>{ws.close()};
  ws.onmessage=e=>{try{handleWs(JSON.parse(e.data))}catch(x){addLog('WS err: '+x.message)}};
}
let _wsSendWarnT=0;
function wsSend(o){if(ws&&ws.readyState===1){ws.send(JSON.stringify(o));return true}
  const now=Date.now();if(now-_wsSendWarnT>3000){_wsSendWarnT=now;showToast('Non connecte - commande ignoree','error')}return false}

function handleWs(d){
  if(d.t==='status'){
    $('monState').textContent=STATES[d.state]||'?';
    $('monState').style.color=d.playing?'#e94560':'#4ecca3';
    if(d.heap){$('monHeap').textContent=(d.heap/1024|0)+'KB';$('heapBar').textContent=(d.heap/1024|0)+'KB';
      const ah=$('airHeapInfo');if(ah){const kb=d.heap/1024|0;ah.textContent=kb+'KB';ah.style.color=kb<30?'#e94560':kb<60?'#e9a645':'#555'}}
    updateCC(1,d.cc1);updateCC(2,d.cc2);updateCC(7,d.cc7);updateCC(11,d.cc11);
    if(d.pp!==undefined)$('progressFill').style.width=d.pp+'%';
    if(d.ppos!==undefined)$('progressText').textContent=fmt(d.ppos)+' / '+fmt(playerDuration);
    // Air system live update - trigger on any air field
    if(d.valve_open!==undefined||d.pump_pwm!==undefined||d.fan_pwm!==undefined||d.res_pct!==undefined||d.air_angle!==undefined){
      lastAirData=d;updateAirDiagram(d);
    }
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
    $('acalState').textContent=sn[d.st]||'...';
    $('acalAngle').textContent=d.st===3&&d.angle!=null?(d.angle+' deg'):''
  }else if(d.t==='acal_done'){
    autoCalRunning=false;$('btnAcalStart').style.display='';$('btnAcalStop').style.display='none';$('btnRfStart').style.display='';
    $('acalFill').style.width='100%';$('acalState').textContent='Termine !';$('acalAngle').textContent='';addLog('Auto-cal OK');
    if(d.results){let h='';d.results.forEach(r=>{h+='<div style="display:flex;justify-content:space-between;font-size:.8em;padding:2px 0">'+
      '<span>'+esc(r.name)+'</span><span style="color:'+(r.ok?'#4ecca3':'#e94560')+'">'+(r.ok?esc(r.min+'%-'+r.max+'%'+(r.minA!=null?' ('+r.minA+'deg-'+r.maxA+'deg)':'')):'Echec')+'</span></div>'});
      $('acalResults').innerHTML=h;$('acalResults').style.display='block'}
    setTimeout(loadConfig,1000)
  }else if(d.t==='rf_prog'){
    $('rfProgress').style.display='block';$('rfAngle').textContent=(d.angle||0)+' deg';
    $('rfFill').style.width=((d.angle||0)/180*100)+'%';
    if(d.min!=null)$('rfStep').textContent='Min: '+d.min+' deg - Sweep...';
  }else if(d.t==='rf_done'){
    $('btnRfStart').style.display='';$('btnRfStop').style.display='none';$('btnAcalStart').style.display='';
    $('rfFill').style.width='100%';
    if(d.ok&&d.min!=null){$('rfMinVal').textContent=d.min;$('rfMaxVal').textContent=d.max;
      $('rfResult').style.display='block';$('rfStep').textContent='Plage detectee !'}
    else{$('rfStep').textContent='Aucun son detecte'}
  }else if(d.t==='rf_applied'){
    showToast('Plage servo mise a jour: '+d.min+'deg-'+d.max+'deg','success');
    $('rfProgress').style.display='none';$('btnRfStart').style.display='';
    if(CFG){CFG.air_min=d.min;CFG.air_max=d.max}
    if($('cfgAirMin'))$('cfgAirMin').value=d.min;if($('cfgAirMax'))$('cfgAirMax').value=d.max;
    buildAirflowRows();setTimeout(loadConfig,500)
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
    buildAirSvg('airSvgFull',true);updateTravVisibility();
    if(CFG.first_boot){showWizard()}
    if(micDetected){$('micSection').style.display='';wsSend({t:'mic_mon',on:1})}
    else $('micSection').style.display='none';
  }).catch(e=>{addLog('Erreur config: '+e);showToast('Erreur chargement config','error')})
}

function updateTravVisibility(){
  const isTrav=CFG&&CFG.embouchure==='trav';
  document.querySelectorAll('.trav-only').forEach(el=>{el.style.display=isTrav?'block':'none'});
  // Populate angle servo config fields
  if(isTrav&&CFG){
    const e=id=>$(id);
    if(e('cfgAngPca'))e('cfgAngPca').value=CFG.ang_pca!=null?CFG.ang_pca:12;
    if(e('cfgAngOff'))e('cfgAngOff').value=CFG.ang_off!=null?CFG.ang_off:90;
    if(e('cfgAngMin'))e('cfgAngMin').value=CFG.ang_min!=null?CFG.ang_min:60;
    if(e('cfgAngMax'))e('cfgAngMax').value=CFG.ang_max!=null?CFG.ang_max:120;
  }
}

// --- KEYBOARD ---
function buildKeyboard(){
  const c=$('pianoKeys');c.innerHTML='';if(!CFG||!CFG.notes||!CFG.notes.length){c.innerHTML='<div style="color:#888;padding:16px;text-align:center">Aucune note</div>';return}
  if(CFG.kbd_mode===1){buildPianoKeyboard(c)}else{buildFluteKeyboard(c)}
  buildKeyMap();updKbdToggle()
}
function updKbdToggle(){const l=$('kbdModeLabel');if(l&&CFG)l.textContent=CFG.kbd_mode===1?'Flute':'Piano'}
function toggleKbdMode(){
  if(!CFG)return;const prev=CFG.kbd_mode;CFG.kbd_mode=CFG.kbd_mode===1?0:1;
  buildKeyboard();
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({kbd_mode:CFG.kbd_mode})})
    .catch(()=>{CFG.kbd_mode=prev;buildKeyboard();showToast('Erreur reseau','error')})
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
  // Souffle: pre-positionner le servo airflow pendant le positionnement des doigts
  // Seulement pour les modes avec valve physique (0,1,4,5) ou la valve bloque l'air
  // Modes 2 (servo seul) et 3 (ventilateur): ne pas envoyer air_live car le servo
  // controle directement l'air et produirait du son avant le positionnement des doigts
  if(CFG){const am=CFG.air_mode||0;
    if(am!==2&&am!==3){const nd=CFG.notes.find(n=>n.midi===midi);if(nd){
    const airPct=nd.amn+Math.round((nd.amx-nd.amn)*velocity/127);
    wsSend({t:'air_live',v:airPct})}}}
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
  if(e.key==='y'&&calibStep===2){e.preventDefault();redoFp()}
  // Ctrl+S on Air tab = save air settings
  const airTab=$('tab-air');
  if(e.key==='s'&&airTab&&airTab.classList.contains('active')){e.preventDefault();saveAirSettings()}
});
// Air tab keyboard shortcuts (no modifier keys, only when Air tab active)
document.addEventListener('keydown',e=>{
  if(e.target.tagName==='INPUT'||e.target.tagName==='SELECT'||e.target.tagName==='TEXTAREA')return;
  if(e.ctrlKey||e.altKey||e.metaKey)return;
  const airTab=$('tab-air');if(!airTab||!airTab.classList.contains('active'))return;
  const m=CFG?CFG.air_mode||0:0;
  if(e.key==='Escape'){e.preventDefault();wsSend({t:'pump_stop'});wsSend({t:'fan_stop'});showToast('Arret envoye','info')}
  if(e.key==='t'){e.preventDefault();const dur=parseInt(($('airTestDur')||{}).value)||2;
    wsSend({t:'pump_target',v:50});setTimeout(()=>wsSend({t:'pump_stop'}),dur*1000);showToast('Test '+dur+'s','success')}
  if(e.key==='h'){e.preventDefault();toggleAirHelp()}
  if(e.key==='?'){e.preventDefault();
    showToast('Raccourcis: Ctrl+S=Sauver, T=Test, Esc=Stop, H=Aide, ?=Info','info')}
});

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

function buildFlute(cfg,svgId,showNums){
  const svg=$(svgId);if(!svg||!cfg)return;
  const nf=cfg.num_fingers||6,fingers=cfg.fingers||[];
  const em=cfg.embouchure||'bec';
  // Ocarina = forme speciale
  if(em==='oca'){buildOcarina(cfg,svgId,showNums);return}
  const sp=50,sx=100,r=14;
  const topHoles=[],botHoles=[];
  for(let i=0;i<nf;i++){(fingers[i]&&fingers[i].th?botHoles:topHoles).push(i)}
  const posTop=topHoles.map((_,i)=>sx+i*sp);
  const posBot=botHoles.map((_,i)=>sx+i*sp);
  const allX=[...posTop,...posBot,sx+200];
  const tw=Math.max(...allX)+60;
  const svgH=100;
  const h_top=35,h_bot=65,cy=50;
  svg.setAttribute('viewBox','0 0 '+tw+' '+svgH);
  const g=svgId,ty=cy-16,by=cy+16,th=by-ty;
  let h=fluteGrad(g,em);
  const fluteStartX=4;
  // Corps du tube (demarre apres l'embouchure)
  const bx=fluteStartX+(em==='trav'?59:54);
  h+='<rect x="'+bx+'" y="'+ty+'" width="'+(tw-bx-10)+'" height="'+th+'" rx="0" fill="url(#wg_'+g+')" stroke="#5C4A0A" stroke-width="1.5"/>';
  h+='<rect x="'+bx+'" y="'+ty+'" width="'+(tw-bx-10)+'" height="6" rx="0" fill="#D4B044" opacity=".18"/>';
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
  // Angle servo PCA dropdown (trav only)
  const angSel=$('calAngPca');if(angSel){angSel.innerHTML='';
    for(let i=0;i<16;i++){const o=document.createElement('option');o.value=i;o.textContent='PCA '+i;angSel.appendChild(o)}
    angSel.value=CFG.ang_pca!=null?CFG.ang_pca:12;
    angSel.addEventListener('change',function(){syncAngPca(this.value);checkPca()});
  }
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
        '<button class="btn btn-s" onclick="testPulse(this);testFinger('+i+',CFG.fingers['+i+'].a+(CFG.angle_open||30)*CFG.fingers['+i+'].d)">Ouvrir</button>'+
        '<button class="btn btn-s" onclick="testPulse(this);testFingerHalf('+i+')">Demi</button></div>'+
      '<div style="margin:6px 0"><div style="display:flex;justify-content:space-between;font-size:.75em;color:#888;margin-bottom:2px">'+
        '<span>Demi %</span><span id="fhpv'+i+'">'+(f.hp||0)+'% <span style="color:#555">'+(f.hp?'':'(global)')+'</span></span></div>'+
        '<input type="range" min="0" max="100" value="'+(f.hp||0)+'" id="fhp'+i+'" style="width:100%"'+
          ' oninput="CFG.fingers['+i+'].hp=parseInt(this.value);$(\'fhpv'+i+'\').innerHTML=this.value+\'% <span style=color:#555>\'+(this.value>0?\'\':\'(global)\')+\'</span>\';markDirty()"></div>'+
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
  const groups={};PR.forEach(p=>{const k=p.em||'bec';(groups[k]=groups[k]||[]).push(p)});
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
  CFG.num_fingers=p.h;CFG.embouchure=p.em||'bec';
  while(CFG.fingers.length<p.h)CFG.fingers.push({ch:CFG.fingers.length,a:90,d:-1,th:0,hp:0});
  CFG.fingers.forEach(f=>f.th=0);
  if(p.th>=0&&CFG.fingers[p.th])CFG.fingers[p.th].th=1;
  // Rebuild UI physique
  buildFingerCards();buildFlute(CFG,'calFluteSvg',true);updateTravVisibility();updateAngleBlockVisibility();markDirty();
  showToast(p.n+' - '+p.h+' trous'+(p.th>=0?' (pouce)':''),'success')
}

function testFinger(i,a){wsSend({t:'test_finger',i:i,a:parseInt(a)});
  const el=$('fh_calFluteSvg_'+i);if(el){const closed=CFG.fingers[i].a;const open=Math.abs(a-closed)>(CFG.angle_open||30)/2;
    el.setAttribute('class','flute-hole '+(open?'open':'closed')+(CFG.fingers[i].th?' thumb':''))}}
function testFingerHalf(i){const f=CFG.fingers[i];const hp=f.hp||CFG.half_hole_pct||50;
  const a=f.a+Math.round((CFG.angle_open||30)*f.d*hp/100);testFinger(i,a);
  const el=$('fh_calFluteSvg_'+i);if(el)el.setAttribute('class','flute-hole half'+(f.th?' thumb':''))}

function saveStep1(){
  if(!CFG)return;btnLoad('btnSaveStep1',true);
  CFG.angle_open=parseInt($('angleOpen').value);CFG.half_hole_pct=parseInt($('halfHolePct').value);CFG.air_pca=parseInt($('airPca').value);
  for(let i=0;i<CFG.num_fingers;i++){
    CFG.fingers[i].ch=parseInt($('fch'+i).value);
    CFG.fingers[i].a=parseInt($('fa'+i).value);
    CFG.fingers[i].d=parseInt($('fd'+i).value);
    const thEl=$('fth'+i);CFG.fingers[i].th=thEl?thEl.checked?1:0:0;
    CFG.fingers[i].hp=parseInt($('fhp'+i).value)||0
  }
  const body={num_fingers:CFG.num_fingers,air_pca:CFG.air_pca,angle_open:CFG.angle_open,half_hole_pct:CFG.half_hole_pct,embouchure:CFG.embouchure||'bec',fingers:CFG.fingers.slice(0,CFG.num_fingers)};
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
  CFG.embouchure=p.em||'bec';
  // Build notes from preset data
  CFG.notes=p.d.map(n=>({midi:n[0],fp:[...n[1]],amn:n[2],amx:n[3],ang:n[4]||50}));
  CFG.notes.forEach(n=>{while(n.fp.length<CFG.num_fingers)n.fp.push(0)});
  CFG.num_notes=CFG.notes.length;
  fpSnap();buildFingeringRows();buildFlute(CFG,'calFluteSvg',true);updPresetInfo();updateTravVisibility();updateAngleBlockVisibility();markDirty()
}

function saveStep2(){
  if(!CFG)return;btnLoad('btnSaveStep2',true);
  const body={num_fingers:CFG.num_fingers,notes:CFG.notes.map(n=>({midi:n.midi,fp:n.fp.slice(0,CFG.num_fingers),amn:n.amn,amx:n.amx,ang:n.ang!=null?n.ang:50}))};
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(r=>r.json()).then(d=>{btnLoad('btnSaveStep2',false);if(d.ok){showToast('Doigtes sauvegardes','success');markClean();fpHistory=[];fpFuture=[];updUndoUI();goStep(3);buildKeyboard();buildFlute(CFG,'fluteSvg',false)}else showToast('Erreur sauvegarde','error')})
    .catch(e=>{btnLoad('btnSaveStep2',false);showToast('Erreur: '+e,'error')})
}

// --- STEP 3: AIRFLOW ---
function pctToAngle(pct){if(!CFG)return'?';const mn=CFG.air_min||0,mx=CFG.air_max||180;return Math.round(mn+(mx-mn)*pct/100)}
function buildAirflowRows(){
  const c=$('airflowRows');c.innerHTML='';if(!CFG)return;
  const isTrav=(CFG.embouchure==='trav');
  CFG.notes.forEach((n,ni)=>{
    let dots='';for(let f=0;f<CFG.num_fingers;f++)dots+='<span class="kf '+kfClass(n.fp[f])+'"></span>';
    const angPct=n.ang!=null?n.ang:50;
    const d=document.createElement('div');d.className='air-card fade-in fade-delay-'+(Math.min(4,(ni%4)+1));
    d.innerHTML='<span class="air-note">'+mn(n.midi)+'</span>'+
      '<span class="kf-row" style="gap:2px">'+dots+'</span>'+
      '<div class="air-sliders">'+
        '<div class="air-vals"><span>Min: <b id="amn'+ni+'">'+n.amn+'</b>% (<b id="amnA'+ni+'">'+pctToAngle(n.amn)+'</b>deg)</span><span>Max: <b id="amx'+ni+'">'+n.amx+'</b>% (<b id="amxA'+ni+'">'+pctToAngle(n.amx)+'</b>deg)</span></div>'+
        '<div class="dual-range"><div class="dual-range-track"></div><div class="dual-range-fill" id="drf'+ni+'"></div>'+
        '<input type="range" min="0" max="100" value="'+n.amn+'" oninput="CFG.notes['+ni+'].amn=parseInt(this.value);$(\'amn'+ni+'\').textContent=this.value;$(\'amnA'+ni+'\').textContent=pctToAngle(this.value);updDualFill('+ni+');markDirty()">'+
        '<input type="range" min="0" max="100" value="'+n.amx+'" oninput="CFG.notes['+ni+'].amx=parseInt(this.value);$(\'amx'+ni+'\').textContent=this.value;$(\'amxA'+ni+'\').textContent=pctToAngle(this.value);updDualFill('+ni+');markDirty()"></div>'+
      '</div>'+
      '<div class="trav-only" style="min-width:90px;'+(isTrav?'display:flex':'')+'"><span style="font-size:.7em;color:#9aa;white-space:nowrap">Angle: <b id="ang'+ni+'">'+angPct+'</b>%</span>'+
        '<input type="range" min="0" max="100" value="'+angPct+'" style="width:80px" oninput="CFG.notes['+ni+'].ang=parseInt(this.value);$(\'ang'+ni+'\').textContent=this.value;markDirty()"></div>'+
      '<button class="btn btn-s" style="padding:4px 8px;font-size:.75em" onclick="testPulse(this);testCalNote('+n.midi+')">Test</button>';
    c.appendChild(d);updDualFill(ni)
  })
}

function testCalNote(midi){wsSend({t:'test_note',n:midi});wsSend({t:'test_sol',o:1});
  setTimeout(()=>wsSend({t:'test_sol',o:0}),TEST_SOL_MS)}

function startRangeFinder(){$('btnRfStart').style.display='none';$('btnRfStop').style.display='';
  $('btnAcalStart').style.display='none';
  $('rfProgress').style.display='block';$('rfResult').style.display='none';wsSend({t:'auto_cal',mode:'range'})}
function stopRangeFinder(){$('btnRfStart').style.display='';$('btnRfStop').style.display='none';
  $('btnAcalStart').style.display='';
  $('rfProgress').style.display='none';wsSend({t:'auto_cal',mode:'stop'})}
function applyRangeResult(){wsSend({t:'auto_cal',mode:'apply_range'})}
function dismissRangeResult(){$('rfProgress').style.display='none';$('btnRfStart').style.display='';$('btnRfStop').style.display='none'}
function startAutoCal(){autoCalRunning=true;$('btnAcalStart').style.display='none';$('btnAcalStop').style.display='';
  $('btnRfStart').style.display='none';
  $('acalProgress').style.display='block';$('acalResults').style.display='none';wsSend({t:'auto_cal',mode:'air'})}
function stopAutoCal(){autoCalRunning=false;$('btnAcalStart').style.display='';$('btnAcalStop').style.display='none';
  $('btnRfStart').style.display='';wsSend({t:'auto_cal',mode:'stop'})}

function saveStep3(){
  if(!CFG)return;btnLoad('btnSaveStep3',true);
  const body={notes_air:CFG.notes.map(n=>({amn:n.amn,amx:n.amx,ang:n.ang!=null?n.ang:50}))};
  if(CFG.embouchure==='trav'){body.ang_pca=CFG.ang_pca;body.ang_off=CFG.ang_off;body.ang_min=CFG.ang_min;body.ang_max=CFG.ang_max}
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
  $('cfgSmidiOn').checked=!!CFG.smidi_on;
  $('cfgSmidiRx').value=CFG.smidi_rx!=null?CFG.smidi_rx:16;
  $('cfgDelay').value=CFG.servo_delay;$('cfgValveInt').value=CFG.valve_interval;$('cfgMinNote').value=CFG.min_note_dur;
  $('cfgCCVol').value=CFG.cc_vol!=null?CFG.cc_vol:127;$('cfgCCExpr').value=CFG.cc_expr!=null?CFG.cc_expr:127;
  $('cfgCCMod').value=CFG.cc_mod!=null?CFG.cc_mod:0;$('cfgCCBreath').value=CFG.cc_breath!=null?CFG.cc_breath:127;
  $('cfgCCBright').value=CFG.cc_bright!=null?CFG.cc_bright:64;
  $('cfgUnpower').value=CFG.time_unpower;
  $('cfgMidiLimit').value=CFG.midi_limit||500;
  $('cfgColor').value=CFG.color||'#D4B044';
  $('cfgHideCalib').checked=!!CFG.hide_calib;
  $('cfgHideAir').checked=!!CFG.hide_air;
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
    smidi_on:$('cfgSmidiOn').checked,smidi_rx:parseInt($('cfgSmidiRx').value),
    servo_delay:parseInt($('cfgDelay').value),valve_interval:parseInt($('cfgValveInt').value),
    min_note_dur:parseInt($('cfgMinNote').value),
    cc_vol:parseInt($('cfgCCVol').value),cc_expr:parseInt($('cfgCCExpr').value),
    cc_mod:parseInt($('cfgCCMod').value),cc_breath:parseInt($('cfgCCBreath').value),cc_bright:parseInt($('cfgCCBright').value),
    time_unpower:parseInt($('cfgUnpower').value),midi_limit:parseInt($('cfgMidiLimit').value),
    color:$('cfgColor').value,
    hide_calib:$('cfgHideCalib').checked,hide_air:$('cfgHideAir').checked};
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
