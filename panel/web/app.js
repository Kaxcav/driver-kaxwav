// ============================================================================
// KaxWav Studio Suite — Pro Hardware Audio Console Controller
// Dedicated 100% to Behringer U-Phoria UM2 (TI Burr-Brown PCM2902 Core)
// 60 FPS Telemetry, CRT Oscilloscope / RTA, Calibrated Vertical VU & Vector Knobs
// ============================================================================

// ----------------------------------------------------------------------------
// 1. CALIBRATED STUDIO VERTICAL VU METER (Vertical Segmented Ladder + Peak Hold)
// ----------------------------------------------------------------------------
class VerticalVuMeter {
  constructor(canvas, clipLedId = null, dbBoxId = null) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.clipLed = clipLedId ? document.getElementById(clipLedId) : null;
    this.dbBox = dbBoxId ? document.getElementById(dbBoxId) : null;

    this.currentLevel = 0.0;
    this.peakLevel = 0.0;
    this.peakHoldFrames = 0;
    this.isClipped = false;

    // Reset clip ao clicar no LED de pico
    if (this.clipLed) {
      this.clipLed.addEventListener('click', () => {
        this.resetClip();
      });
    }

    this.initCanvasResolution();
  }

  initCanvasResolution() {
    const dpr = window.devicePixelRatio || 1;
    const rect = this.canvas.getBoundingClientRect();
    const w = rect.width || this.canvas.width;
    const h = rect.height || this.canvas.height;

    this.canvas.width = Math.round(w * dpr);
    this.canvas.height = Math.round(h * dpr);
    this.ctx.scale(dpr, dpr);
    this.renderWidth = w;
    this.renderHeight = h;
  }

  resetClip() {
    this.isClipped = false;
    this.peakLevel = 0.0;
    this.peakHoldFrames = 0;
    if (this.clipLed) {
      this.clipLed.classList.remove('clipped', 'on');
    }
  }

  update(rawLevel) {
    // Balística de ataque instantânea e decay suave de estúdio
    if (rawLevel >= this.currentLevel) {
      this.currentLevel = rawLevel;
    } else {
      this.currentLevel = this.currentLevel * 0.86 + rawLevel * 0.14;
    }

    // Trava de Clip acima de -0.5 dBFS (~0.944 linear)
    if (rawLevel >= 0.94) {
      this.isClipped = true;
      if (this.clipLed) {
        this.clipLed.classList.add('clipped', 'on');
      }
    }

    // Peak Hold com trava por 1.5s (~90 quadros a 60 FPS)
    if (this.currentLevel >= this.peakLevel) {
      this.peakLevel = this.currentLevel;
      this.peakHoldFrames = 90;
    } else {
      if (this.peakHoldFrames > 0) {
        this.peakHoldFrames--;
      } else {
        this.peakLevel = Math.max(0, this.peakLevel - 0.012);
      }
    }

    // Atualiza Leitura Digital Tabular se vinculada
    if (this.dbBox) {
      this.dbBox.textContent = this.linearToDb(this.currentLevel);
    }

    this.draw();
  }

  linearToDb(val) {
    if (val <= 0.0001) return '-∞ dBFS';
    const db = 20 * Math.log10(val);
    if (db <= -60) return '-∞ dBFS';
    return `${db >= 0 ? '+' : ''}${db.toFixed(1)} dBFS`;
  }

  draw() {
    const { ctx, renderWidth: w, renderHeight: h, currentLevel, peakLevel } = this;
    ctx.clearRect(0, 0, w, h);

    // Fundo do trilho escuro
    ctx.fillStyle = '#07090C';
    ctx.fillRect(0, 0, w, h);

    // Segmentos LED verticais (de baixo para cima)
    const numSegments = 40;
    const gap = 1.5;
    const segHeight = (h - (numSegments - 1) * gap) / numSegments;
    const activeSegments = Math.round(currentLevel * numSegments);

    for (let i = 0; i < numSegments; i++) {
      // Índice de baixo para cima
      const segIndexFromBottom = i;
      const y = h - (i + 1) * segHeight - i * gap;
      const ratio = segIndexFromBottom / numSegments;

      let color;
      let glow = false;

      if (ratio < 0.70) {
        // -48 dBFS a -12 dBFS: Electric Neon Lime (idêntico à referência de estúdio)
        color = '#D4FF00';
      } else if (ratio < 0.92) {
        // -12 dBFS a -0.5 dBFS: Âmbar Hardware
        color = '#FFB300';
      } else {
        // Acima de -0.5 dBFS: Ruby Red Clip
        color = '#FF2638';
        glow = true;
      }

      if (segIndexFromBottom < activeSegments) {
        ctx.fillStyle = color;
        if (glow) {
          ctx.shadowColor = '#FF2638';
          ctx.shadowBlur = 6;
        } else {
          ctx.shadowColor = '#D4FF00';
          ctx.shadowBlur = 3;
        }
        ctx.fillRect(1, y, w - 2, segHeight);
      } else {
        // Segmento inativo apagado
        ctx.fillStyle = 'rgba(255, 255, 255, 0.035)';
        ctx.shadowBlur = 0;
        ctx.fillRect(1, y, w - 2, segHeight);
      }
    }

    // Traço de Peak Hold (brilhante)
    if (peakLevel > 0.02) {
      const peakY = Math.max(1, Math.min(h - 2, h - Math.round(peakLevel * h)));
      ctx.fillStyle = peakLevel > 0.92 ? '#EF4444' : '#FFFFFF';
      ctx.shadowColor = ctx.fillStyle;
      ctx.shadowBlur = 3;
      ctx.fillRect(0, peakY - 1, w, 2);
      ctx.shadowBlur = 0;
    }
  }
}

// ----------------------------------------------------------------------------
// 2. DUAL SIGNAL MONITOR (Osciloscópio CRT + Analisador de Espectro RTA)
// ----------------------------------------------------------------------------
class DualSignalMonitor {
  constructor(canvas) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.mode = 'wave'; // 'wave', 'rta', 'dual', 'tuner'
    this.source = 'master'; // 'master', 'ch1', 'ch2', 'loopback'
    this.phase = 0;
    this.energy = 0.0;
    this.scopeBuffer = new Float32Array(128);
    this.spectrumBands = new Float32Array(36);
    this.spectrumPeaks = new Float32Array(36);
    this.tunerData = { freq: 0.0, note: 0, noteName: '--', cents: 0.0, source: 1 };
    this.smoothCents = 0.0;
    this.strobePhase = 0.0;

    // Pré-computa tabelas trigonométricas para DFT logarítmica de 36 bandas (40Hz a 16kHz)
    this.cosTable = new Float32Array(36 * 128);
    this.sinTable = new Float32Array(36 * 128);
    const fMin = 40.0;
    const fMax = 16000.0;
    for (let k = 0; k < 36; k++) {
      const freq = fMin * Math.pow(fMax / fMin, k / 35.0);
      const omega = (2.0 * Math.PI * freq) / 48000.0;
      const baseIdx = k * 128;
      for (let n = 0; n < 128; n++) {
        // Janela Hann para evitar vazamento espectral
        const win = 0.5 * (1.0 - Math.cos((2.0 * Math.PI * n) / 127.0));
        this.cosTable[baseIdx + n] = Math.cos(omega * n) * win;
        this.sinTable[baseIdx + n] = Math.sin(omega * n) * win;
      }
    }

