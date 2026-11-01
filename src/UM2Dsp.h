#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

// Processador de Estúdio DSP: Soft-Clipper Musical & Dithering TPDF 16-bit
struct StudioDspProcessor {
    uint32_t prngState{ 0x8A5C3D21 };

    inline uint32_t Xorshift32() {
        prngState ^= (prngState << 13);
        prngState ^= (prngState >> 17);
        prngState ^= (prngState << 5);
        return prngState;
    }

    // Soft-Clipper: Compressão analógica suave acima de -1.0 dBFS (~0.89125f)
    // Impede ceifamento rígido (hard clipping) e elimina estouros ásperos
    static inline float SoftClip(float x) {
        constexpr float threshold = 0.89125f;
        if (x > threshold) {
            float excess = x - threshold;
            return threshold + (1.0f - threshold) * (excess / (1.0f + excess));
        } else if (x < -threshold) {
            float excess = -x - threshold;
            return -(threshold + (1.0f - threshold) * (excess / (1.0f + excess)));
        }
        return x;
    }

    // Dithering TPDF (Triangular Probability Density Function) com Auto-Blanking em silêncio digital
    inline int16_t Quantize16(float sample) {
        if (std::abs(sample) < 1e-6f) {
            return 0;
        }
        float clipped = SoftClip(sample);
        // 2 variáveis pseudo-aleatórias uniformes independentes (R1 - R2) geram distribuição triangular [-1.0 LSB, +1.0 LSB]
        int32_t r1 = static_cast<int32_t>(Xorshift32() & 0x7FFF);
        int32_t r2 = static_cast<int32_t>(Xorshift32() & 0x7FFF);
        float ditherLsb = static_cast<float>(r1 - r2) / 32767.0f;

        float scaled = clipped * 32767.0f + ditherLsb;
        int32_t rounded = static_cast<int32_t>(scaled >= 0.0f ? (scaled + 0.5f) : (scaled - 0.5f));
        return static_cast<int16_t>((std::max)(-32768, (std::min)(32767, rounded)));
    }

    // Interpolação Hermite 4-pontos (Catmull-Rom Spline) para Resampling Estéreo de Alta Qualidade
    struct HermiteResampler {
        double phase{ 0.0 };
        float historyL[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
        float historyR[4]{ 0.0f, 0.0f, 0.0f, 0.0f };

        void Reset() {
            phase = 0.0;
            for (int i = 0; i < 4; ++i) {
                historyL[i] = 0.0f;
                historyR[i] = 0.0f;
            }
        }

        static inline float Interpolate(float y0, float y1, float y2, float y3, float t) {
            float c0 = y1;
            float c1 = 0.5f * (y2 - y0);
            float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            return ((c3 * t + c2) * t + c1) * t + c0;
        }

        inline void ProcessStereo(const float* inL, const float* inR, size_t inFrames,
                                  float* outL, float* outR, size_t outFrames,
                                  double inRate, double outRate) {
            if (inRate <= 0.0 || outRate <= 0.0 || inFrames == 0 || outFrames == 0) return;
            double ratio = inRate / outRate;
            for (size_t i = 0; i < outFrames; ++i) {
                size_t baseIdx = static_cast<size_t>(phase);
                float t = static_cast<float>(phase - baseIdx);

                float y0L = (baseIdx >= 1) ? inL[baseIdx - 1] : historyL[3];
                float y1L = inL[(std::min)(baseIdx, inFrames - 1)];
                float y2L = inL[(std::min)(baseIdx + 1, inFrames - 1)];
                float y3L = inL[(std::min)(baseIdx + 2, inFrames - 1)];

                float y0R = (baseIdx >= 1) ? inR[baseIdx - 1] : historyR[3];
                float y1R = inR[(std::min)(baseIdx, inFrames - 1)];
                float y2R = inR[(std::min)(baseIdx + 1, inFrames - 1)];
                float y3R = inR[(std::min)(baseIdx + 2, inFrames - 1)];

                outL[i] = Interpolate(y0L, y1L, y2L, y3L, t);
                outR[i] = Interpolate(y0R, y1R, y2R, y3R, t);

                phase += ratio;
            }

            if (inFrames >= 4) {
                for (int k = 0; k < 4; ++k) {
                    historyL[k] = inL[inFrames - 4 + k];
                    historyR[k] = inR[inFrames - 4 + k];
                }
            }
            if (phase >= inFrames) {
                phase -= inFrames;
            }
        }
    };
    HermiteResampler resampler;
};

// Noise Gate de Estúdio com Detector de Envelope Suave, Histerese de 6.0 dB, Attack 0.5ms, Hold 120ms e Release 160ms
struct StudioNoiseGate {
    float sampleRate{ 48000.0f };
    float currentGain{ 0.0f }; // 0.0f = fechado, 1.0f = aberto
    float envelope{ 0.0f };
    float envAttackCoeff{ 0.15f };
    float envReleaseCoeff{ 0.999f };
    uint32_t holdCounter{ 0 };
    uint32_t holdFrames{ 5760 }; // 120ms @48k
    float attackCoeff{ 0.95f }; // rampa suave de subida ~0.8ms
    float releaseCoeff{ 0.999f }; // decaimento exponencial ~160ms
    bool isOpen{ false };

    void Setup(float sr) {
        sampleRate = (sr > 8000.0f) ? sr : 48000.0f;
        envAttackCoeff = 1.0f - expf(-1.0f / (0.0003f * sampleRate));
        envReleaseCoeff = expf(-1.0f / (0.055f * sampleRate));
        attackCoeff = 1.0f - expf(-1.0f / (0.0008f * sampleRate));
        releaseCoeff = expf(-1.0f / (0.160f * sampleRate));
        holdFrames = static_cast<uint32_t>(0.120f * sampleRate);
        holdCounter = 0;
        currentGain = 0.0f;
        envelope = 0.0f;
        isOpen = false;
    }

    inline float Process(float sample, bool enabled, float thresholdDb = -58.0f) {
        float absSample = std::abs(sample);
        if (absSample > envelope) {
            envelope += (absSample - envelope) * envAttackCoeff;
        } else {
            envelope *= envReleaseCoeff;
            if (envelope < 1e-7f) envelope = 0.0f;
        }

        if (!enabled) {
            currentGain = 1.0f;
            isOpen = true;
            return sample;
        }

        float envDb = (envelope > 1e-6f) ? (20.0f * log10f(envelope)) : -120.0f;

        float openThreshold = thresholdDb;
        float closeThreshold = thresholdDb - 6.0f; // 6.0 dB de histerese para nunca cortar fim de frases

        if (envDb >= openThreshold) {
            isOpen = true;
            holdCounter = holdFrames;
        } else if (holdCounter > 0) {
            holdCounter--;
            isOpen = true;
        } else if (envDb < closeThreshold) {
            isOpen = false;
        }

        float targetGain = isOpen ? 1.0f : 0.0f;
        if (isOpen) {
            currentGain += (targetGain - currentGain) * attackCoeff;
        } else {
            currentGain *= releaseCoeff;
            if (currentGain < 1e-5f) currentGain = 0.0f;
        }

        return sample * currentGain;
    }
};

// Filtro Biquad Butterworth 4ª Ordem (24 dB/oct no Process HPF) em 64-bit Double (Transposed Direct Form II) com Rampa Suave
struct BiquadFilter {
    double b0{ 1.0 }, b1{ 0.0 }, b2{ 0.0 };
    double a1{ 0.0 }, a2{ 0.0 };
    double z1{ 0.0 }, z2{ 0.0 };
    double z3{ 0.0 }, z4{ 0.0 }; // 2o estagio em cascata para corte cirurgico de 24 dB/oitava no HPF
    float currentWeight{ 0.0f }; // 0.0f = bypass, 1.0f = ativo

    void SetupHighPass(float sampleRate, float cutoffHz = 95.0f) {
        double sr = (sampleRate > 8000.0f) ? static_cast<double>(sampleRate) : 48000.0;
        double w0 = 2.0 * 3.14159265358979323846 * static_cast<double>(cutoffHz) / sr;
        double cosw0 = std::cos(w0);
        double alpha = std::sin(w0) / (2.0 * 0.70710678118654752440);
        double a0 = 1.0 + alpha;
        b0 = ((1.0 + cosw0) * 0.5) / a0;
        b1 = (-(1.0 + cosw0)) / a0;
        b2 = ((1.0 + cosw0) * 0.5) / a0;
        a1 = (-2.0 * cosw0) / a0;
        a2 = (1.0 - alpha) / a0;
        Reset();
    }

    void Reset() {
        z1 = 0.0;
        z2 = 0.0;
        z3 = 0.0;
        z4 = 0.0;
        currentWeight = 0.0f;
    }

    inline float Process(float in, bool enabled) {
        // Rampa suave de 128 amostras (~2.66ms @48k) para eliminar estalos ao clicar no botão
        float targetWeight = enabled ? 1.0f : 0.0f;
        if (currentWeight < targetWeight) {
            currentWeight = (std::min)(targetWeight, currentWeight + (1.0f / 128.0f));
        } else if (currentWeight > targetWeight) {
            currentWeight = (std::max)(targetWeight, currentWeight - (1.0f / 128.0f));
        }

        double x = static_cast<double>(in);
        // 1o estagio (12 dB/oct)
        double y1 = b0 * x + z1;
        z1 = b1 * x - a1 * y1 + z2;
        z2 = b2 * x - a2 * y1;
        // 2o estagio (24 dB/oct Linkwitz-Riley / Butterworth)
        double y2 = b0 * y1 + z3;
        z3 = b1 * y1 - a1 * y2 + z4;
        z4 = b2 * y1 - a2 * y2;

        if (std::abs(z1) < 1e-30) z1 = 0.0;
        if (std::abs(z2) < 1e-30) z2 = 0.0;
        if (std::abs(z3) < 1e-30) z3 = 0.0;
        if (std::abs(z4) < 1e-30) z4 = 0.0;

        float outHpf = static_cast<float>(y2);
        return in * (1.0f - currentWeight) + outHpf * currentWeight;
    }

    // Configuracoes adicionais para Biquad generico (LowPass, BandPass, Peaking, HighShelf)
    void SetupLowPass(float sampleRate, float cutoffHz, float Q = 0.70710678f) {
        double sr = (sampleRate > 8000.0f) ? static_cast<double>(sampleRate) : 48000.0;
        double w0 = 2.0 * 3.14159265358979323846 * static_cast<double>(cutoffHz) / sr;
        double cosw0 = std::cos(w0);
        double alpha = std::sin(w0) / (2.0 * static_cast<double>(Q));
        double a0 = 1.0 + alpha;
        b0 = ((1.0 - cosw0) * 0.5) / a0;
        b1 = (1.0 - cosw0) / a0;
        b2 = ((1.0 - cosw0) * 0.5) / a0;
        a1 = (-2.0 * cosw0) / a0;
        a2 = (1.0 - alpha) / a0;
        z1 = 0.0;
        z2 = 0.0;
        z3 = 0.0;
        z4 = 0.0;
        currentWeight = 1.0f;
    }