    this.initCanvas();
    window.addEventListener('resize', () => this.initCanvas());
  }

  initCanvas() {
    const dpr = window.devicePixelRatio || 1;
    const rect = this.canvas.getBoundingClientRect();
    const w = rect.width || this.canvas.parentElement.clientWidth || 980;
    const h = 92;

    this.canvas.width = Math.round(w * dpr);
    this.canvas.height = Math.round(h * dpr);
    this.ctx.scale(dpr, dpr);
    this.w = w;
    this.h = h;
  }

  setAudioData(buffer, rawEnergy) {
    this.energy = this.energy * 0.78 + rawEnergy * 0.22;
    if (buffer && buffer.length >= 128) {
      for (let i = 0; i < 128; i++) {
        this.scopeBuffer[i] = buffer[i];
      }
    }
    this.updateSpectrumFromBuffer(rawEnergy);
  }

  updateSpectrumFromBuffer(rawEnergy) {
    const numBands = 36;
    for (let k = 0; k < numBands; k++) {
      let re = 0.0;
      let im = 0.0;
      const baseIdx = k * 128;
      for (let n = 0; n < 128; n++) {
        const sample = this.scopeBuffer[n];
        re += sample * this.cosTable[baseIdx + n];
        im += sample * this.sinTable[baseIdx + n];
      }
      const mag = Math.sqrt(re * re + im * im) / 24.0;
      // Curva de ponderação auditiva musical (Pink Noise Equalization)
      const weighting = 1.0 + (k / numBands) * 0.75;
      const target = Math.min(1.0, (mag * 3.2 + rawEnergy * 0.12) * weighting);

      if (target >= this.spectrumBands[k]) {
        this.spectrumBands[k] = target;
      } else {
        this.spectrumBands[k] *= 0.84;
      }

      if (this.spectrumBands[k] >= this.spectrumPeaks[k]) {
        this.spectrumPeaks[k] = this.spectrumBands[k];
      } else {
        this.spectrumPeaks[k] = Math.max(0, this.spectrumPeaks[k] - 0.012);
      }
    }
  }

  setTunerData(data) {
    if (!data) return;
    this.tunerData = {
      freq: data.freq || 0.0,
      note: data.note || 0,
      noteName: data.noteName || '--',
      cents: data.cents || 0.0,
      source: typeof data.source === 'number' ? data.source : 1
    };
  }

  draw() {
    const { ctx, w, h, mode } = this;
    if (!w || !h) return;

    // Fundo do osciloscópio CRT com leve persistência de fósforo
    ctx.fillStyle = mode === 'tuner' ? 'rgba(5, 6, 8, 0.85)' : 'rgba(5, 6, 8, 0.40)';
    ctx.fillRect(0, 0, w, h);

    if (mode === 'tuner') {
      this.drawTuner();
    } else if (mode === 'rta') {
      this.drawRta(1.0);
    } else if (mode === 'dual') {
      this.drawRta(0.35);
      this.drawWave();
    } else {
      this.drawWave();
    }

    this.phase += 0.18;
  }

  drawTuner() {
    const { ctx, w, h, tunerData } = this;
    const midX = w / 2;
    const hasSignal = tunerData.freq >= 27.5 && tunerData.note > 0;
    const rawCents = hasSignal ? Math.max(-50, Math.min(50, tunerData.cents)) : 0.0;

    if (hasSignal) {
      this.smoothCents = this.smoothCents * 0.78 + rawCents * 0.22;
      if (Math.abs(rawCents) > 1.8) {
        this.strobePhase += rawCents * 0.022;
      }
    } else {
      this.smoothCents *= 0.88;
    }

    const inTune = hasSignal && Math.abs(this.smoothCents) <= 2.5;
    const nearTune = hasSignal && Math.abs(this.smoothCents) <= 10.0;
    const accentColor = !hasSignal ? '#52525E' : (inTune ? '#D4FF00' : (nearTune ? '#FFB300' : '#FF2638'));

    // 1. Faixa Estroboscópica Superior (Strobe Track)
    const trackMargin = 48;
    const trackW = w - trackMargin * 2;
    const trackY = 30;
    const trackH = 12;

    ctx.fillStyle = '#07090D';
    ctx.fillRect(trackMargin, trackY, trackW, trackH);
    ctx.strokeStyle = 'rgba(255, 255, 255, 0.08)';
    ctx.lineWidth = 1;
    ctx.strokeRect(trackMargin, trackY, trackW, trackH);

    ctx.save();
    ctx.beginPath();
    ctx.rect(trackMargin + 1, trackY + 1, trackW - 2, trackH - 2);
    ctx.clip();

    const barSpacing = 28;
    const offset = ((this.strobePhase % barSpacing) + barSpacing) % barSpacing;
    ctx.fillStyle = hasSignal ? accentColor : 'rgba(255, 255, 255, 0.05)';
    ctx.globalAlpha = hasSignal ? (inTune ? 0.85 : 0.45) : 0.25;
    for (let x = trackMargin - barSpacing + offset; x < trackMargin + trackW + barSpacing; x += barSpacing) {
      ctx.fillRect(x, trackY + 2, 12, trackH - 4);
    }
    ctx.restore();

    // 2. Régua de Cents (-50c a +50c)
    const scaleY = 54;
    ctx.strokeStyle = 'rgba(255, 255, 255, 0.14)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(trackMargin, scaleY);
    ctx.lineTo(trackMargin + trackW, scaleY);
    ctx.stroke();

    const ticks = [-50, -25, -10, 0, 10, 25, 50];
    ctx.font = '700 8px "JetBrains Mono", monospace';
    ctx.textAlign = 'center';
    ticks.forEach(t => {
      const tx = midX + (t / 50.0) * (trackW * 0.46);
      const isZero = t === 0;
      ctx.strokeStyle = isZero ? '#D4FF00' : 'rgba(255, 255, 255, 0.25)';
      ctx.lineWidth = isZero ? 2 : 1;
      ctx.beginPath();
      ctx.moveTo(tx, scaleY - (isZero ? 5 : 3));
      ctx.lineTo(tx, scaleY + (isZero ? 5 : 3));
      ctx.stroke();

      ctx.fillStyle = isZero ? '#D4FF00' : '#7C7C8A';
      ctx.fillText(t === 0 ? '0c' : `${t > 0 ? '+' : ''}${t}c`, tx, scaleY - 6);
    });

    // 3. Agulha de Pitch (Needle Indicator)
    if (hasSignal) {
      const needleX = midX + (this.smoothCents / 50.0) * (trackW * 0.46);
      ctx.save();
      ctx.fillStyle = accentColor;
      ctx.shadowColor = accentColor;
      ctx.shadowBlur = inTune ? 12 : 6;
      ctx.beginPath();
      ctx.moveTo(needleX, scaleY + 2);
      ctx.lineTo(needleX - 5, scaleY + 10);
      ctx.lineTo(needleX + 5, scaleY + 10);
      ctx.closePath();
      ctx.fill();
      ctx.restore();
    }

    // 4. Display Central de Nota, Frequência Hz e Fonte
    const srcLabels = ['OFF', 'MIC 1 VOCAL', 'INST 2 GUITAR / BASS', 'LOOPBACK 808 KEY FINDER'];
    const srcName = srcLabels[tunerData.source] || 'MIC 1 VOCAL';

    // Badge Esquerda: Fonte Ativa
    ctx.textAlign = 'left';
    ctx.font = '700 9px "JetBrains Mono", monospace';
    ctx.fillStyle = '#00E5FF';
    ctx.fillText(`FONTE: ${srcName}`, trackMargin, h - 8);

    // Nota Central Gigante
    ctx.textAlign = 'center';
    ctx.font = '800 22px "JetBrains Mono", monospace';
    ctx.fillStyle = hasSignal ? accentColor : '#52525E';
    if (inTune) {
      ctx.shadowColor = '#D4FF00';
      ctx.shadowBlur = 12;
    }
    ctx.fillText(hasSignal ? tunerData.noteName : '--', midX, h - 7);
    ctx.shadowBlur = 0;

    // Direita: Leitura de Frequência e Desvio em Cents
    ctx.textAlign = 'right';
    ctx.font = '700 9px "JetBrains Mono", monospace';
    if (hasSignal) {
      const centsStr = `${this.smoothCents >= 0 ? '+' : ''}${this.smoothCents.toFixed(1)} cents`;
      const lockBadge = inTune ? ' • ★ AFINADO' : '';
      ctx.fillStyle = accentColor;
      ctx.fillText(`${tunerData.freq.toFixed(1)} Hz (${centsStr})${lockBadge}`, trackMargin + trackW, h - 8);
    } else {
      ctx.fillStyle = '#52525E';
      ctx.fillText('AGUARDANDO NOTA FUNDAMENTAL...', trackMargin + trackW, h - 8);
    }
  }

  drawWave() {
    const { ctx, w, h, energy } = this;
    const midY = h / 2;
    const ampY = h * 0.45;

    // Gradiente vertical sob a onda em Electric Lime
    const grad = ctx.createLinearGradient(0, 0, 0, h);
    grad.addColorStop(0, 'rgba(212, 255, 0, 0.28)');
    grad.addColorStop(0.5, 'rgba(212, 255, 0, 0.07)');
    grad.addColorStop(1, 'rgba(212, 255, 0, 0.00)');

    ctx.beginPath();
    ctx.moveTo(0, midY);

    const stepX = w / 127.0;
    for (let i = 0; i < 128; i++) {
      const x = i * stepX;
      let sample = this.scopeBuffer[i] || 0.0;
      if (energy < 0.005) {
        sample += Math.sin(this.phase + i * 0.35) * 0.0015;
      }
      const y = midY - (sample * ampY);
      ctx.lineTo(x, y);
    }

    // Preenchimento translúcido sob a onda
    ctx.lineTo(w, h);
    ctx.lineTo(0, h);
    ctx.closePath();
    ctx.fillStyle = grad;
    ctx.fill();

    // Traçado da linha de fósforo Electric Neon Lime (#D4FF00) com brilho de CRT
    ctx.beginPath();
    for (let i = 0; i < 128; i++) {
      const x = i * stepX;
      let sample = this.scopeBuffer[i] || 0.0;
      if (energy < 0.005) {
        sample += Math.sin(this.phase + i * 0.35) * 0.0015;
      }
      const y = midY - (sample * ampY);
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }

    ctx.lineWidth = 2.0;
    ctx.strokeStyle = '#D4FF00';
    ctx.shadowColor = '#D4FF00';
    ctx.shadowBlur = energy > 0.02 ? 9 : 3;
    ctx.stroke();
    ctx.shadowBlur = 0;
  }

  drawRta(alpha = 1.0) {
    const { ctx, w, h } = this;
    const numBands = this.spectrumBands.length;
    const padding = 24;
    const usableW = w - padding * 2;
    const bandW = (usableW / numBands) * 0.72;
    const gap = (usableW / numBands) * 0.28;

    for (let i = 0; i < numBands; i++) {
      const x = padding + i * (bandW + gap);
      const bandVal = this.spectrumBands[i];
      const peakVal = this.spectrumPeaks[i];
      const barH = Math.max(2, bandVal * (h - 22));

      // Barra de espectro Electric Lime
      ctx.fillStyle = `rgba(212, 255, 0, ${0.80 * alpha})`;
      ctx.fillRect(x, h - barH - 4, bandW, barH);

      // Traço de pico RTA
      if (peakVal > 0.05) {
        const peakY = h - Math.max(2, peakVal * (h - 22)) - 4;
        ctx.fillStyle = `rgba(255, 255, 255, ${0.95 * alpha})`;
        ctx.fillRect(x, peakY - 1, bandW, 1.5);
      }
    }
  }
}

// ----------------------------------------------------------------------------
// 3. ROTARY VECTOR KNOB INTERACTION (-24dB a +12dB / 0.0 dB Reset)
// ----------------------------------------------------------------------------
class RotaryAudioKnob {
  constructor(elemId, arcId, needleId, numericId, onChange) {
    this.elem = document.getElementById(elemId);
    this.arc = document.getElementById(arcId);
    this.needle = document.getElementById(needleId);
    this.numeric = document.getElementById(numericId);
    this.onChange = onChange;

    this.min = parseFloat(this.elem.dataset.min || 0);
    this.max = parseFloat(this.elem.dataset.max || 200);
    this.val = parseFloat(this.elem.dataset.val || 100); // 100 = 0.0 dB

    this.isDragging = false;
    this.startY = 0;
    this.startVal = this.val;

    this.initEvents();
    this.updateVisuals();
  }

  initEvents() {
    this.elem.addEventListener('pointerdown', (e) => {
      this.isDragging = true;
      this.startY = e.clientY;
      this.startVal = this.val;
      this.elem.setPointerCapture(e.pointerId);
    });

    window.addEventListener('pointermove', (e) => {
      if (!this.isDragging) return;
      const deltaY = this.startY - e.clientY;
      const step = (this.max - this.min) / 130;
      let newVal = this.startVal + deltaY * step;
      newVal = Math.max(this.min, Math.min(this.max, newVal));
      this.val = newVal;
      this.updateVisuals();
      if (this.onChange) this.onChange(this.val);
    });

    window.addEventListener('pointerup', (e) => {
      if (this.isDragging) {
        this.isDragging = false;
        try { this.elem.releasePointerCapture(e.pointerId); } catch (_) {}
      }
    });

    this.elem.addEventListener('wheel', (e) => {
      e.preventDefault();
      const delta = e.deltaY < 0 ? 3 : -3;
      this.val = Math.max(this.min, Math.min(this.max, this.val + delta));
      this.updateVisuals();
      if (this.onChange) this.onChange(this.val);
    });

    this.elem.addEventListener('dblclick', () => {
      this.val = 100; // Reset para 0.0 dB
      this.updateVisuals();
      if (this.onChange) this.onChange(this.val);
    });
  }

  updateVisuals() {
    const norm = (this.val - this.min) / (this.max - this.min);
    const deg = -135 + norm * 270;

    if (this.needle) {
      this.needle.style.transform = `rotate(${deg}deg)`;
    }

    // Arc length para 270° no raio 22.5 (perímetro útil = 106.03)
    if (this.arc) {
      const arcLen = norm * 106.03;
      this.arc.style.strokeDasharray = `${arcLen.toFixed(1)} 141.4`;
    }

    if (this.numeric) {
      this.numeric.textContent = this.formatDb();
    }
  }