    void SetupBandPass(float sampleRate, float centerHz, float Q = 2.0f) {
        double sr = (sampleRate > 8000.0f) ? static_cast<double>(sampleRate) : 48000.0;
        double w0 = 2.0 * 3.14159265358979323846 * static_cast<double>(centerHz) / sr;
        double cosw0 = std::cos(w0);
        double alpha = std::sin(w0) / (2.0 * static_cast<double>(Q));
        double a0 = 1.0 + alpha;
        b0 = alpha / a0;
        b1 = 0.0;
        b2 = -alpha / a0;
        a1 = (-2.0 * cosw0) / a0;
        a2 = (1.0 - alpha) / a0;
        z1 = 0.0;
        z2 = 0.0;
        z3 = 0.0;
        z4 = 0.0;
        currentWeight = 1.0f;
    }

    void SetupPeaking(float sampleRate, float centerHz, float Q, float gainDb) {
        double sr = (sampleRate > 8000.0f) ? static_cast<double>(sampleRate) : 48000.0;
        double A = std::pow(10.0, static_cast<double>(gainDb) / 40.0);
        double w0 = 2.0 * 3.14159265358979323846 * static_cast<double>(centerHz) / sr;
        double cosw0 = std::cos(w0);
        double alpha = std::sin(w0) / (2.0 * static_cast<double>(Q));
        double a0 = 1.0 + alpha / A;
        b0 = (1.0 + alpha * A) / a0;
        b1 = (-2.0 * cosw0) / a0;
        b2 = (1.0 - alpha * A) / a0;
        a1 = (-2.0 * cosw0) / a0;
        a2 = (1.0 - alpha / A) / a0;
        z1 = 0.0;
        z2 = 0.0;
        z3 = 0.0;
        z4 = 0.0;
        currentWeight = 1.0f;
    }

    void SetupHighShelf(float sampleRate, float shelfHz, float gainDb) {
        double sr = (sampleRate > 8000.0f) ? static_cast<double>(sampleRate) : 48000.0;
        double A = std::pow(10.0, static_cast<double>(gainDb) / 40.0);
        double w0 = 2.0 * 3.14159265358979323846 * static_cast<double>(shelfHz) / sr;
        double cosw0 = std::cos(w0);
        double sinw0 = std::sin(w0);
        double alpha = sinw0 * 0.5 * std::sqrt(2.0);
        double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alpha;
        double a0 = (A + 1.0) - (A - 1.0) * cosw0 + twoSqrtAAlpha;
        b0 = (A * ((A + 1.0) + (A - 1.0) * cosw0 + twoSqrtAAlpha)) / a0;
        b1 = (-2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0)) / a0;
        b2 = (A * ((A + 1.0) + (A - 1.0) * cosw0 - twoSqrtAAlpha)) / a0;
        a1 = (2.0 * ((A - 1.0) - (A + 1.0) * cosw0)) / a0;
        a2 = ((A + 1.0) - (A - 1.0) * cosw0 - twoSqrtAAlpha) / a0;
        z1 = 0.0;
        z2 = 0.0;
        z3 = 0.0;
        z4 = 0.0;
        currentWeight = 1.0f;
    }

    inline float ProcessDirect(float in) {
        double x = static_cast<double>(in);
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        if (std::abs(z1) < 1e-30) z1 = 0.0;
        if (std::abs(z2) < 1e-30) z2 = 0.0;
        return static_cast<float>(y);
    }
};

// ============================================================================
// UNDERGROUND STUDIO CONSOLE v3.0 — ZERO-ALLOCATION REAL-TIME DSP MODULES
// ============================================================================

// 1. Detector de Clip Fisico do Conversor 16-bit (PCM2902 ADC Clip Guard)
struct AdcClipGuard {
    uint32_t consecutiveClips{ 0 };
    uint32_t holdFramesRemaining{ 0 };
    uint32_t holdDurationFrames{ 12000 }; // ~250ms @ 48kHz

    void Setup(float sampleRate) {
        holdDurationFrames = static_cast<uint32_t>((sampleRate > 8000.0f ? sampleRate : 48000.0f) * 0.25f);
        consecutiveClips = 0;
        holdFramesRemaining = 0;
    }

    inline bool CheckRawInt16(int16_t rawSample) {
        int32_t absVal = std::abs(static_cast<int32_t>(rawSample));
        if (absVal >= 32760) {
            consecutiveClips++;
            if (consecutiveClips >= 3) {
                holdFramesRemaining = holdDurationFrames;
            }
        } else {
            consecutiveClips = 0;
        }
        if (holdFramesRemaining > 0) {
            holdFramesRemaining--;
            return true;
        }
        return false;
    }

    inline bool CheckFloatPeak(float sample) {
        if (std::abs(sample) >= 0.9985f) {
            consecutiveClips++;
            if (consecutiveClips >= 3) {
                holdFramesRemaining = holdDurationFrames;
            }
        } else {
            consecutiveClips = 0;
        }
        if (holdFramesRemaining > 0) {
            holdFramesRemaining--;
            return true;
        }
        return false;
    }