  formatDb() {
    if (this.val <= 2) return '-∞ dB';
    // 0..100 -> -24 dB a 0.0 dB
    // 100..200 -> 0.0 dB a +12.0 dB
    if (this.val === 100) return '0.0 dB';
    if (this.val < 100) {
      const db = ((this.val - 100) / 100) * 24.0;
      return `${db.toFixed(1)} dB`;
    } else {
      const db = ((this.val - 100) / 100) * 12.0;
      return `+${db.toFixed(1)} dB`;
    }
  }
}

// ----------------------------------------------------------------------------
// 4. MATRIX DE PRESETS & CONFIGURAÇÕES ASIO
// ----------------------------------------------------------------------------
const BUFFER_PRESETS = [
  { size: 64, ms: '1.33 ms', tag: 'Flex Ultra ★', isStar: true },
  { size: 96, ms: '2.00 ms', tag: 'Flex Ágil' },
  { size: 128, ms: '2.67 ms', tag: 'Equilibrado' },
  { size: 144, ms: '3.00 ms', tag: 'Nativo UM2 ★', isStar: true },
  { size: 192, ms: '4.00 ms', tag: 'Estúdio Pro' },
  { size: 256, ms: '5.33 ms', tag: 'Gravação DAW' },
  { size: 512, ms: '10.67 ms', tag: 'Mixagem' },
  { size: 1024, ms: '21.33 ms', tag: 'Seguro Master' }
];

let activeBufferSize = 64;
let selectedBufferSize = 64;
let activeEngineMode = 'shared'; // 'shared' ou 'exclusive'
let currentEngineMode = 'shared';
let isSoftwareMuted = false;

// ----------------------------------------------------------------------------
// 5. INICIALIZAÇÃO DE ELEMENTOS DOM
// ----------------------------------------------------------------------------

// OLED Elements
const oledBlockSize = document.getElementById('oledBlockSize');
const oledOutLatency = document.getElementById('oledOutLatency');
const oledRtlLatency = document.getElementById('oledRtlLatency');
const oledSampleRate = document.getElementById('oledSampleRate');
const oledDspCpu = document.getElementById('oledDspCpu');
const oledXruns = document.getElementById('oledXruns');

// Hardware Model Switch (UM2 vs UMC22)
const btnModelUm2 = document.getElementById('btnModelUm2');
const btnModelUmc22 = document.getElementById('btnModelUmc22');
const brandSubtitle = document.getElementById('brandSubtitle');
const ch1PreSpec = document.getElementById('ch1PreSpec');
const footerDriverName = document.getElementById('footerDriverName');

// Flashback 60s RAM Recorder
const flashbackStatusText = document.getElementById('flashbackStatusText');
const btnFlashback30 = document.getElementById('btnFlashback30');
const btnFlashback60 = document.getElementById('btnFlashback60');
const btnFlashbackFolder = document.getElementById('btnFlashbackFolder');

// v3 DSP Strip Elements
const adcClipBadge1 = document.getElementById('adcClipBadge1');
const adcClipBadge2 = document.getElementById('adcClipBadge2');
const btnAutoGain1 = document.getElementById('btnAutoGain1');
const btnAutoGain2 = document.getElementById('btnAutoGain2');
const grFill1 = document.getElementById('grFill1');
const grFill2 = document.getElementById('grFill2');
const lblGrDb2 = document.getElementById('lblGrDb2');
const sliderComp1 = document.getElementById('sliderComp1');
const valComp1 = document.getElementById('valComp1');
const sliderDeEss1 = document.getElementById('sliderDeEss1');
const valDeEss1 = document.getElementById('valDeEss1');
const sliderComp2 = document.getElementById('sliderComp2');
const valComp2 = document.getElementById('valComp2');
const btnCab2 = document.getElementById('btnCab2');

// v3 Per-App Loopback & 808 Key Finder Elements
const loopbackPidRow = document.getElementById('loopbackPidRow');
const selectLoopPid = document.getElementById('selectLoopPid');
const btnRefreshPids = document.getElementById('btnRefreshPids');
const lblLoopTunerNote = document.getElementById('lblLoopTunerNote');
const btnLoopDetect808 = document.getElementById('btnLoopDetect808');
let currentLoopbackProcessMode = 0;

// v3 Master Mix Check & PDC Elements
const btnDspPrint = document.getElementById('btnDspPrint');
const btnDspMonOnly = document.getElementById('btnDspMonOnly');
const btnMonSub = document.getElementById('btnMonSub');
const btnMonPhone = document.getElementById('btnMonPhone');
const btnMonXfeed = document.getElementById('btnMonXfeed');
const lblTotalPdc = document.getElementById('lblTotalPdc');
const sliderPdcOffset = document.getElementById('sliderPdcOffset');
const valPdcOffset = document.getElementById('valPdcOffset');

// Status & Sync Badges
const asioBadge = document.getElementById('asioBadge');
const asioStatusText = document.getElementById('asioStatusText');
const syncStatusCapsule = document.getElementById('syncStatusCapsule');
const syncStatusText = document.getElementById('syncStatusText');

// Engine Mode Cards
const cardModeShared = document.getElementById('cardModeShared');
const cardModeExclusive = document.getElementById('cardModeExclusive');

// Buffer Matrix & Sync
const bufferMatrixGrid = document.getElementById('bufferMatrixGrid');
const lblSelectedSpec = document.getElementById('lblSelectedSpec');
const btnApplySync = document.getElementById('btnApplySync');
const btnApplyText = document.getElementById('btnApplyText');

// Hardware Deck & Mute
const hwLedSig1 = document.getElementById('hwLedSig1');
const hwLedClip1 = document.getElementById('hwLedClip1');
const hwLedSig2 = document.getElementById('hwLedSig2');
const hwLedClip2 = document.getElementById('hwLedClip2');
const hwLedPhantom = document.getElementById('hwLedPhantom');
const btnGlobalMute = document.getElementById('btnGlobalMute');
const fastMuteLed = document.getElementById('fastMuteLed');
const fastMuteLabel = document.getElementById('fastMuteLabel');

// Toast
const studioToast = document.getElementById('studioToast');
const toastMessageText = document.getElementById('toastMessageText');

// Instancia Medidores VU Verticais
const vuCh1 = new VerticalVuMeter(document.getElementById('vuCanvas1'), 'clipLed1', 'dbBox1');
const vuCh2 = new VerticalVuMeter(document.getElementById('vuCanvas2'), 'clipLed2', 'dbBox2');
const vuLoopL = new VerticalVuMeter(document.getElementById('vuCanvasLoopL'), 'clipLedLoopL');
const vuLoopR = new VerticalVuMeter(document.getElementById('vuCanvasLoopR'), 'clipLedLoopR');
const vuOutL = new VerticalVuMeter(document.getElementById('vuCanvasOutL'), 'clipLedOutL');
const vuOutR = new VerticalVuMeter(document.getElementById('vuCanvasOutR'), 'clipLedOutR');
const dbBoxLoop = document.getElementById('dbBoxLoop');
const dbBoxOut = document.getElementById('dbBoxOut');

// Instancia Osciloscópio / RTA
const scopeMonitor = new DualSignalMonitor(document.getElementById('scopeCanvas'));

// Conversão de Knob para Ganho Linear
function knobValToGain(val) {
  if (val <= 2) return 0.0;
  if (val === 100) return 1.0;
  if (val < 100) {
    const db = ((val - 100) / 100) * 24.0;
    return Math.pow(10, db / 20.0);
  } else {
    const db = ((val - 100) / 100) * 12.0;
    return Math.pow(10, db / 20.0);
  }
}

function gainToKnobVal(gain) {
  if (gain <= 0.0001) return 0;
  const db = 20 * Math.log10(gain);
  if (db <= -24) return 0;
  if (db < 0) {
    return Math.round(100 + (db / 24.0) * 100);
  } else {
    return Math.round(100 + Math.min(100, (db / 12.0) * 100));
  }
}

let trimDebounceTimer1 = null;
let trimDebounceTimer2 = null;
let trimDebounceTimerMaster = null;

function sendDspCh1(updates) {
  fetch('/api/dsp/ch1', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(updates)
  }).catch(() => {});
}

function sendDspCh2(updates) {
  fetch('/api/dsp/ch2', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(updates)
  }).catch(() => {});
}

function sendDspLoopback(updates) {
  fetch('/api/dsp/loopback', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(updates)
  }).catch(() => {});
}

function sendDspMaster(updates) {
  fetch('/api/dsp/master', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(updates)
  }).catch(() => {});
}

function sendScopeSource(srcInt) {
  fetch('/api/scope/source', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ source: srcInt })
  }).catch(() => {});
}

// Instancia Knobs Rotativos
const knobTrim1 = new RotaryAudioKnob('knobTrim1', 'arcTrim1', 'needleTrim1', 'valTrim1', (val) => {
  clearTimeout(trimDebounceTimer1);
  trimDebounceTimer1 = setTimeout(() => {
    sendDspCh1({ trim: knobValToGain(val) });
  }, 20);
});

const knobTrim2 = new RotaryAudioKnob('knobTrim2', 'arcTrim2', 'needleTrim2', 'valTrim2', (val) => {
  clearTimeout(trimDebounceTimer2);
  trimDebounceTimer2 = setTimeout(() => {
    sendDspCh2({ trim: knobValToGain(val) });
  }, 20);
});

const knobLoopGain = new RotaryAudioKnob('knobLoopGain', 'arcLoopGain', 'needleLoopGain', 'valLoopGain', (val) => {
  sendLoopbackUpdate();
});

const knobMasterOut = new RotaryAudioKnob('knobMasterOut', 'arcMasterOut', 'needleMasterOut', 'valMasterOut', (val) => {
  clearTimeout(trimDebounceTimerMaster);
  trimDebounceTimerMaster = setTimeout(() => {
    sendDspMaster({ trim: knobValToGain(val) });
  }, 20);
});

// ----------------------------------------------------------------------------
// 6. RENDER DA MATRIZ TÁTICA DE BUFFER (GRID 4x2)
// ----------------------------------------------------------------------------
function renderBufferMatrix() {
  bufferMatrixGrid.innerHTML = '';

  BUFFER_PRESETS.forEach(p => {
    const cell = document.createElement('div');
    cell.className = `btn-matrix-cell${p.size === selectedBufferSize ? ' active' : ''}`;
    cell.dataset.size = p.size;

    cell.innerHTML = `
      <span class="cell-samples">${p.size} smp</span>
      <span class="cell-ms">${p.ms}</span>
      <span class="cell-tag">${p.tag}</span>
    `;

    cell.addEventListener('click', () => {
      selectedBufferSize = p.size;
      updateMatrixVisualSelection();
      updateSyncStatus();
    });

    bufferMatrixGrid.appendChild(cell);
  });
}

function updateMatrixVisualSelection() {
  document.querySelectorAll('.btn-matrix-cell').forEach(cell => {
    const sz = parseInt(cell.dataset.size, 10);
    cell.classList.toggle('active', sz === selectedBufferSize);
  });
}

// ----------------------------------------------------------------------------
// 7. SELEÇÃO DE ARQUITETURA DO MOTOR ASIO (FLEX vs PRO EXCLUSIVE)
// ----------------------------------------------------------------------------
function setEngineMode(mode) {
  currentEngineMode = (mode === 'exclusive') ? 'exclusive' : 'shared';

  if (currentEngineMode === 'exclusive') {
    cardModeExclusive.classList.add('active');
    cardModeExclusive.querySelector('.mode-led-dot').classList.add('active');
    cardModeShared.classList.remove('active');
    cardModeShared.querySelector('.mode-led-dot').classList.remove('active');
  } else {
    cardModeShared.classList.add('active');
    cardModeShared.querySelector('.mode-led-dot').classList.add('active');
    cardModeExclusive.classList.remove('active');
    cardModeExclusive.querySelector('.mode-led-dot').classList.remove('active');
  }

  updateMatrixVisualSelection();
  updateSyncStatus();
}

cardModeShared.addEventListener('click', () => setEngineMode('shared'));
cardModeExclusive.addEventListener('click', () => setEngineMode('exclusive'));

// ----------------------------------------------------------------------------
// 8. ATUALIZAÇÃO DA BARRA DE SINCRONIZAÇÃO E STATUS DE PENDÊNCIA
// ----------------------------------------------------------------------------
function updateSyncStatus() {
  const ms = (selectedBufferSize / 48.0).toFixed(2);
  const modeLabel = (currentEngineMode === 'shared') ? 'FLEX' : 'PRO EXCLUSIVO';
  lblSelectedSpec.textContent = `${selectedBufferSize} SMP (${ms} ms) • ${modeLabel}`;

  const isChanged = (selectedBufferSize !== activeBufferSize) || (currentEngineMode !== activeEngineMode);

  if (isChanged) {
    syncStatusCapsule.className = 'sync-badge-capsule pending';
    syncStatusText.textContent = '● PENDENTE DE APLICAR';
    btnApplySync.classList.add('pulse-amber');
  } else {
    syncStatusCapsule.className = 'sync-badge-capsule synced';
    syncStatusText.textContent = 'SINCRONIZADO';
    btnApplySync.classList.remove('pulse-amber');
  }
}

// ----------------------------------------------------------------------------
// 9. APLICAR CONFIGURAÇÕES VIA kAsioResetRequest
// ----------------------------------------------------------------------------
btnApplySync.addEventListener('click', async () => {
  btnApplySync.disabled = true;
  btnApplyText.textContent = 'SINCRONIZANDO COM FL STUDIO...';

  try {
    const res = await fetch('/api/buffer/apply', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        bufferSize: selectedBufferSize,
        mode: currentEngineMode
      })
    });

    const data = await res.json();
    if (data.status === 'ok') {
      activeBufferSize = selectedBufferSize;
      activeEngineMode = currentEngineMode;
      updateSyncStatus();
      showToast(`Buffer sincronizado: ${selectedBufferSize} smp • Pulso kAsioResetRequest enviado ao FL Studio!`, true);
    } else {
      showToast('Falha ao aplicar alterações no driver ASIO', false);
    }
  } catch (err) {
    showToast('Erro de comunicação com o backend KaxWav', false);
  } finally {
    btnApplySync.disabled = false;
    btnApplyText.textContent = '↻ APLICAR NO FL STUDIO';
  }
});

// ----------------------------------------------------------------------------
// 10. BOTÕES TÁTICOS (PHASE, HPF, MUTE, DAW SEND, DIM)
// ----------------------------------------------------------------------------

// Channel 1 Tactile Buttons
const btnPhase1 = document.getElementById('btnPhase1');
const btnHpf1 = document.getElementById('btnHpf1');
const btnGate1 = document.getElementById('btnGate1');
const btnMute1 = document.getElementById('btnMute1');

btnPhase1.addEventListener('click', () => {
  btnPhase1.classList.toggle('active-led');
  const active = btnPhase1.classList.contains('active-led');
  sendDspCh1({ phase: active });
  showToast(active ? 'CH 1: Inversão de Fase Ativa (Ø)' : 'CH 1: Fase Normal', true);
});

btnHpf1.addEventListener('click', () => {
  btnHpf1.classList.toggle('active-led');
  const active = btnHpf1.classList.contains('active-led');
  sendDspCh1({ hpf: active });
  showToast(active ? 'CH 1: Filtro HPF 80Hz Ativo (Low Cut)' : 'CH 1: HPF em Bypass', true);
});

btnGate1.addEventListener('click', () => {
  btnGate1.classList.toggle('active-led');
  const active = btnGate1.classList.contains('active-led');
  sendDspCh1({ gate: active });
  showToast(active ? 'CH 1: Noise Gate Ativo (-52dBFS / 20ms Hold)' : 'CH 1: Noise Gate em Bypass', true);
});

btnMute1.addEventListener('click', () => {
  btnMute1.classList.toggle('muted');
  const muted = btnMute1.classList.contains('muted');
  sendDspCh1({ mute: muted });
  showToast(muted ? 'CH 1: Microfone Mutado' : 'CH 1: Microfone Ativo', !muted);
});

// Channel 2 Tactile Buttons
const btnPhase2 = document.getElementById('btnPhase2');
const btnPad2 = document.getElementById('btnPad2');
const btnGate2 = document.getElementById('btnGate2');
const btnMute2 = document.getElementById('btnMute2');

btnPhase2.addEventListener('click', () => {
  btnPhase2.classList.toggle('active-led');
  const active = btnPhase2.classList.contains('active-led');
  sendDspCh2({ phase: active });
  showToast(active ? 'CH 2: Inversão de Fase Ativa (Ø)' : 'CH 2: Fase Normal', true);
});

btnPad2.addEventListener('click', () => {
  btnPad2.classList.toggle('active-led');
  const active = btnPad2.classList.contains('active-led');
  sendDspCh2({ pad: active });
  showToast(active ? 'CH 2: Atenuador PAD -10dB Ativo' : 'CH 2: PAD em Bypass (0dB)', true);
});

btnGate2.addEventListener('click', () => {
  btnGate2.classList.toggle('active-led');
  const active = btnGate2.classList.contains('active-led');
  sendDspCh2({ gate: active });
  showToast(active ? 'CH 2: Noise Gate Ativo (-52dBFS / 20ms Hold)' : 'CH 2: Noise Gate em Bypass', true);
});

btnMute2.addEventListener('click', () => {
  btnMute2.classList.toggle('muted');
  const muted = btnMute2.classList.contains('muted');
  sendDspCh2({ mute: muted });
  showToast(muted ? 'CH 2: Instrumento Mutado' : 'CH 2: Instrumento Ativo', !muted);
});

// Loopback Controls
const btnDawSend = document.getElementById('btnDawSend');
const btnBroadcastMix = document.getElementById('btnBroadcastMix');
const btnLoopMono = document.getElementById('btnLoopMono');

btnDawSend.addEventListener('click', () => {
  btnDawSend.classList.toggle('active');
  sendLoopbackUpdate();
  showToast(btnDawSend.classList.contains('active') ? 'Loopback enviado para CH 3-4 do DAW' : 'Loopback desativado', true);
});

btnBroadcastMix.addEventListener('click', () => {
  btnBroadcastMix.classList.toggle('active');
  sendLoopbackUpdate();
  const bcast = btnBroadcastMix.classList.contains('active');
  showToast(bcast ? 'Broadcast Mix Ativo: PC Audio + Mic CH1 + Inst CH2' : 'Modo PC Only Ativo', true);
});

btnLoopMono.addEventListener('click', () => {
  btnLoopMono.classList.toggle('active-led');
  const mono = btnLoopMono.classList.contains('active-led');
  sendLoopbackUpdate();
  showToast(mono ? 'Loopback somado em Mono (L+R)' : 'Loopback em Estéreo', true);
});

function sendLoopbackUpdate() {
  const isEnabled = btnDawSend.classList.contains('active');
  const gain = knobLoopGain.val / 100.0;
  const mono = btnLoopMono.classList.contains('active-led');
  const bcast = btnBroadcastMix.classList.contains('active');
  const pid = selectLoopPid ? parseInt(selectLoopPid.value || '0', 10) : 0;
  sendDspLoopback({
    enabled: isEnabled,
    gain: gain,
    mono: mono,
    broadcastMix: bcast,
    processMode: currentLoopbackProcessMode,
    targetPid: pid
  });
}

// ----------------------------------------------------------------------------
// 10B. CONTROLES v3.0 — UNISON STRIPS, AUTO-GAIN, FLASHBACK, PER-APP LOOPBACK & MIX CHECK
// ----------------------------------------------------------------------------

// 1. Hardware Model Switch (UM2 vs UMC22)
function setHardwareModelVisuals(modelInt) {
  const isUmc22 = (modelInt === 1);
  if (btnModelUm2) btnModelUm2.classList.toggle('active', !isUmc22);
  if (btnModelUmc22) btnModelUmc22.classList.toggle('active', isUmc22);
  if (brandSubtitle) {
    brandSubtitle.textContent = isUmc22
      ? 'U-PHORIA UMC22 • MIDAS PRE • TI PCM2902'
      : 'U-PHORIA UM2 • XENYX PRE • TI PCM2902';
  }
  if (ch1PreSpec) {
    ch1PreSpec.textContent = isUmc22 ? 'MIDAS PRE • +48V' : 'XENYX PRE • +48V';
  }
  if (footerDriverName) {
    footerDriverName.textContent = isUmc22
      ? 'BehringerUM2ASIODriver.dll (Perfil UMC22 MIDAS Ativo)'
      : 'BehringerUM2ASIODriver.dll (Perfil UM2 XENYX Ativo)';
  }
}

if (btnModelUm2) {
  btnModelUm2.addEventListener('click', () => {
    setHardwareModelVisuals(0);
    sendDspMaster({ hardwareModel: 0 });
    showToast('Perfil Hardware: Behringer U-Phoria UM2 (XENYX Mic Preamp)', true);
  });
}

if (btnModelUmc22) {
  btnModelUmc22.addEventListener('click', () => {
    setHardwareModelVisuals(1);
    sendDspMaster({ hardwareModel: 1 });
    showToast('Perfil Hardware: Behringer U-Phoria UMC22 (MIDAS Mic Preamp)', true);
  });
}

// 2. Flashback 60s RAM Recorder
async function saveFlashbackAudio(seconds) {
  try {
    const resp = await fetch('/api/flashback/save', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ seconds })
    });
    const data = await resp.json();
    if (data.status === 'ok') {
      showToast(`Flashback salvo (${data.secondsSaved}s): ${data.filename}`, true);
    } else {
      showToast(data.message || 'Buffer Flashback ainda vazio', false);
    }
  } catch (e) {
    showToast('Erro ao salvar Flashback WAV', false);
  }
}