    inline void CheckSample(float sample) {
        CheckFloatPeak(sample);
    }

    inline bool IsClipping() const noexcept {
        return holdFramesRemaining > 0;
    }
};

// 2. Modelador de Pre-Amp Analogico ("Unison-Style": Clean / 73 Trap Warm+Air / Tube Grit)
struct AnalogPreampProcessor {
    // Filtros dedicados do Modo 0 (Clean Studio Condenser: Anti-Mud 310Hz, Articulation 4.2kHz, Crisp Air 7.5kHz)
    BiquadFilter cleanMudCut;
    BiquadFilter cleanPresence;
    BiquadFilter cleanAirShelf;
    // Filtros dedicados do Modo 1 (Neve 1073 Trap Vocal: Tight Chest 120Hz, Anti-Mud 320Hz, Presence +3.8kHz, Air Shelf +8.5kHz)
    BiquadFilter trapLowPunch;
    BiquadFilter trapMudCut;
    BiquadFilter trapPresence;
    BiquadFilter trapAirShelf;
    // Filtros dedicados do Modo 2 (Avalon/Telefunken Tube Valve: Warm Body 135Hz, Anti-Mud 330Hz, Vocal Intimacy 2.8kHz, Silk Air +7.2kHz)
    BiquadFilter tubeBodyPeak;
    BiquadFilter tubeMudCut;
    BiquadFilter tubeMidWarmth;
    BiquadFilter tubeSmoothTop;

    float airLpState{ 0.0f };
    float dcBlockX1{ 0.0f };
    float dcBlockY1{ 0.0f };
    float airAlpha{ 0.72f };
    float adaptiveEnv{ 0.015f };

    void Setup(float sampleRate) {
        float sr = (sampleRate > 8000.0f) ? sampleRate : 48000.0f;
        // Curva Clean Studio (compensa o roll-off agudo e o medio encaixotado do codec TI PCM2902 no VB-Audio)
        cleanMudCut.SetupPeaking(sr, 310.0f, 1.30f, -2.2f);
        cleanPresence.SetupPeaking(sr, 4200.0f, 1.05f, 2.5f);
        cleanAirShelf.SetupHighShelf(sr, 7500.0f, 3.8f);

        // Curva Neve 1073 Trap: +2.2 dB @ 120 Hz, -3.0 dB @ 320 Hz (limpeza), +6.0 dB @ 3.8 kHz, +8.2 dB High-Shelf @ 8.5 kHz
        trapLowPunch.SetupPeaking(sr, 120.0f, 1.15f, 2.2f);
        trapMudCut.SetupPeaking(sr, 320.0f, 1.35f, -3.0f);
        trapPresence.SetupPeaking(sr, 3800.0f, 0.95f, 6.0f);
        trapAirShelf.SetupHighShelf(sr, 8500.0f, 8.2f);

        // Curva Modern Tube (Avalon/CL1B): +3.5 dB @ 135 Hz, -2.5 dB @ 330 Hz, +3.6 dB @ 2.8 kHz, +4.5 dB Silk Air @ 7.2 kHz
        tubeBodyPeak.SetupPeaking(sr, 135.0f, 1.05f, 3.5f);
        tubeMudCut.SetupPeaking(sr, 330.0f, 1.30f, -2.5f);
        tubeMidWarmth.SetupPeaking(sr, 2800.0f, 0.95f, 3.6f);
        tubeSmoothTop.SetupHighShelf(sr, 7200.0f, 4.5f);

        airAlpha = 1.0f - std::exp(-2.0f * 3.14159265f * 9500.0f / sr);
        airLpState = 0.0f;
        dcBlockX1 = 0.0f;
        dcBlockY1 = 0.0f;
        adaptiveEnv = 0.015f;
    }