if (btnFlashback30) btnFlashback30.addEventListener('click', () => saveFlashbackAudio(30));
if (btnFlashback60) btnFlashback60.addEventListener('click', () => saveFlashbackAudio(60));
if (btnFlashbackFolder) {
  btnFlashbackFolder.addEventListener('click', () => {
    fetch('/api/flashback/open-folder', { method: 'POST' }).catch(() => {});
  });
}

// 3. Unison Preamp Voicing Selectors (CH1 & CH2)
const PREAMP_NAMES = ['CLEAN (Flat)', '73 TRAP (+Air & Warmth)', 'TUBE (Triode Harmonics)'];
document.querySelectorAll('.btn-strip-mode[data-ch]').forEach(btn => {
  btn.addEventListener('click', () => {
    const ch = parseInt(btn.dataset.ch, 10);
    const mode = parseInt(btn.dataset.preamp, 10);
    document.querySelectorAll(`.btn-strip-mode[data-ch="${ch}"]`).forEach(b => {
      b.classList.toggle('active', parseInt(b.dataset.preamp, 10) === mode);
    });
    if (ch === 1) {
      sendDspCh1({ preampMode: mode });
      showToast(`CH 1 Preamp: ${PREAMP_NAMES[mode] || 'CLEAN'}`, true);
    } else if (ch === 2) {
      sendDspCh2({ preampMode: mode });
      showToast(`CH 2 Preamp: ${PREAMP_NAMES[mode] || 'CLEAN'}`, true);
    }
  });
});

// 4. Smart Auto-Gain Assistant (3s Peak Calibration)
async function triggerSmartAutoGain(channel, btnElem, knobObj) {
  if (!btnElem || btnElem.classList.contains('calibrating')) return;
  btnElem.classList.add('calibrating');
  const origText = btnElem.textContent;
  btnElem.textContent = 'OUVINDO 3S...';
  showToast(`CH ${channel}: Cantando/tocando por 3 segundos para calibrar headroom (-12 dBFS)...`, true);

  try {
    const resp = await fetch('/api/auto-gain', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ channel, targetDb: -12.0 })
    });
    const data = await resp.json();
    if (data.status === 'ok') {
      knobObj.val = gainToKnobVal(data.newTrim);
      knobObj.updateVisuals();
      showToast(`CH ${channel} Auto-Gain: Pico ${data.measuredPeakDb} dBFS → Trim ajustado para ${data.newTrimDb >= 0 ? '+' : ''}${data.newTrimDb} dB`, true);
    } else {
      showToast(data.message || `CH ${channel}: Sinal muito baixo para calibrar Auto-Gain`, false);
    }
  } catch (e) {
    showToast(`Erro ao calibrar Auto-Gain no CH ${channel}`, false);
  } finally {
    btnElem.classList.remove('calibrating');
    btnElem.textContent = origText;
  }
}

if (btnAutoGain1) btnAutoGain1.addEventListener('click', () => triggerSmartAutoGain(1, btnAutoGain1, knobTrim1));
if (btnAutoGain2) btnAutoGain2.addEventListener('click', () => triggerSmartAutoGain(2, btnAutoGain2, knobTrim2));

// 5. 1176 Compressor, De-Esser & Cab Sim 4x12
if (sliderComp1) {
  sliderComp1.addEventListener('input', (e) => {
    const pct = parseInt(e.target.value, 10) || 0;
    if (valComp1) valComp1.textContent = `${pct}%`;
    sendDspCh1({ compAmount: pct / 100.0 });
  });
}

if (sliderDeEss1) {
  sliderDeEss1.addEventListener('input', (e) => {
    const pct = parseInt(e.target.value, 10) || 0;
    if (valDeEss1) valDeEss1.textContent = `${pct}%`;
    sendDspCh1({ deEsserAmount: pct / 100.0 });
  });
}

if (sliderComp2) {
  sliderComp2.addEventListener('input', (e) => {
    const pct = parseInt(e.target.value, 10) || 0;
    if (valComp2) valComp2.textContent = `${pct}%`;
    sendDspCh2({ compAmount: pct / 100.0 });
  });
}

if (btnCab2) {
  btnCab2.addEventListener('click', () => {
    btnCab2.classList.toggle('active-led');
    const active = btnCab2.classList.contains('active-led');
    sendDspCh2({ cabSim: active });
    showToast(active ? 'CH 2: Simulador de Gabinete 4x12 Ativo (Zero-Latency)' : 'CH 2: Gabinete 4x12 em Bypass (DI Puro)', true);
  });
}

// 6. Per-App Loopback (Win10 2004+ / Win11) & 808 Key Finder
function updateLoopbackPidRowState(mode) {
  if (loopbackPidRow) {
    loopbackPidRow.style.display = 'flex';
    loopbackPidRow.style.opacity = mode > 0 ? '1' : '0.55';
  }
  if (selectLoopPid) {
    selectLoopPid.disabled = (mode === 0);
    if (mode === 0 && selectLoopPid.options.length > 0) {
      selectLoopPid.options[0].textContent = 'Mix Global Windows (PC)';
      selectLoopPid.value = '0';
    } else if (mode > 0 && selectLoopPid.options.length > 0 && selectLoopPid.options[0].value === '0') {
      selectLoopPid.options[0].textContent = 'Selecione o App (PID)...';
    }
  }
}

async function refreshAudioProcesses(selectedPid = null) {
  if (!selectLoopPid) return;
  try {
    const resp = await fetch('/api/audio-processes');
    const data = await resp.json();
    const currentVal = selectedPid !== null ? String(selectedPid) : selectLoopPid.value;
    const zeroLabel = currentLoopbackProcessMode === 0 ? 'Mix Global Windows (PC)' : 'Selecione o App (PID)...';
    selectLoopPid.innerHTML = `<option value="0">${zeroLabel}</option>`;
    (data.processes || []).forEach(proc => {
      const opt = document.createElement('option');
      opt.value = String(proc.pid);
      opt.textContent = `${proc.label} (PID ${proc.pid})`;
      selectLoopPid.appendChild(opt);
    });
    if (currentVal && currentVal !== '0') {
      selectLoopPid.value = currentVal;
    }
  } catch (e) {}
}

document.querySelectorAll('.btn-strip-mode[data-loopmode]').forEach(btn => {
  btn.addEventListener('click', () => {
    const mode = parseInt(btn.dataset.loopmode, 10) || 0;
    currentLoopbackProcessMode = mode;
    document.querySelectorAll('.btn-strip-mode[data-loopmode]').forEach(b => {
      b.classList.toggle('active', parseInt(b.dataset.loopmode, 10) === mode);
    });
    updateLoopbackPidRowState(mode);
    if (mode > 0) {
      refreshAudioProcesses();
    }
    sendLoopbackUpdate();
    const modeLabels = ['Loopback: Todo o PC', 'Loopback: Capturar Apenas App Selecionado', 'Loopback: Excluir App Selecionado (ex: Discord)'];
    showToast(modeLabels[mode] || modeLabels[0], true);
  });
});

if (btnRefreshPids) {
  btnRefreshPids.addEventListener('click', () => {
    refreshAudioProcesses();
    showToast('Lista de processos de áudio atualizada', true);
  });
}

if (selectLoopPid) {
  selectLoopPid.addEventListener('change', () => {
    sendLoopbackUpdate();
  });
}

if (btnLoopDetect808) {
  btnLoopDetect808.addEventListener('click', () => {
    if (btnDawSend && !btnDawSend.classList.contains('active-led')) {
      btnDawSend.classList.add('active-led');
      sendLoopbackUpdate();
    }
    setMonitorMode('tuner');
    setTunerSource(3);
    showToast('Afinador YIN focado no Loopback CH 3-4 (Detector de Nota de 808 / Sample)', true);
  });
}

// ADR 0004: DSP na DAW por canal (padrão desligado = DAW bit-perfect)
[1, 2].forEach((ch) => {
  const btn = document.getElementById(`btnDawDsp${ch}`);
  if (!btn) return;
  btn.addEventListener('click', async () => {
    const enabled = !btn.classList.contains('active-led');
    try {
      const r = await fetch('/api/dsp/daw', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ channel: ch, enabled })
      });
      if (!r.ok) throw new Error(r.status);
      btn.classList.toggle('active-led', enabled);
      showToast(enabled
        ? `CH ${ch}: a DAW grava com o DSP do canal`
        : `CH ${ch}: a DAW grava a entrada limpa (bit-perfect)`, true);
    } catch (e) {
      showToast(`Falha ao mudar DSP na DAW do CH ${ch}`, false);
    }
  });
});

// 7. Master DSP Print vs Monitor Only, Mix Check Matrix & PDC Calibration
function setDspMonitorOnlyVisuals(monOnly) {
  if (btnDspPrint) btnDspPrint.classList.toggle('active', !monOnly);
  if (btnDspMonOnly) btnDspMonOnly.classList.toggle('active', !!monOnly);
}

if (btnDspPrint) {
  btnDspPrint.addEventListener('click', () => {
    setDspMonitorOnlyVisuals(false);
    sendDspMaster({ dspMonitorOnly: false });
    showToast('PRINT DAW: fones sem o DSP extra. O que vai para a DAW é escolhido por canal (DSP NA DAW)', true);
  });
}

if (btnDspMonOnly) {
  btnDspMonOnly.addEventListener('click', () => {
    setDspMonitorOnlyVisuals(true);
    sendDspMaster({ dspMonitorOnly: true });
    showToast('MON ONLY: você ouve o DSP nos fones. O que vai para a DAW é escolhido por canal (DSP NA DAW)', true);
  });
}

if (btnMonSub) {
  btnMonSub.addEventListener('click', () => {
    btnMonSub.classList.toggle('active-led');
    const active = btnMonSub.classList.contains('active-led');
    sendDspMaster({ monSubCheck: active });
    showToast(active ? 'Mix Check: SUB 808 LPF (<120Hz) Ativo' : 'Mix Check: SUB 808 Desligado', true);
  });
}

if (btnMonPhone) {
  btnMonPhone.addEventListener('click', () => {
    btnMonPhone.classList.toggle('active-led');
    const active = btnMonPhone.classList.contains('active-led');
    sendDspMaster({ monPhoneSim: active });
    showToast(active ? 'Mix Check: Simulador de Speaker de Celular (300Hz-3.8kHz) Ativo' : 'Mix Check: Phone Speaker Desligado', true);
  });
}

if (btnMonXfeed) {
  btnMonXfeed.addEventListener('click', () => {
    btnMonXfeed.classList.toggle('active-led');
    const active = btnMonXfeed.classList.contains('active-led');
    sendDspMaster({ monCrossfeed: active });
    showToast(active ? 'Mix Check: Studio Headphone Crossfeed Ativo' : 'Mix Check: Crossfeed Desligado', true);
  });
}

if (sliderPdcOffset) {
  sliderPdcOffset.addEventListener('input', (e) => {
    const val = parseInt(e.target.value, 10) || 0;
    if (valPdcOffset) valPdcOffset.textContent = `${val >= 0 ? '+' : ''}${val}`;
    sendDspMaster({ pdcOffsetSamples: val });
  });
}

// Master Controls
const btnDim = document.getElementById('btnDim');
const btnMasterMono = document.getElementById('btnMasterMono');
const btnMuteMaster = document.getElementById('btnMuteMaster');

btnDim.addEventListener('click', () => {
  btnDim.classList.toggle('active-led');
  const dim = btnDim.classList.contains('active-led');
  sendDspMaster({ dim: dim });
  showToast(dim ? 'Master Monitor: DIM -14dB Ativo' : 'Master: Nível Pleno (0dB)', true);
});

btnMasterMono.addEventListener('click', () => {
  btnMasterMono.classList.toggle('active-led');
  const mono = btnMasterMono.classList.contains('active-led');
  sendDspMaster({ mono: mono });
  showToast(mono ? 'Master Monitor: Mono Check Ativo (L+R)' : 'Master: Estéreo', true);
});

btnMuteMaster.addEventListener('click', () => {
  btnMuteMaster.classList.toggle('muted');
  const muted = btnMuteMaster.classList.contains('muted');
  sendDspMaster({ mute: muted });
  showToast(muted ? 'Saída Master Mutada' : 'Saída Master Ativa', !muted);
});

// Hardware Deck: Fast Mic Mute Button & +48V
btnGlobalMute.addEventListener('click', () => {
  isSoftwareMuted = !isSoftwareMuted;
  setSoftwareMuteState(isSoftwareMuted);
  fetch('/api/mute', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ mute: isSoftwareMuted })
  }).catch(() => {});
});

function setSoftwareMuteState(muted) {
  isSoftwareMuted = muted;
  if (muted) {
    fastMuteLed.className = 'fast-mute-led led-red';
    fastMuteLabel.textContent = '● MICROFONE MUTADO (SILENCIOSO)';
    btnGlobalMute.style.borderColor = 'var(--led-red)';
    showToast('Microfone mutado no driver ASIO', false);
  } else {
    fastMuteLed.className = 'fast-mute-led led-green';
    fastMuteLabel.textContent = '● MICROFONE ABERTO (ATIVO NO FL STUDIO)';
    btnGlobalMute.style.borderColor = '';
    showToast('Microfone aberto e pronto para gravação', true);
  }
}

if (hwLedPhantom) {
  hwLedPhantom.addEventListener('click', () => {
    hwLedPhantom.classList.toggle('on');
    showToast(hwLedPhantom.classList.contains('on') ? 'Alimentação Phantom +48V Ativa' : 'Phantom +48V Desligada', true);
  });
}

// ----------------------------------------------------------------------------
// 11. CONTROLES DO VISOR DUAL (WAVEFORM / SPECTRUM RTA / DUAL / TUNER & FONTE)
// ----------------------------------------------------------------------------
const btnModeWave = document.getElementById('btnModeWave');
const btnModeRta = document.getElementById('btnModeRta');
const btnModeDual = document.getElementById('btnModeDual');
const btnModeTuner = document.getElementById('btnModeTuner');
const scopeSourceSelector = document.getElementById('scopeSourceSelector');
const tunerSourceSelector = document.getElementById('tunerSourceSelector');
const graticuleOverlay = document.getElementById('graticuleOverlay');

function setMonitorMode(mode) {
  scopeMonitor.mode = mode;
  btnModeWave.classList.toggle('active', mode === 'wave');
  btnModeRta.classList.toggle('active', mode === 'rta');
  btnModeDual.classList.toggle('active', mode === 'dual');
  if (btnModeTuner) btnModeTuner.classList.toggle('active', mode === 'tuner');
  if (scopeSourceSelector) {
    scopeSourceSelector.style.display = (mode === 'tuner') ? 'none' : 'flex';
  }
  if (tunerSourceSelector) {
    tunerSourceSelector.style.display = (mode === 'tuner') ? 'flex' : 'none';
  }
  if (graticuleOverlay) {
    graticuleOverlay.style.opacity = (mode === 'tuner') ? '0.12' : '1.0';
  }
}

function setTunerSource(srcInt) {
  document.querySelectorAll('.btn-tuner-src-pill').forEach(b => {
    b.classList.toggle('active', parseInt(b.dataset.tunersrc, 10) === srcInt);
  });
  fetch('/api/tuner/source', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ source: srcInt })
  }).catch(() => {});
}

btnModeWave.addEventListener('click', () => setMonitorMode('wave'));
btnModeRta.addEventListener('click', () => setMonitorMode('rta'));
btnModeDual.addEventListener('click', () => setMonitorMode('dual'));
if (btnModeTuner) btnModeTuner.addEventListener('click', () => setMonitorMode('tuner'));

document.querySelectorAll('.btn-tuner-src-pill').forEach(btn => {
  btn.addEventListener('click', () => {
    const srcInt = parseInt(btn.dataset.tunersrc, 10) || 0;
    setTunerSource(srcInt);
  });
});

const SOURCE_MAP = { 'master': 0, 'ch1': 1, 'ch2': 2, 'loopback': 3 };

document.querySelectorAll('.btn-source-pill').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.btn-source-pill').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
    const srcKey = btn.dataset.source || 'master';
    scopeMonitor.source = srcKey;
    sendScopeSource(SOURCE_MAP[srcKey] ?? 0);
  });
});

// Loop de animação do osciloscópio (60 FPS)
function monitorAnimationLoop() {
  scopeMonitor.draw();
  requestAnimationFrame(monitorAnimationLoop);
}
requestAnimationFrame(monitorAnimationLoop);

// ----------------------------------------------------------------------------
// 12. TOAST NOTIFICATIONS DE ESTÚDIO
// ----------------------------------------------------------------------------
let toastTimer = null;
function showToast(msg, isSuccess = true) {
  toastMessageText.textContent = msg;
  studioToast.style.borderColor = isSuccess ? 'var(--led-emerald)' : 'var(--led-red)';
  studioToast.classList.add('show');

  if (toastTimer) clearTimeout(toastTimer);
  toastTimer = setTimeout(() => {
    studioToast.classList.remove('show');
  }, 3200);
}

// ----------------------------------------------------------------------------
// 13. WEBSOCKET TELEMETRIA DE ALTA PRECISÃO (/ws/meters)
// ----------------------------------------------------------------------------
let ws = null;
let reconnectTimer = null;
let isInitialSyncDone = false;