    // Aproximacao Pade ultra-rapida de tanh(x) para zero overhead na thread real-time
    static inline float FastTanh(float x) {
        if (x > 3.0f) return 1.0f;
        if (x < -3.0f) return -1.0f;
        float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    inline float Process(float x, int32_t mode, int32_t hardwareModel = 0) {
        if (hardwareModel == 1) {
            x *= 1.03f;
        }

        // Seguimento de envelope adaptativo para garantir saturacao harmonica audivel em qualquer volume de microfone
        float absX = std::abs(x);
        if (absX > adaptiveEnv) {
            adaptiveEnv += (absX - adaptiveEnv) * 0.015f;
        } else {
            adaptiveEnv = adaptiveEnv * 0.9995f + 0.000001f;
        }
        float normScale = (std::max)(0.0035f, (std::min)(0.25f, adaptiveEnv * 1.8f));
        float invNorm = 1.0f / normScale;

        if (mode <= 0) {
            // Modo 0: CLEAN (Zero distorcao, clareza de condensador de estudio sem som abafado no VB-Audio)
            float clean = cleanMudCut.ProcessDirect(x);
            clean = cleanPresence.ProcessDirect(clean);
            clean = cleanAirShelf.ProcessDirect(clean);
            return StudioDspProcessor::SoftClip(clean * 1.12f);
        }

        if (mode == 1) {
            // Modo 1: 73 TRAP (Saturacao de Transformador Assimetrica de 2a Ordem + EQ Neve 1073 + Air Exciter)
            float xNorm = x * invNorm;
            constexpr float drive = 2.15f;
            constexpr float bias = 0.14f;
            constexpr float biasTanh = 0.139092f; // tanh(0.14)
            float satNorm = (FastTanh(drive * xNorm + bias) - biasTanh) * (1.0f / 1.65f);
            float sat = satNorm * normScale;

            // DC Blocker
            float yDc = sat - dcBlockX1 + 0.9992f * dcBlockY1;
            dcBlockX1 = sat;
            dcBlockY1 = yDc;

            // Tone Stack Neve 1073 com corte anti-lama em 320 Hz
            float shaped = trapLowPunch.ProcessDirect(yDc);
            shaped = trapMudCut.ProcessDirect(shaped);
            shaped = trapPresence.ProcessDirect(shaped);
            shaped = trapAirShelf.ProcessDirect(shaped);

            // Air Exciter Harmonico acima de 9.5 kHz
            airLpState += airAlpha * (shaped - airLpState);
            float highBand = shaped - airLpState;
            float excitedHigh = FastTanh(highBand * invNorm * 2.2f) * normScale * 0.55f;

            // +4.5 dB de presenca vocal na frente da mix
            return StudioDspProcessor::SoftClip((shaped + excitedHigh) * 1.68f);
        }

        // Modo 2: TUBE (Valvula Quente + Limpeza em 330Hz + Presenca 2.8kHz + Brilho de Seda em 7.2kHz)
        float xNorm = x * invNorm;
        float tubeDrivenNorm = FastTanh(xNorm * 2.65f + 0.08f * xNorm * xNorm) * 0.78f;
        float tubeSat = (x * 0.25f + (tubeDrivenNorm * normScale) * 0.75f);

        float warm = tubeBodyPeak.ProcessDirect(tubeSat);
        warm = tubeMudCut.ProcessDirect(warm);
        warm = tubeMidWarmth.ProcessDirect(warm);
        warm = tubeSmoothTop.ProcessDirect(warm);

        // +4.8 dB de corpo valvulado aberto e cristalino
        return StudioDspProcessor::SoftClip(warm * 1.74f);
    }
};

// 3. De-Esser Dinamico Focada na Banda de Sibilancia (5.2 kHz - 9.5 kHz) com Deteccao Relativa Vocal
// Nunca abafa vogais ou silencio — atenua cirurgicamente apenas consoantes sibilantes ("S", "SH", "C", "Z")
struct StudioDeEsser {
    BiquadFilter sibBandFilter;
    BiquadFilter bodyLpFilter;
    float sibEnv{ 0.0f };
    float bodyEnv{ 0.0f };
    float attackCoeff{ 0.25f };
    float releaseCoeff{ 0.995f };
    float meterReleaseCoeff{ 0.99985f };
    float lastGainReductionDb{ 0.0f };
    float meterGrDb{ 0.0f };

    void Setup(float sampleRate) {
        float sr = (sampleRate > 8000.0f) ? sampleRate : 48000.0f;
        // Isola estritamente a banda de sibilancia (centrada em 6.8 kHz, Q=1.35) preservando medios (<4.5 kHz) e ar (>10 kHz)
        sibBandFilter.SetupBandPass(sr, 6800.0f, 1.35f);
        // Detector do corpo da voz (< 3.2 kHz) para distinguir vogais normais de sibilancia real ("S"/"SH")
        bodyLpFilter.SetupLowPass(sr, 3200.0f, 0.7071f);
        attackCoeff = 1.0f - std::exp(-1.0f / (0.0004f * sr));
        releaseCoeff = std::exp(-1.0f / (0.040f * sr));
        meterReleaseCoeff = std::exp(-1.0f / (0.220f * sr));
        sibEnv = 0.0f;
        bodyEnv = 0.0f;
        lastGainReductionDb = 0.0f;
        meterGrDb = 0.0f;
    }

    inline float Process(float x, float amount) {
        if (amount <= 0.005f) {
            lastGainReductionDb = 0.0f;
            meterGrDb *= meterReleaseCoeff;
            if (meterGrDb < 0.05f) meterGrDb = 0.0f;
            return x;
        }
        float amt = (std::min)(1.0f, (std::max)(0.0f, amount));

        float sibBand = sibBandFilter.ProcessDirect(x);
        float bodyBand = bodyLpFilter.ProcessDirect(x);

        float absSib = std::abs(sibBand);
        float absBody = std::abs(bodyBand);

        if (absSib > sibEnv) sibEnv += (absSib - sibEnv) * attackCoeff;
        else sibEnv *= releaseCoeff;

        if (absBody > bodyEnv) bodyEnv += (absBody - bodyEnv) * attackCoeff;
        else bodyEnv *= releaseCoeff;

        // Piso absoluto imune a ruido de fundo (-35 dBFS em 10% ate -49 dBFS em 100%)
        float absThresh = 0.018f * std::pow(0.19f, amt);
        // Razao relativa: exige que a energia de sibilancia (6.8 kHz) supere proporcionalmente o corpo da vogal
        float relRatio = 0.52f - 0.28f * amt;
        float triggerLevel = (std::max)(absThresh, bodyEnv * relRatio);

        float maxReduction = 0.25f + 0.62f * amt; // Ate 87% (-17.7 dB) de corte cirurgico apenas sobre o "S"
        float reduction = 0.0f;
        if (sibEnv > triggerLevel) {
            float excess = (sibEnv - triggerLevel) / (sibEnv + 1e-6f);
            reduction = (std::min)(maxReduction, excess * (0.85f + 0.95f * amt));
        }

        float instGrDb = -20.0f * std::log10((std::max)(0.10f, 1.0f - reduction));
        lastGainReductionDb = instGrDb;
        if (instGrDb > meterGrDb) {
            meterGrDb = instGrDb;
        } else {
            meterGrDb *= meterReleaseCoeff;
            if (meterGrDb < 0.05f) meterGrDb = 0.0f;
        }

        // Subtrai dinamicamente apenas a banda sibilante (6.8 kHz), mantendo a voz 100% aberta e cristalina
        return x - sibBand * reduction;
    }

    inline float GetGainReductionDb() const noexcept {
        return meterGrDb;
    }
};

// 4. Compressor Estilo 1176 FET One-Knob com Auto Make-Up Inteligente & Medidor de Reducao de Ganho (GR dB)
struct StudioFetCompressor {
    float env{ 0.0f };
    float gateEnv{ 0.0f };
    float attackCoeff{ 0.04f };
    float releaseCoeff{ 0.999f };
    float meterReleaseCoeff{ 0.99985f };
    float lastGainReductionDb{ 0.0f };
    float meterGrDb{ 0.0f };

    void Setup(float sampleRate) {
        float sr = (sampleRate > 8000.0f) ? sampleRate : 48000.0f;
        attackCoeff = 1.0f - std::exp(-1.0f / (0.0008f * sr));  // 0.8ms Ultra-Fast FET Attack
        releaseCoeff = std::exp(-1.0f / (0.065f * sr));         // 65ms Punchy Release
        meterReleaseCoeff = std::exp(-1.0f / (0.240f * sr));    // 240ms Visual Meter Hold/Decay
        env = 0.0f;
        gateEnv = 0.0f;
        lastGainReductionDb = 0.0f;
        meterGrDb = 0.0f;
    }

    inline float Process(float x, float amount) {
        if (amount <= 0.005f) {
            lastGainReductionDb = 0.0f;
            meterGrDb *= meterReleaseCoeff;
            if (meterGrDb < 0.05f) meterGrDb = 0.0f;
            return x;
        }
        float amt = (std::min)(1.0f, (std::max)(0.0f, amount));

        float absX = std::abs(x);
        if (absX > env) env += (absX - env) * attackCoeff;
        else {
            env *= releaseCoeff;
            if (env < 1e-7f) env = 0.0f;
        }

        float envDb = (env > 1e-6f) ? (20.0f * std::log10(env)) : -100.0f;
        // Threshold calibrado para pegar ate microfones baixos: desce de -18 dBFS ate -54 dBFS
        float thresholdDb = -18.0f - (36.0f * amt);
        // Ratio sobe de 4.0:1 ate 20.0:1 (All-Buttons-In 1176 Slam em 100%)
        float ratio = 4.0f + 16.0f * amt;
        float slope = 1.0f - (1.0f / ratio);

        float grDb = 0.0f;
        float overDb = envDb - thresholdDb;
        constexpr float kneeHalf = 3.0f;
        if (overDb > -kneeHalf) {
            if (overDb < kneeHalf) {
                float k = overDb + kneeHalf;
                grDb = slope * (k * k) / (4.0f * kneeHalf);
            } else {
                grDb = slope * overDb;
            }
        }

        float maxGr = 4.0f + (18.0f * amt);
        grDb = (std::min)(maxGr, grDb);
        lastGainReductionDb = grDb;
        if (grDb > meterGrDb) {
            meterGrDb = grDb;
        } else {
            meterGrDb *= meterReleaseCoeff;
            if (meterGrDb < 0.05f) meterGrDb = 0.0f;
        }

        // Auto Make-Up Gain inteligente: compensa a reducao de ganho (85%) e adiciona densidade vocal (+9.5 dB em 100%)
        float voicePresence = (std::min)(1.0f, (std::max)(0.10f, (envDb + 62.0f) * (1.0f / 14.0f)));
        float makeupDb = ((grDb * 0.85f) + (amt * 9.5f)) * voicePresence;
        float netGainLinear = std::pow(10.0f, (-grDb + makeupDb) * 0.05f);
        return StudioDspProcessor::SoftClip(x * netGainLinear);
    }

    inline float GetGainReductionDb() const noexcept {
        return meterGrDb;
    }
};

// 5. Simulador Zero-Latency de Amp & Gabinete 4x12 / Bass Cab para o Canal 2 (INST 2 Hi-Z)
struct GuitarCabSimulator {
    BiquadFilter hpfTight;
    BiquadFilter cabResonance;
    BiquadFilter midContour;
    BiquadFilter lpfRoll1;
    BiquadFilter lpfRoll2;

    void Setup(float sampleRate) {
        float sr = (sampleRate > 8000.0f) ? sampleRate : 48000.0f;
        hpfTight.SetupHighPass(sr, 75.0f);
        cabResonance.SetupPeaking(sr, 118.0f, 1.85f, 3.8f);   // Ressonancia de caixa fechada 4x12
        midContour.SetupPeaking(sr, 2150.0f, 1.1f, 3.2f);     // Presenca de cone Celestion
        lpfRoll1.SetupLowPass(sr, 4700.0f, 0.7071f);          // Corte de aspereza de linha 24dB/oct
        lpfRoll2.SetupLowPass(sr, 5200.0f, 0.95f);
    }