function connectWebSocket() {
  const loc = window.location;
  const wsUrl = `ws://${loc.hostname || '127.0.0.1'}:${loc.port || '8930'}/ws/meters`;
  ws = new WebSocket(wsUrl);

  ws.onopen = () => {
    console.log('[KaxWav] WebSocket conectado ao motor de áudio ASIO na porta 8930.');
  };

  ws.onmessage = (event) => {
    try {
      const data = JSON.parse(event.data);

      const in1 = data.peakIn1 || 0.0;
      const in2 = data.peakIn2 || 0.0;
      const loopL = data.peakLoopbackL || 0.0;
      const loopR = data.peakLoopbackR || 0.0;
      const outL = data.peakOut1 || 0.0;
      const outR = data.peakOut2 || 0.0;

      // 1. Atualiza Medidores VU Verticais
      vuCh1.update(in1);
      vuCh2.update(in2);
      vuLoopL.update(loopL);
      vuLoopR.update(loopR);
      vuOutL.update(outL);
      vuOutR.update(outR);

      // Atualiza Leituras Digitais Estéreo
      if (dbBoxLoop) {
        dbBoxLoop.textContent = vuLoopL.linearToDb(Math.max(loopL, loopR));
      }
      if (dbBoxOut) {
        dbBoxOut.textContent = vuOutL.linearToDb(Math.max(outL, outR));
      }

      // 2. Alimenta Monitor de Sinal com o canal selecionado e buffer de onda real
      let selectedSignal = Math.max(outL, outR);
      if (scopeMonitor.source === 'ch1') selectedSignal = in1;
      else if (scopeMonitor.source === 'ch2') selectedSignal = in2;
      else if (scopeMonitor.source === 'loopback') selectedSignal = Math.max(loopL, loopR);

      scopeMonitor.setAudioData(data.scopeBuffer, selectedSignal);
      scopeMonitor.setTunerData({
        freq: data.tunerFreqHz || 0.0,
        note: data.tunerMidiNote || 0,
        noteName: data.tunerNoteName || '--',
        cents: data.tunerCents || 0.0,
        source: typeof data.tunerSource === 'number' ? data.tunerSource : 1
      });

      // 2B. Telemetria v3: ADC Clip Guard, Gain Reduction & 808 Note Badge
      const gr1 = Math.min(24.0, (data.ch1CompGrDb || 0.0) + (data.ch1DeEsserGrDb || 0.0));
      const gr2 = Math.min(24.0, data.ch2CompGrDb || 0.0);

      if (grFill1) {
        grFill1.style.height = `${Math.min(100, (gr1 / 18.0) * 100).toFixed(1)}%`;
      }
      if (grFill2) {
        grFill2.style.height = `${Math.min(100, (gr2 / 18.0) * 100).toFixed(1)}%`;
        if (lblGrDb2) lblGrDb2.textContent = `-${gr2.toFixed(1)}dB`;
      }

      if (adcClipBadge1) {
        if (data.ch1AdcClipped) {
          adcClipBadge1.textContent = 'ADC CLIP';
          adcClipBadge1.classList.add('clipped');
          adcClipBadge1.classList.remove('gr-active');
        } else if (gr1 >= 0.10) {
          adcClipBadge1.textContent = `GR -${gr1.toFixed(1)}dB`;
          adcClipBadge1.classList.remove('clipped');
          adcClipBadge1.classList.add('gr-active');
        } else {
          adcClipBadge1.textContent = 'ADC OK';
          adcClipBadge1.classList.remove('clipped', 'gr-active');
        }
      }

      if (adcClipBadge2) {
        if (data.ch2AdcClipped) {
          adcClipBadge2.textContent = 'ADC CLIP';
          adcClipBadge2.classList.add('clipped');
          adcClipBadge2.classList.remove('gr-active');
        } else if (gr2 >= 0.10) {
          adcClipBadge2.textContent = `GR -${gr2.toFixed(1)}dB`;
          adcClipBadge2.classList.remove('clipped');
          adcClipBadge2.classList.add('gr-active');
        } else {
          adcClipBadge2.textContent = 'ADC OK';
          adcClipBadge2.classList.remove('clipped', 'gr-active');
        }
      }

      if (lblLoopTunerNote) {
        if (data.tunerSource === 3 && data.tunerMidiNote > 0 && data.tunerFreqHz >= 27.5) {
          lblLoopTunerNote.textContent = `${data.tunerNoteName} (${data.tunerFreqHz.toFixed(0)}Hz)`;
        } else if (data.tunerSource !== 3) {
          lblLoopTunerNote.textContent = '--';
        }
      }

      if (flashbackStatusText && typeof data.flashbackSeconds === 'number') {
        flashbackStatusText.textContent = `FLASHBACK RAM: ${Math.round(data.flashbackSeconds)}s / 60s`;
      }

      // 3. Atualiza LEDs de Hardware Mimic da UM2
      if (hwLedSig1) hwLedSig1.classList.toggle('on', in1 > 0.03);
      if (hwLedClip1) hwLedClip1.classList.toggle('on', in1 >= 0.94 || !!data.ch1AdcClipped);
      if (hwLedSig2) hwLedSig2.classList.toggle('on', in2 > 0.03);
      if (hwLedClip2) hwLedClip2.classList.toggle('on', in2 >= 0.94 || !!data.ch2AdcClipped);

      // 4. Status de Streaming do ASIO & Standalone DSP
      const modelTag = data.hardwareModel === 1 ? 'UMC22' : 'UM2';
      if (data.isStreaming) {
        asioBadge.className = 'asio-status-badge active';
        asioStatusText.textContent = `● ASIO • ${modelTag} • DAW`;
      } else if (data.standaloneActive || data.cableDetected) {
        asioBadge.className = 'asio-status-badge active standalone';
        asioStatusText.textContent = `● DSP ACTIVE • ${modelTag}`;
      } else {
        asioBadge.className = 'asio-status-badge standby';
        asioStatusText.textContent = '● STANDBY';
      }

      // 5. Visor OLED Unificado de Telemetria
      if (data.bufferSize) {
        activeBufferSize = data.bufferSize;
        oledBlockSize.innerHTML = `${activeBufferSize} <small>SMP</small>`;
      }

      const outLatMs = data.latencyMs ? data.latencyMs : (activeBufferSize / 48.0);
      const totalRtlMs = data.totalRtlMs ? data.totalRtlMs : (outLatMs * 2.0 + 3.0);
      oledOutLatency.innerHTML = `${outLatMs.toFixed(2)} <small>ms</small>`;
      oledRtlLatency.innerHTML = `${totalRtlMs.toFixed(2)} <small>ms</small>`;

      if (lblTotalPdc && data.totalLatencySamples) {
        lblTotalPdc.textContent = `${data.totalLatencySamples} smp (${totalRtlMs.toFixed(1)}ms)`;
      }

      if (data.sampleRate) {
        oledSampleRate.innerHTML = `${(data.sampleRate / 1000).toFixed(1)} <small>kHz</small>`;
      }

      if (oledDspCpu && typeof data.dspCpuPercent === 'number') {
        oledDspCpu.innerHTML = `${data.dspCpuPercent.toFixed(1)} <small>%</small>`;
      }

      const xrunsCount = data.xruns || 0;
      oledXruns.innerHTML = `${xrunsCount} <small>ERR</small>`;
      if (xrunsCount > 0) {
        oledXruns.className = 'oled-val oled-amber';
      } else {
        oledXruns.className = 'oled-val oled-green';
      }

      // 6. Sincronização inicial de controles na primeira conexão
      if (!isInitialSyncDone && data.connected) {
        isInitialSyncDone = true;

        if (typeof data.hardwareModel === 'number') {
          setHardwareModelVisuals(data.hardwareModel);
        }

        if (typeof data.ch1Phase !== 'undefined') btnPhase1.classList.toggle('active-led', !!data.ch1Phase);
        if (typeof data.ch1Hpf !== 'undefined') btnHpf1.classList.toggle('active-led', !!data.ch1Hpf);
        if (typeof data.ch1Gate !== 'undefined') btnGate1.classList.toggle('active-led', !!data.ch1Gate);
        if (typeof data.ch1Mute !== 'undefined') btnMute1.classList.toggle('muted', !!data.ch1Mute);
        if (typeof data.ch1Trim !== 'undefined') {
          knobTrim1.val = gainToKnobVal(data.ch1Trim);
          knobTrim1.updateVisuals();
        }
        if (typeof data.ch1PreampMode === 'number') {
          document.querySelectorAll('.btn-strip-mode[data-ch="1"]').forEach(b => {
            b.classList.toggle('active', parseInt(b.dataset.preamp, 10) === data.ch1PreampMode);
          });
        }
        if (typeof data.ch1CompAmount === 'number' && sliderComp1) {
          const pct = Math.round(data.ch1CompAmount * 100);
          sliderComp1.value = pct;
          if (valComp1) valComp1.textContent = `${pct}%`;
        }
        if (typeof data.ch1DeEsserAmount === 'number' && sliderDeEss1) {
          const pct = Math.round(data.ch1DeEsserAmount * 100);
          sliderDeEss1.value = pct;
          if (valDeEss1) valDeEss1.textContent = `${pct}%`;
        }

        if (typeof data.ch2Phase !== 'undefined') btnPhase2.classList.toggle('active-led', !!data.ch2Phase);
        if (typeof data.ch2Pad !== 'undefined') btnPad2.classList.toggle('active-led', !!data.ch2Pad);
        if (typeof data.ch2Gate !== 'undefined') btnGate2.classList.toggle('active-led', !!data.ch2Gate);
        if (typeof data.ch2Mute !== 'undefined') btnMute2.classList.toggle('muted', !!data.ch2Mute);
        if (typeof data.ch2CabSim !== 'undefined' && btnCab2) btnCab2.classList.toggle('active-led', !!data.ch2CabSim);
        if (typeof data.ch2Trim !== 'undefined') {
          knobTrim2.val = gainToKnobVal(data.ch2Trim);
          knobTrim2.updateVisuals();
        }
        if (typeof data.ch2PreampMode === 'number') {
          document.querySelectorAll('.btn-strip-mode[data-ch="2"]').forEach(b => {
            b.classList.toggle('active', parseInt(b.dataset.preamp, 10) === data.ch2PreampMode);
          });
        }
        if (typeof data.ch2CompAmount === 'number' && sliderComp2) {
          const pct = Math.round(data.ch2CompAmount * 100);
          sliderComp2.value = pct;
          if (valComp2) valComp2.textContent = `${pct}%`;
        }

        if (typeof data.loopbackMono !== 'undefined') btnLoopMono.classList.toggle('active-led', !!data.loopbackMono);
        if (typeof data.loopbackEnabled !== 'undefined') btnDawSend.classList.toggle('active', !!data.loopbackEnabled);
        if (typeof data.loopbackBroadcastMix !== 'undefined') btnBroadcastMix.classList.toggle('active', !!data.loopbackBroadcastMix);
        if (typeof data.loopbackGain !== 'undefined') {
          knobLoopGain.val = Math.round(data.loopbackGain * 100);
          knobLoopGain.updateVisuals();
        }
        if (typeof data.loopbackProcessMode === 'number') {
          currentLoopbackProcessMode = data.loopbackProcessMode;
          document.querySelectorAll('.btn-strip-mode[data-loopmode]').forEach(b => {
            b.classList.toggle('active', parseInt(b.dataset.loopmode, 10) === currentLoopbackProcessMode);
          });
          updateLoopbackPidRowState(currentLoopbackProcessMode);
          if (currentLoopbackProcessMode > 0) {
            refreshAudioProcesses(data.loopbackTargetPid || 0);
          }
        }

        if (typeof data.masterDim !== 'undefined') btnDim.classList.toggle('active-led', !!data.masterDim);
        if (typeof data.masterMono !== 'undefined') btnMasterMono.classList.toggle('active-led', !!data.masterMono);
        if (typeof data.masterMute !== 'undefined') btnMuteMaster.classList.toggle('muted', !!data.masterMute);
        if (typeof data.masterTrim !== 'undefined') {
          knobMasterOut.val = gainToKnobVal(data.masterTrim);
          knobMasterOut.updateVisuals();
        }
        if (typeof data.dspMonitorOnly !== 'undefined') {
          setDspMonitorOnlyVisuals(!!data.dspMonitorOnly);
        }
        [1, 2].forEach((ch) => {
          const b = document.getElementById(`btnDawDsp${ch}`);
          if (b && typeof data[`dawDspCh${ch}`] !== 'undefined') b.classList.toggle('active-led', !!data[`dawDspCh${ch}`]);
        });
        if (typeof data.monSubCheck !== 'undefined' && btnMonSub) btnMonSub.classList.toggle('active-led', !!data.monSubCheck);
        if (typeof data.monPhoneSim !== 'undefined' && btnMonPhone) btnMonPhone.classList.toggle('active-led', !!data.monPhoneSim);
        if (typeof data.monCrossfeed !== 'undefined' && btnMonXfeed) btnMonXfeed.classList.toggle('active-led', !!data.monCrossfeed);
        if (typeof data.pdcOffsetSamples === 'number' && sliderPdcOffset) {
          sliderPdcOffset.value = data.pdcOffsetSamples;
          if (valPdcOffset) valPdcOffset.textContent = `${data.pdcOffsetSamples >= 0 ? '+' : ''}${data.pdcOffsetSamples}`;
        }

        if (typeof data.softwareMute !== 'undefined') {
          setSoftwareMuteState(!!data.softwareMute);
        }

        if (typeof data.scopeSource !== 'undefined') {
          const invMap = ['master', 'ch1', 'ch2', 'loopback'];
          const srcName = invMap[data.scopeSource] || 'master';
          scopeMonitor.source = srcName;
          document.querySelectorAll('.btn-source-pill').forEach(b => {
            b.classList.toggle('active', b.dataset.source === srcName);
          });
        }

        if (typeof data.tunerSource === 'number') {
          document.querySelectorAll('.btn-tuner-src-pill').forEach(b => {
            b.classList.toggle('active', parseInt(b.dataset.tunersrc, 10) === data.tunerSource);
          });
        }

        if (typeof data.driverMode !== 'undefined') {
          setEngineMode(data.driverMode);
        }

        if (data.bufferSize) {
          selectedBufferSize = data.bufferSize;
          updateMatrixVisualSelection();
        }

        if (typeof data.isLicensed !== 'undefined') {
          updateLicenseVisuals(data.isLicensed, data.licenseTier, data.machineHwid);
        }
      }

      updateSyncStatus();
    } catch (err) {
      console.error('[KaxWav] Erro ao processar telemetria:', err);
    }
  };

  ws.onclose = () => {
    asioBadge.className = 'asio-status-badge standby';
    asioStatusText.textContent = '● ASIO OFFLINE';
    if (!reconnectTimer) {
      reconnectTimer = setTimeout(() => {
        reconnectTimer = null;
        connectWebSocket();
      }, 1500);
    }
  };
}