    inline float Process(float x, bool enabled) {
        if (!enabled) return x;
        // Pre-Drive dinamico sensivel ao toque da palheta
        float driven = AnalogPreampProcessor::FastTanh(x * 2.1f) * 1.15f;
        float s = hpfTight.ProcessDirect(driven);
        s = cabResonance.ProcessDirect(s);
        s = midContour.ProcessDirect(s);
        s = lpfRoll1.ProcessDirect(s);
        s = lpfRoll2.ProcessDirect(s);
        return StudioDspProcessor::SoftClip(s);
    }
};

// 6. Afinador Cromatico Lock-Free em Tempo Real (Algoritmo YIN / CMNDF + Interpolacao Parabolica)
struct ChromaticPitchTuner {
    static constexpr size_t kWindowSize = 2048;
    static constexpr size_t kHopSize = 1024;
    static constexpr size_t kHalfWindow = kWindowSize / 2;
    float ring[kWindowSize]{};
    float frame[kWindowSize]{};
    float diff[kHalfWindow + 1]{};
    float cmndf[kHalfWindow + 1]{};
    size_t writePos{ 0 };
    size_t samplesSinceLastRun{ 0 };
    float sampleRate{ 48000.0f };

    float detectedFreqHz{ 0.0f };
    int32_t detectedMidiNote{ 0 };
    float detectedCents{ 0.0f };
    uint32_t silenceHold{ 0 };

    void Setup(float sr) {
        sampleRate = (sr > 8000.0f) ? sr : 48000.0f;
        writePos = 0;
        samplesSinceLastRun = 0;
        detectedFreqHz = 0.0f;
        detectedMidiNote = 0;
        detectedCents = 0.0f;
        silenceHold = 0;
        for (size_t i = 0; i < kWindowSize; ++i) {
            ring[i] = 0.0f;
            frame[i] = 0.0f;
        }
        for (size_t i = 0; i <= kHalfWindow; ++i) {
            diff[i] = 0.0f;
            cmndf[i] = 0.0f;
        }
    }

    inline void PushSample(float x) {
        ring[writePos] = x;
        writePos = (writePos + 1) & (kWindowSize - 1);
        samplesSinceLastRun++;
        if (samplesSinceLastRun >= kHopSize) {
            samplesSinceLastRun = 0;
            AnalyzeFrame();
        }
    }

    void AnalyzeFrame() {
        // Extrai janela ordenada linearmente e verifica energia RMS
        float energy = 0.0f;
        for (size_t i = 0; i < kWindowSize; ++i) {
            float v = ring[(writePos + i) & (kWindowSize - 1)];
            frame[i] = v;
            energy += v * v;
        }
        float rms = std::sqrt(energy / static_cast<float>(kWindowSize));
        if (rms < 0.0010f) { // Abaixo de -60 dBFS: mantem ultima leitura por ~600ms e depois zera
            silenceHold++;
            if (silenceHold > 28) {
                detectedFreqHz = 0.0f;
                detectedMidiNote = 0;
                detectedCents = 0.0f;
            }
            return;
        }
        silenceHold = 0;

        // Faixa de busca: 47 Hz (Sub/808/Baixo F#1) ate 1050 Hz (C6 Vocal/Guitarra)
        const size_t minTau = static_cast<size_t>((std::max)(8.0f, sampleRate / 1050.0f));
        const size_t maxTau = (std::min)(kHalfWindow - 1, static_cast<size_t>(sampleRate / 35.0f));

        cmndf[0] = 1.0f;
        float runningSum = 0.0f;

        for (size_t tau = 1; tau <= maxTau; ++tau) {
            float sum = 0.0f;
            for (size_t i = 0; i < kHalfWindow; ++i) {
                float d = frame[i] - frame[i + tau];
                sum += d * d;
            }
            diff[tau] = sum;
            runningSum += sum;
            cmndf[tau] = (runningSum > 1e-9f) ? (sum * static_cast<float>(tau) / runningSum) : 1.0f;
        }

        // Busca o primeiro minimo absoluto abaixo do limiar YIN (0.18)
        size_t bestTau = 0;
        constexpr float threshold = 0.18f;
        for (size_t tau = minTau; tau < maxTau; ++tau) {
            if (cmndf[tau] < threshold) {
                while (tau + 1 < maxTau && cmndf[tau + 1] < cmndf[tau]) {
                    tau++;
                }
                bestTau = tau;
                break;
            }
        }

        // Se nao atingiu 0.18, pega o minimo global se for confiavel (< 0.35)
        if (bestTau == 0) {
            float minVal = 1.0f;
            for (size_t tau = minTau; tau < maxTau; ++tau) {
                if (cmndf[tau] < minVal) {
                    minVal = cmndf[tau];
                    bestTau = tau;
                }
            }
            if (minVal > 0.35f) return;
        }

        // Interpolacao parabolica para precisao sub-sample (< 0.2 cents)
        float refinedTau = static_cast<float>(bestTau);
        if (bestTau > 1 && bestTau + 1 <= maxTau) {
            float s0 = cmndf[bestTau - 1];
            float s1 = cmndf[bestTau];
            float s2 = cmndf[bestTau + 1];
            float denom = 2.0f * (2.0f * s1 - s2 - s0);
            if (std::abs(denom) > 1e-7f) {
                refinedTau += (s2 - s0) / denom;
            }
        }

        float freq = sampleRate / refinedTau;
        if (freq < 30.0f || freq > 1200.0f) return;

        // Suavizacao leve se estiver na mesma nota
        if (detectedFreqHz > 0.0f && std::abs(freq - detectedFreqHz) / detectedFreqHz < 0.04f) {
            freq = detectedFreqHz * 0.45f + freq * 0.55f;
        }
        detectedFreqHz = freq;

        // Conversao para MIDI Note e Cents (A4 = 440.0 Hz = MIDI 69)
        float exactMidi = 69.0f + 12.0f * std::log2(freq / 440.0f);
        int32_t nearestMidi = static_cast<int32_t>( std::floor(exactMidi + 0.5f) );
        detectedMidiNote = (std::max)(0, (std::min)(127, nearestMidi));
        detectedCents = (exactMidi - static_cast<float>(detectedMidiNote)) * 100.0f;
    }

    inline float GetFrequencyHz() const noexcept { return detectedFreqHz; }
    inline int32_t GetMidiNote() const noexcept { return detectedMidiNote; }
    inline float GetCents() const noexcept { return detectedCents; }
};

// 7. Processador de Monitoramento Master Pos-DAW (Mix Check: Sub 808, Phone Sim, Headphone Crossfeed)
struct MasterMonitorProcessor {
    // Sub Check (<120 Hz 4th-Order Linkwitz-Riley)
    BiquadFilter subLpL1, subLpL2;
    BiquadFilter subLpR1, subLpR2;

    // Phone Speaker Translation Sim (320 Hz - 4.2 kHz + 1.4 kHz Mid Box)
    BiquadFilter phoneHpL, phoneLpL, phoneMidL;
    BiquadFilter phoneHpR, phoneLpR, phoneMidR;

    // Headphone Acoustic Crossfeed (~290us ITD Delay + 700 Hz Low-Pass)
    static constexpr size_t kCrossDelayLen = 32;
    float delayLineL[kCrossDelayLen]{};
    float delayLineR[kCrossDelayLen]{};
    size_t delayIdx{ 0 };
    size_t delaySamples{ 14 }; // 14 samples @ 48kHz = 291 us
    float xfeedLpL{ 0.0f };
    float xfeedLpR{ 0.0f };
    float xfeedAlpha{ 0.087f };

    void Setup(float sampleRate) {
        float sr = (sampleRate > 8000.0f) ? sampleRate : 48000.0f;
        subLpL1.SetupLowPass(sr, 120.0f, 0.7071f);
        subLpL2.SetupLowPass(sr, 120.0f, 0.7071f);
        subLpR1.SetupLowPass(sr, 120.0f, 0.7071f);
        subLpR2.SetupLowPass(sr, 120.0f, 0.7071f);

        phoneHpL.SetupHighPass(sr, 320.0f);
        phoneLpL.SetupLowPass(sr, 4200.0f, 0.8f);
        phoneMidL.SetupPeaking(sr, 1450.0f, 1.4f, 4.0f);

        phoneHpR.SetupHighPass(sr, 320.0f);
        phoneLpR.SetupLowPass(sr, 4200.0f, 0.8f);
        phoneMidR.SetupPeaking(sr, 1450.0f, 1.4f, 4.0f);

        delaySamples = (std::min)(kCrossDelayLen - 1, static_cast<size_t>((std::max)(4.0f, sr * 0.00029f)));
        xfeedAlpha = 1.0f - std::exp(-2.0f * 3.14159265f * 700.0f / sr);
        delayIdx = 0;
        xfeedLpL = 0.0f;
        xfeedLpR = 0.0f;
        for (size_t i = 0; i < kCrossDelayLen; ++i) {
            delayLineL[i] = 0.0f;
            delayLineR[i] = 0.0f;
        }
    }

    inline void ProcessStereo(float& ioL, float& ioR, bool subCheck, bool phoneSim, bool crossfeed) {
        if (subCheck) {
            ioL = StudioDspProcessor::SoftClip(subLpL2.ProcessDirect(subLpL1.ProcessDirect(ioL)) * 1.45f);
            ioR = StudioDspProcessor::SoftClip(subLpR2.ProcessDirect(subLpR1.ProcessDirect(ioR)) * 1.45f);
        }

        if (phoneSim) {
            float mono = (ioL + ioR) * 0.5f;
            float pL = phoneMidL.ProcessDirect(phoneLpL.ProcessDirect(phoneHpL.ProcessDirect(mono)));
            float pR = phoneMidR.ProcessDirect(phoneLpR.ProcessDirect(phoneHpR.ProcessDirect(mono)));
            ioL = AnalogPreampProcessor::FastTanh(pL * 1.45f) * 0.85f;
            ioR = AnalogPreampProcessor::FastTanh(pR * 1.45f) * 0.85f;
        }

        if (crossfeed) {
            // Filtra graves/medios abaixo de 700Hz e atrasa 290us antes de cruzar para o ouvido oposto (-12 dB)
            xfeedLpL += xfeedAlpha * (ioL - xfeedLpL);
            xfeedLpR += xfeedAlpha * (ioR - xfeedLpR);

            delayLineL[delayIdx] = xfeedLpL;
            delayLineR[delayIdx] = xfeedLpR;
            size_t readIdx = (delayIdx + kCrossDelayLen - delaySamples) & (kCrossDelayLen - 1);
            float delayedL = delayLineL[readIdx];
            float delayedR = delayLineR[readIdx];
            delayIdx = (delayIdx + 1) & (kCrossDelayLen - 1);

            float outL = ioL * 0.88f + delayedR * 0.22f;
            float outR = ioR * 0.88f + delayedL * 0.22f;
            ioL = outL;
            ioR = outR;
        }
    }
};