// ----------------------------------------------------------------------------
// 14. SISTEMA DE LICENCIAMENTO KAXWAV PRO & HWID BINDING
// ----------------------------------------------------------------------------
const licenseBadgeCapsule = document.getElementById('licenseBadgeCapsule');
const licenseBadgeText = document.getElementById('licenseBadgeText');
const licenseModal = document.getElementById('licenseModal');
const btnCloseLicenseModal = document.getElementById('btnCloseLicenseModal');
const licCardBadge = document.getElementById('licCardBadge');
const licCardTier = document.getElementById('licCardTier');
const licCardDesc = document.getElementById('licCardDesc');
const licenseStatusCard = document.getElementById('licenseStatusCard');
const hwidDisplay = document.getElementById('hwidDisplay');
const btnCopyHwid = document.getElementById('btnCopyHwid');
const btnCopyHwidText = document.getElementById('btnCopyHwidText');
const inputLicenseKey = document.getElementById('inputLicenseKey');
const btnActivateLicense = document.getElementById('btnActivateLicense');

let currentHwid = 'UM2-3617-6135-C1FD-B61C';
let isLicensedActive = false;

function updateLicenseVisuals(isLicensed, tier, hwid) {
  isLicensedActive = !!isLicensed;
  if (hwid) {
    currentHwid = hwid;
    if (hwidDisplay) hwidDisplay.textContent = hwid;
  }

  if (isLicensedActive) {
    if (licenseBadgeCapsule) {
      licenseBadgeCapsule.className = 'license-badge-capsule pro';
      licenseBadgeText.textContent = 'PRO VITALÍCIO';
    }
    if (licCardBadge) {
      licCardBadge.className = 'lic-status-badge pro';
      licCardBadge.textContent = 'LICENÇA PRO ATIVADA';
    }
    if (licCardTier) {
      licCardTier.textContent = tier ? `TIER: ${tier} • VITALÍCIO` : 'TIER: PRO_LIFETIME';
    }
    if (licCardDesc) {
      licCardDesc.textContent = 'Sistema completamente liberado com streaming contínuo sem interrupções em 64 samples e DMA em tempo real.';
    }
    if (licenseStatusCard) {
      licenseStatusCard.className = 'license-status-card pro';
    }
    if (btnActivateLicense) {
      btnActivateLicense.style.opacity = '0.7';
      btnActivateLicense.querySelector('.activate-text').textContent = 'LICENÇA PRO JÁ ATIVA';
    }
  } else {
    if (licenseBadgeCapsule) {
      licenseBadgeCapsule.className = 'license-badge-capsule demo';
      licenseBadgeText.textContent = 'MODO DEMO';
    }
    if (licCardBadge) {
      licCardBadge.className = 'lic-status-badge demo';
      licCardBadge.textContent = 'MODO DEMONSTRAÇÃO';
    }
    if (licCardTier) {
      licCardTier.textContent = 'Restrição: Áudio pausa 2s a cada 120s';
    }
    if (licCardDesc) {
      licCardDesc.textContent = 'Ative sua licença para desbloquear latência ultrabaixa de 64 samples e fluxo ininterrupto de estúdio.';
    }
    if (licenseStatusCard) {
      licenseStatusCard.className = 'license-status-card demo';
    }
    if (btnActivateLicense) {
      btnActivateLicense.style.opacity = '1.0';
      btnActivateLicense.querySelector('.activate-text').textContent = 'ATIVAR LICENÇA PRO VITALÍCIO';
    }
  }
}

if (licenseBadgeCapsule) {
  licenseBadgeCapsule.addEventListener('click', () => {
    if (licenseModal) licenseModal.style.display = 'flex';
  });
}

if (btnCloseLicenseModal) {
  btnCloseLicenseModal.addEventListener('click', () => {
    if (licenseModal) licenseModal.style.display = 'none';
  });
}

if (licenseModal) {
  licenseModal.addEventListener('click', (e) => {
    if (e.target === licenseModal) {
      licenseModal.style.display = 'none';
    }
  });
}

if (btnCopyHwid) {
  btnCopyHwid.addEventListener('click', () => {
    const textToCopy = currentHwid || (hwidDisplay ? hwidDisplay.textContent : '');
    navigator.clipboard.writeText(textToCopy).then(() => {
      btnCopyHwidText.textContent = 'COPIADO!';
      showToast('Hardware ID copiado com sucesso!', true);
      setTimeout(() => {
        btnCopyHwidText.textContent = 'COPIAR ID';
      }, 2000);
    }).catch(() => {
      showToast('Erro ao copiar HWID', false);
    });
  });
}

// Auto-formatação da chave com máscara Base32: UM2-XXXX-XXXX-XXXX-XXXX
if (inputLicenseKey) {
  inputLicenseKey.addEventListener('input', (e) => {
    let val = e.target.value.toUpperCase().replace(/[^A-Z0-9]/g, '');
    if (val.startsWith('UM2')) {
      val = val.substring(3);
    }
    val = val.replace(/[IO01]/g, '');
    val = val.substring(0, 16);

    let formatted = 'UM2';
    for (let i = 0; i < val.length; i++) {
      if (i % 4 === 0) formatted += '-';
      formatted += val[i];
    }
    e.target.value = formatted;
  });
}

if (btnActivateLicense) {
  btnActivateLicense.addEventListener('click', async () => {
    const rawKey = inputLicenseKey ? inputLicenseKey.value.trim() : '';
    if (!rawKey || rawKey.length < 15) {
      showToast('Digite uma chave de licença válida!', false);
      return;
    }

    btnActivateLicense.style.opacity = '0.6';
    btnActivateLicense.querySelector('.activate-text').textContent = 'VALIDANDO CRIPTOGRAFIA...';

    try {
      const resp = await fetch('/api/license/activate', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ key: rawKey })
      });
      const data = await resp.json();

      if (data.success) {
        showToast('Licença Pro Ativada com Sucesso!', true);
        updateLicenseVisuals(true, data.license?.tier, data.license?.hwid);
        setTimeout(() => {
          if (licenseModal) licenseModal.style.display = 'none';
        }, 1200);
      } else {
        showToast(data.message || 'Chave de licença inválida!', false);
      }
    } catch (err) {
      showToast('Erro de conexão com o backend de licença', false);
    } finally {
      btnActivateLicense.style.opacity = '1.0';
      btnActivateLicense.querySelector('.activate-text').textContent = 'ATIVAR LICENÇA PRO VITALÍCIO';
    }
  });
}

// ----------------------------------------------------------------------------
// 15. CRÉDITOS & MODAL DE DOAÇÕES (Created by 8kaxcav & fuj11)
// ----------------------------------------------------------------------------
const creditsModal = document.getElementById('creditsModal');
const btnOpenCreditsModal = document.getElementById('btnCredits');
const btnCloseCreditsModal = document.getElementById('btnCloseCreditsModal');
const btnOpenDonateLink = document.getElementById('btnOpenDonateLink');
const btnCopyPix = document.getElementById('btnCopyPix');
const btnCopyPixText = document.getElementById('btnCopyPixText');
const pixKeyDisplay = document.getElementById('pixKeyDisplay');
const creditsModalTitle = document.getElementById('creditsModalTitle');
const creditsModalSubtitle = document.getElementById('creditsModalSubtitle');
const creditsModalMessage = document.getElementById('creditsModalMessage');
const btnDonateLabel = document.getElementById('btnDonateLabel');
const donateSection = document.getElementById('donateSection');
const pixSection = document.getElementById('pixSection');

let currentDonateConfig = {
  creditsTitle: "Created by 8kaxcav & fuj11",
  creditsSubtitle: "KaxWav Studio Suite — Engenharia de Áudio de Baixa Latência",
  donateEnabled: true,
  donateButtonLabel: "APOIE O PROJETO (DONATE)",
  donateUrl: "https://livepix.gg/seu-link-aqui",
  pixKey: "sua-chave-pix-aqui",
  donateMessage: "Gostou da latência absurda do driver? Apoie os criadores 8kaxcav & fuj11 para continuarmos lançando atualizações!"
};

async function loadDonateConfig() {
  try {
    const resp = await fetch('/api/donate-config');
    if (resp.ok) {
      const cfg = await resp.json();
      currentDonateConfig = { ...currentDonateConfig, ...cfg };
      if (creditsModalTitle && cfg.creditsTitle) creditsModalTitle.textContent = cfg.creditsTitle;
      if (creditsModalSubtitle && cfg.creditsSubtitle) creditsModalSubtitle.textContent = cfg.creditsSubtitle;
      if (creditsModalMessage && cfg.donateMessage) creditsModalMessage.textContent = cfg.donateMessage;
      if (btnDonateLabel && cfg.donateButtonLabel) btnDonateLabel.textContent = cfg.donateButtonLabel;
      if (pixKeyDisplay && cfg.pixKey) pixKeyDisplay.textContent = cfg.pixKey;
      if (donateSection) donateSection.style.display = cfg.donateEnabled ? 'flex' : 'none';
      if (pixSection) pixSection.style.display = (cfg.pixKey && cfg.pixKey.trim().length > 0) ? 'flex' : 'none';
    }
  } catch (e) {}
}

if (btnOpenCreditsModal) {
  btnOpenCreditsModal.addEventListener('click', () => {
    loadDonateConfig();
    if (creditsModal) creditsModal.style.display = 'flex';
  });
}

if (btnCloseCreditsModal) {
  btnCloseCreditsModal.addEventListener('click', () => {
    if (creditsModal) creditsModal.style.display = 'none';
  });
}

if (creditsModal) {
  creditsModal.addEventListener('click', (e) => {
    if (e.target === creditsModal) creditsModal.style.display = 'none';
  });
}

if (btnOpenDonateLink) {
  btnOpenDonateLink.addEventListener('click', () => {
    if (currentDonateConfig.donateUrl) {
      window.open(currentDonateConfig.donateUrl, '_blank');
    }
  });
}

if (btnCopyPix) {
  btnCopyPix.addEventListener('click', async () => {
    const key = currentDonateConfig.pixKey || (pixKeyDisplay ? pixKeyDisplay.textContent : '');
    if (!key) return;
    try {
      if (navigator.clipboard && navigator.clipboard.writeText) {
        await navigator.clipboard.writeText(key);
      } else {
        const ta = document.createElement('textarea');
        ta.value = key;
        document.body.appendChild(ta);
        ta.select();
        document.execCommand('copy');
        document.body.removeChild(ta);
      }
      showToast('Chave Pix copiada com sucesso!', true);
      if (btnCopyPixText) btnCopyPixText.textContent = 'COPIADO!';
      setTimeout(() => {
        if (btnCopyPixText) btnCopyPixText.textContent = 'COPIAR PIX';
      }, 2000);
    } catch (e) {
      showToast('Falha ao copiar Pix automaticamente', false);
    }
  });
}

// ----------------------------------------------------------------------------
// 16. INICIALIZAÇÃO DA APLICAÇÃO
// ----------------------------------------------------------------------------
document.addEventListener('DOMContentLoaded', () => {
  renderBufferMatrix();
  updateSyncStatus();
  updateLoopbackPidRowState(0);
  connectWebSocket();
  loadDonateConfig();
});
