// formant.cpp — Unified Formant Synth with EBU R128 Loudness, stdin/stdout, and CLI args
// Build: make  (expects -std=c++20 -O2 -Wall -Wextra)

#include <cstdint>
#include <cmath>
#include <vector>
#include <fstream>
#include <iostream>
#include <numbers>
#include <array>
#include <algorithm>
#include <string>
#include <sstream>

// ============ Constants & Types ============

constexpr double kSampleRate  = 44100.0;
constexpr double kPi          = std::numbers::pi;
constexpr int    kFrameSize   = 220;            // 5 ms at 44.1 kHz

enum class SourceType { Impulse, Noise, Mixed };

struct FormantParams {
    double f1, f2, f3;
    double bw1, bw2, bw3;
    double gain = 1.0;
    SourceType source = SourceType::Impulse;
};

static FormantParams lerp(const FormantParams& a, const FormantParams& b, double t) {
    return {
        a.f1 + (b.f1 - a.f1) * t,
        a.f2 + (b.f2 - a.f2) * t,
        a.f3 + (b.f3 - a.f3) * t,
        a.bw1 + (b.bw1 - a.bw1) * t,
        a.bw2 + (b.bw2 - a.bw2) * t,
        a.bw3 + (b.bw3 - a.bw3) * t,
        a.gain + (b.gain - a.gain) * t,
        a.source,
    };
}

// ============ Smoother ============
struct Smoother {
    double current = 0.0;
    double target = 0.0;
    double alpha = 0.02;

    void init(double val, double a) { current = target = val; alpha = a; }
    void setAlpha(double a) { alpha = a; }
    void setTarget(double t) { target = t; }
    double process() {
        current += alpha * (target - current);
        return current;
    }
};

struct FormantSmoother {
    Smoother f1, f2, f3;
    Smoother bw1, bw2, bw3;
    Smoother gain;

    void init(const FormantParams& p, double alpha) {
        f1.init(p.f1, alpha); f2.init(p.f2, alpha); f3.init(p.f3, alpha);
        bw1.init(p.bw1, alpha); bw2.init(p.bw2, alpha); bw3.init(p.bw3, alpha);
        gain.init(p.gain, alpha);
    }

    void setTargets(const FormantParams& p) {
        f1.setTarget(p.f1); f2.setTarget(p.f2); f3.setTarget(p.f3);
        bw1.setTarget(p.bw1); bw2.setTarget(p.bw2); bw3.setTarget(p.bw3);
        gain.setTarget(p.gain);
    }

    FormantParams process() {
        return {
            f1.process(), f2.process(), f3.process(),
            bw1.process(), bw2.process(), bw3.process(),
            gain.process(),
            SourceType::Impulse
        };
    }
};

// ============ Resonator ============
struct Resonator {
    double z1 = 0, z2 = 0;
    double a0 = 0, b1 = 0, b2 = 0;

    void set(double freq, double bw, double fs) {
        double R = std::exp(-kPi * bw / fs);
        b1 = -2.0 * R * std::cos(2.0 * kPi * freq / fs);
        b2 = R * R;
        a0 = 1.0 + b1 + b2;
    }

    double process(double input) {
        double y = a0 * input - b1 * z1 - b2 * z2;
        z2 = z1;
        z1 = y;
        return y;
    }
};

// ============ Source Generation ============
struct NoiseGen {
    uint32_t seed = 22695477;
    double next() {
        seed = seed * 1664525 + 1013904223;
        return static_cast<double>(static_cast<int32_t>(seed)) / 2147483648.0;
    }
};

struct ImpulseTrain {
    double phase = 0.0;
    double next(double f0, double fs) {
        phase += f0 / fs;
        if (phase >= 1.0) {
            phase -= 1.0;
            return 1.0;
        }
        return 0.0;
    }
};

// ============ Synthesizer ============
struct PhonemeEntry {
    FormantParams params;
    int duration_samples;
};

class Synthesizer {
public:
    double alpha = 0.02;
    double f1_min = 220.0;
    double f0 = 160.0;

    void synthesize(const std::vector<PhonemeEntry>& sequence,
                    std::vector<int16_t>& output) {
        int totalSamples = 0;
        for (auto& e : sequence) totalSamples += e.duration_samples;
        output.resize(totalSamples);
        if (sequence.empty()) return;

        FormantSmoother smoother;
        smoother.init(sequence[0].params, alpha);

        int pos = 0;
        SourceType currentSource = sequence[0].params.source;

        for (std::size_t seg = 0; seg < sequence.size(); ++seg) {
            auto cur = sequence[seg].params;
            cur.f1 = std::max(f1_min, cur.f1);
            const auto& nxt_raw = (seg + 1 < sequence.size())
                                ? sequence[seg + 1].params : cur;
            int dur = sequence[seg].duration_samples;
            currentSource = cur.source;

            for (int n = 0; n < dur; ++n) {
                if (n % kFrameSize == 0) {
                    double t = static_cast<double>(n) / dur;
                    double blend = (t > 0.7) ? (t - 0.7) / 0.3 : 0.0;
                    auto target_p = lerp(cur, nxt_raw, blend);
                    target_p.f1 = std::max(f1_min, target_p.f1);
                    smoother.setTargets(target_p);
                }

                FormantParams p = smoother.process();

                filters_[0].set(p.f1, p.bw1, kSampleRate);
                filters_[1].set(p.f2, p.bw2, kSampleRate);
                filters_[2].set(p.f3, p.bw3, kSampleRate);

                double s;
                switch (currentSource) {
                    case SourceType::Noise: s = noise_.next(); break;
                    case SourceType::Mixed:
                        s = 0.6 * noise_.next() + 0.4 * impulse_.next(f0, kSampleRate);
                        break;
                    default: s = impulse_.next(f0, kSampleRate); break;
                }

                for (auto& f : filters_) s = f.process(s);

                double out = s * 32767.0 * p.gain;
                out = std::clamp(out, -32768.0, 32767.0);
                output[pos++] = static_cast<int16_t>(out);
            }
        }
    }

private:
    std::array<Resonator, 3> filters_{};
    ImpulseTrain impulse_;
    NoiseGen noise_;
};

// ============ Professional Loudness Normalization ============
static void normalizeLoudnessEBU(std::vector<int16_t>& samples, double targetLUFS) {
    if (samples.empty()) return;

    std::vector<double> filtered(samples.size());
    double x1 = 0, y1 = 0;
    const double b0 = 1.0, b1 = -1.0, a1 = -0.7; 
    for (std::size_t i = 0; i < samples.size(); ++i) {
        double x = static_cast<double>(samples[i]) / 32768.0;
        double y = b0 * x + b1 * x1 - a1 * y1;
        filtered[i] = y;
        x1 = x;
        y1 = y;
    }

    double sumSq = 0.0;
    for (double val : filtered) {
        sumSq += val * val;
    }
    double meanSq = sumSq / samples.size();
    if (meanSq < 1e-9) return;

    double currentLUFS = -0.691 + 10.0 * std::log10(meanSq);
    double gainDB = targetLUFS - currentLUFS;
    double scale = std::pow(10.0, gainDB / 20.0);

    for (int16_t& s : samples) {
        double scaled = static_cast<double>(s) * scale;
        scaled = std::clamp(scaled, -32768.0, 32767.0);
        s = static_cast<int16_t>(scaled);
    }
}

// ============ Phoneme Data & Parameters ============
constexpr FormantParams kVowelA = {800, 1200, 2600, 80, 100, 120};
constexpr FormantParams kVowelI = {300, 2300, 3000, 80, 120, 150};
constexpr FormantParams kVowelU = {350, 1300, 2500, 80, 100, 120};
constexpr FormantParams kVowelE = {500, 1900, 2600, 80, 100, 120};
constexpr FormantParams kVowelO = {500,  800, 2400, 80, 100, 120};

static const FormantParams* const kVowels[] = {&kVowelA, &kVowelI, &kVowelU, &kVowelE, &kVowelO};

constexpr FormantParams kSemiJ = {280, 2300, 3000, 80, 100, 120, 1.0, SourceType::Impulse};
constexpr FormantParams kSemiW = {320, 750,  2300, 80, 100, 120, 1.0, SourceType::Impulse};
constexpr FormantParams kTapR  = {350, 1500, 2500, 80, 120, 150, 0.3, SourceType::Impulse};
constexpr FormantParams kNasalM = {250, 1000, 2200, 180, 200, 250, 0.3, SourceType::Impulse};
constexpr FormantParams kNasalN = {250, 1700, 2600, 180, 200, 250, 0.3, SourceType::Impulse};

// 口蓋化鼻音・流音パラメータ（拗音用）
constexpr FormantParams kNasalNy = {250, 2400, 2900, 180, 200, 250, 0.3, SourceType::Impulse};
constexpr FormantParams kNasalMy = {250, 2000, 2600, 180, 200, 250, 0.3, SourceType::Impulse};
constexpr FormantParams kTapRy  = {350, 2200, 2900, 80, 120, 150, 0.3, SourceType::Impulse};

// 摩擦音・歯擦音
constexpr FormantParams kFricH_A = {800, 1200, 2600, 200, 300, 400, 0.15, SourceType::Noise};
constexpr FormantParams kFricS   = {200, 5500, 7500, 500, 3000, 2000, 0.4, SourceType::Noise};
constexpr FormantParams kFricSh  = {200, 3800, 6000, 500, 2500, 2000, 0.4, SourceType::Noise};
constexpr FormantParams kFricChi = {200, 3000, 5000, 500, 2000, 2000, 0.3, SourceType::Noise};
constexpr FormantParams kFricPhi = {200, 2500, 4000, 500, 3000, 2000, 0.15, SourceType::Noise};
constexpr FormantParams kFricH_E = {500, 1900, 2600, 200, 300, 400, 0.15, SourceType::Noise};
constexpr FormantParams kFricH_O = {500,  800, 2400, 200, 300, 400, 0.15, SourceType::Noise};
constexpr FormantParams kFricZ   = {200, 5500, 7500, 500, 3000, 2000, 0.4, SourceType::Noise};
constexpr FormantParams kFricZh  = {200, 3800, 6000, 500, 2500, 2000, 0.4, SourceType::Noise};

// 有声破裂音用の VoiceBar
constexpr FormantParams kVoiceBarG = {250, 1200, 2400, 100, 200, 300, 0.25, SourceType::Impulse};
constexpr FormantParams kVoiceBarD = {250, 1700, 2600, 100, 200, 300, 0.25, SourceType::Impulse};
constexpr FormantParams kVoiceBarB = {250, 800,  2200, 100, 200, 300, 0.25, SourceType::Impulse};

// 破裂音バースト
constexpr FormantParams kSilence   = {100, 100, 100, 100, 100, 100, 0.0, SourceType::Impulse};
constexpr FormantParams kBurstP   = {300, 1000, 2300, 500, 1500, 2000, 0.30, SourceType::Noise};
constexpr FormantParams kBurstB   = {300, 900,  2200, 500, 1500, 2000, 0.35, SourceType::Noise};
constexpr FormantParams kBurstT   = {300, 4000, 5000, 500, 2000, 2000, 0.35, SourceType::Noise};
constexpr FormantParams kBurstD   = {300, 3500, 4800, 500, 2000, 2000, 0.35, SourceType::Noise};
constexpr FormantParams kBurstK   = {300, 1800, 2600, 500, 2000, 2000, 0.30, SourceType::Noise};
constexpr FormantParams kBurstG   = {300, 1500, 2400, 500, 2000, 2000, 0.35, SourceType::Noise};
constexpr FormantParams kBurstTCh = {300, 3800, 6000, 500, 2000, 2000, 0.35, SourceType::Noise};
constexpr FormantParams kBurstTs  = {300, 5500, 7500, 500, 2500, 2000, 0.35, SourceType::Noise};

constexpr FormantParams kVotP = {800, 1200, 2600, 300, 400, 500, 0.10, SourceType::Noise};
constexpr FormantParams kVotT = {800, 1200, 2600, 300, 400, 500, 0.10, SourceType::Noise};
constexpr FormantParams kVotK = {800, 1200, 2600, 300, 400, 500, 0.10, SourceType::Noise};

constexpr int kSokuonSamples = 5292;
constexpr int kHatsuonSamples = 3528;

inline constexpr int ms2s(int ms) {
    return static_cast<int>(kSampleRate * ms / 1000.0);
}

static std::vector<PhonemeEntry> makePlosiveCV(
    int closure_ms, int burst_ms, int vot_ms,
    const FormantParams& closure_params,
    const FormantParams& burst_params,
    const FormantParams& vot_params,
    const FormantParams& vowel, int vowel_ms)
{
    std::vector<PhonemeEntry> result;
    if (closure_ms > 0) result.push_back({closure_params, ms2s(closure_ms)});
    if (burst_ms > 0)    result.push_back({burst_params,   ms2s(burst_ms)});
    if (vot_ms > 0)      result.push_back({vot_params,      ms2s(vot_ms)});
    result.push_back({vowel, ms2s(vowel_ms)});
    return result;
}

static void append(std::vector<PhonemeEntry>& seq, const std::vector<PhonemeEntry>& entries) {
    seq.insert(seq.end(), entries.begin(), entries.end());
}

enum CType : int8_t {
    V = 0, K, G, T, D, P, B,
    CH, TS, DZH, DZ, S, SH,
    HA, HI, HU, HE, HO,
    NN, M, R, J, W, Q, HN, SK
};

struct KanaEntry {
    CType c;
    int vi;
};

// 完全修正された kKanaTable (Unicode 0x3041 〜 0x3093 の全83音を完全マッピング)
static constexpr KanaEntry kKanaTable[83] = {
    {V,0},{V,0},{V,1},{V,1},{V,2},{V,2},{V,3},{V,3},{V,4},{V,4}, // 00-09: ぁあぃいぅうぇえぉお
    {K,0},{G,0},{K,1},{G,1},{K,2},{G,2},{K,3},{G,3},{K,4},{G,4}, // 10-19: かがきぎくぐけげこご
    {S,0},{DZ,0},{SH,1},{DZH,1},{S,2},{DZ,2},{S,3},{DZ,3},{S,4},{DZ,4}, // 20-29: さざしじすずせぜそぞ
    {T,0},{D,0},{CH,1},{DZH,1},{Q,-1},{TS,2},{DZ,2},{T,3},{D,3},{T,4},{D,4}, // 30-40: ただちぢっつづてでとど
    {NN,0},{NN,1},{NN,2},{NN,3},{NN,4},                           // 41-45: なにぬねの
    {HA,0},{B,0},{P,0},{HI,1},{B,1},{P,1},{HU,2},{B,2},{P,2},{HE,3},{B,3},{P,3},{HO,4},{B,4},{P,4}, // 46-60: はばぱひびぴふぶぷへべぺほぼぽ
    {M,0},{M,1},{M,2},{M,3},{M,4},                               // 61-65: まみむめも
    {SK,-1},{J,0},{SK,-1},{J,2},{SK,-1},{J,4},                   // 66-71: ゃやゅゆょよ
    {R,0},{R,1},{R,2},{R,3},{R,4},                               // 72-76: らりるれろ
    {SK,-1},{W,0},{SK,-1},{SK,-1},{V,4},{HN,-1},                 // 77-82: ゎわゐゑをん
};
/*
static void emitCV(std::vector<PhonemeEntry>& seq, CType c, int vi, bool is_y_on) {
    const auto& v = *kVowels[vi];
    constexpr int vMs = 120;

    switch (c) {
        case V:
            seq.push_back({v, ms2s(vMs)});
            break;
        case K: {
            double k_f2[5] = {1500.0, 2300.0, 1200.0, 1800.0, 1400.0};
            FormantParams closure = {200.0, 800.0, 2000.0, 200.0, 300.0, 400.0, 0.0, SourceType::Impulse};
            FormantParams burst   = {400.0, k_f2[vi], 2800.0, 300.0, 500.0, 600.0, 0.4, SourceType::Noise};
            FormantParams vot     = {v.f1, k_f2[vi], v.f3, 100.0, 200.0, 300.0, 0.15, SourceType::Noise};
            append(seq, makePlosiveCV(60, 12, 25, closure, burst, vot, v, vMs));
            break;
        }
        case G: {
            // が行：軟口蓋有声破裂音（F2を両唇音から完全に分離し、固有の周波数に設定）
            double g_f2[5] = {1500.0, 2200.0, 1300.0, 1800.0, 1400.0};
            FormantParams voicebar = {250.0, 1100.0, 2400.0, 100.0, 150.0, 200.0, 0.3, SourceType::Impulse};
            FormantParams burst    = {350.0, g_f2[vi], 2600.0, 300.0, 500.0, 600.0, 0.35, SourceType::Mixed};
            append(seq, makePlosiveCV(50, 15, 0, voicebar, burst, burst, v, vMs));
            break;
        }
        case T: {
            double t_f2[5] = {3500.0, 4200.0, 3200.0, 3800.0, 3600.0};
            FormantParams closure = {200.0, 1500.0, 2500.0, 200.0, 300.0, 400.0, 0.0, SourceType::Impulse};
            FormantParams burst   = {400.0, t_f2[vi], 5000.0, 300.0, 500.0, 600.0, 0.4, SourceType::Noise};
            FormantParams vot     = {v.f1, t_f2[vi], v.f3, 100.0, 200.0, 300.0, 0.15, SourceType::Noise};
            append(seq, makePlosiveCV(60, 10, 20, closure, burst, vot, v, vMs));
            break;
        }
        case D: {
            double d_f2[5] = {3200.0, 3900.0, 3000.0, 3500.0, 3300.0};
            FormantParams voicebar = {250.0, 1200.0, 2400.0, 100.0, 150.0, 200.0, 0.25, SourceType::Impulse};
            FormantParams burst    = {350.0, d_f2[vi], 4800.0, 300.0, 500.0, 600.0, 0.35, SourceType::Mixed};
            append(seq, makePlosiveCV(50, 10, 0, voicebar, burst, burst, v, vMs));
            break;
        }
        case P: {
            FormantParams closure = {200.0, 500.0, 1500.0, 200.0, 300.0, 400.0, 0.0, SourceType::Impulse};
            FormantParams burst   = {300.0, 1000.0, 2300.0, 300.0, 500.0, 600.0, 0.4, SourceType::Noise};
            FormantParams vot     = {v.f1, 1000.0, v.f3, 100.0, 200.0, 300.0, 0.1, SourceType::Noise};
            append(seq, makePlosiveCV(60, 10, 15, closure, burst, vot, v, vMs));
            break;
        }
        case B: {
            FormantParams voicebar = {250.0, 800.0,  2200.0, 100.0, 150.0, 200.0, 0.25, SourceType::Impulse};
            FormantParams burst    = {300.0, 900.0,  2200.0, 300.0, 500.0, 600.0, 0.35, SourceType::Mixed};
            append(seq, makePlosiveCV(50, 10, 0, voicebar, burst, burst, v, vMs));
            break;
        }
        case S:    
            seq.insert(seq.end(), {{kFricS, ms2s(100)}, {v, ms2s(vMs)}}); 
            break;
        case SH:   
            // しゃ行の拗音時は硬口蓋化された専用の強い摩擦音を適用
            {
                FormantParams fric = is_y_on 
                    ? FormantParams{200.0, 4000.0, 6200.0, 400.0, 2000.0, 1800.0, 0.45, SourceType::Noise}
                    : kFricSh;
                seq.insert(seq.end(), {{fric, ms2s(100)}, {v, ms2s(vMs)}});
            }
            break;
        case CH:   
            // ちゃ行：硬口蓋破擦音
            {
                FormantParams burst_ch = is_y_on
                    ? FormantParams{300.0, 4000.0, 6200.0, 400.0, 1800.0, 1800.0, 0.4, SourceType::Noise}
                    : kBurstTCh;
                append(seq, makePlosiveCV(70, 5, 70, kSilence, burst_ch, kFricSh, v, vMs));
            }
            break;
        case TS:   
            append(seq, makePlosiveCV(70, 5, 70, kSilence, kBurstTs, kFricS, v, vMs)); 
            break;
        case DZH:  
            append(seq, makePlosiveCV(40, 5, 60, kVoiceBarD, kBurstTCh, kFricZh, v, vMs)); 
            break;
        case DZ:   
            append(seq, makePlosiveCV(40, 5, 50, kVoiceBarD, kBurstTs, kFricZ, v, vMs)); 
            break;
        case HA:   
            seq.insert(seq.end(), {{kFricH_A, ms2s(80)}, {v, ms2s(vMs)}}); 
            break;
        case HI:   
            // ひゃ行：硬口蓋化された摩擦音
            {
                FormantParams fric_hi = is_y_on
                    ? FormantParams{200.0, 3800.0, 5800.0, 400.0, 2000.0, 1800.0, 0.35, SourceType::Noise}
                    : kFricChi;
                seq.insert(seq.end(), {{fric_hi, ms2s(90)}, {v, ms2s(vMs)}});
            }
            break;
        case HU:   
            seq.insert(seq.end(), {{kFricPhi, ms2s(90)}, {v, ms2s(vMs)}}); 
            break;
        case HE:   
            seq.insert(seq.end(), {{kFricH_E, ms2s(80)}, {v, ms2s(vMs)}}); 
            break;
        case HO:   
            seq.insert(seq.end(), {{kFricH_O, ms2s(80)}, {v, ms2s(vMs)}}); 
            break;
        case NN:   {
            // にゃ行：口蓋化された鼻音（や行に化けないようF2を明確に高域に固定）
            FormantParams nasal = is_y_on 
                ? FormantParams{250.0, 2600.0, 3100.0, 150.0, 200.0, 250.0, 0.4, SourceType::Impulse}
                : FormantParams{250.0, 1700.0, 2600.0, 150.0, 200.0, 250.0, 0.4, SourceType::Impulse};
            seq.insert(seq.end(), {{nasal, ms2s(80)}, {v, ms2s(vMs)}});
            break;
        }
        case M: {
            // みゃ行：口蓋化された両唇鼻音（ら行に化けないよう独立した共鳴を定義）
            FormantParams nasal_m = is_y_on
                ? FormantParams{250.0, 2300.0, 2800.0, 150.0, 200.0, 250.0, 0.45, SourceType::Impulse}
                : FormantParams{250.0, 1000.0, 2200.0, 150.0, 200.0, 250.0, 0.45, SourceType::Impulse};
            seq.insert(seq.end(), {{nasal_m, ms2s(80)}, {v, ms2s(vMs)}});
            break;
        }
        case R: {
            // らい・りゃ行：弾き音
            FormantParams tap = is_y_on
                ? FormantParams{400.0, 2400.0, 3000.0, 100.0, 150.0, 200.0, 0.3, SourceType::Impulse}
                : FormantParams{400.0, 1500.0, 2500.0, 100.0, 150.0, 200.0, 0.3, SourceType::Impulse};
            seq.insert(seq.end(), {{tap, ms2s(30)}, {v, ms2s(vMs)}});
            break;
        }
        case J:    
            seq.insert(seq.end(), {{kSemiJ, ms2s(60)}, {v, ms2s(vMs)}}); 
            break;
        case W:    
            seq.insert(seq.end(), {{kSemiW, ms2s(60)}, {v, ms2s(vMs)}}); 
            break;
        default:   
            seq.push_back({v, ms2s(vMs)}); 
            break;
    }
}
*/

static void emitCV(std::vector<PhonemeEntry>& seq, CType c, int vi, bool is_y_on) {
    const auto& v = *kVowels[vi];
    constexpr int vMs = 120;

    switch (c) {
        case V:
            seq.push_back({v, ms2s(vMs)});
            break;
        case K: {
            // か行：軟口蓋破裂音（閉鎖のF2を1600Hzにし、両唇音の800Hzと完全に分離）
            double k_f2[5] = {1500.0, 2300.0, 1300.0, 1800.0, 1400.0};
            double closure_f2 = is_y_on ? 2100.0 : 1600.0;
            FormantParams closure = {200.0, closure_f2, 2500.0, 200.0, 300.0, 400.0, 0.0, SourceType::Impulse};
            FormantParams burst   = {400.0, k_f2[vi], 2800.0, 300.0, 500.0, 600.0, 0.4, SourceType::Noise};
            FormantParams vot     = {v.f1, k_f2[vi], v.f3, 100.0, 200.0, 300.0, 0.15, SourceType::Noise};
            append(seq, makePlosiveCV(60, 12, 25, closure, burst, vot, v, vMs));
            break;
        }
        case G: {
            // が行：軟口蓋有声破裂音
            double g_f2[5] = {1400.0, 2100.0, 1200.0, 1700.0, 1300.0};
            double voice_f2 = is_y_on ? 2000.0 : 1400.0;
            FormantParams voicebar = {250.0, voice_f2, 2400.0, 100.0, 150.0, 200.0, 0.3, SourceType::Impulse};
            FormantParams burst    = {350.0, g_f2[vi], 2600.0, 300.0, 500.0, 600.0, 0.35, SourceType::Mixed};
            append(seq, makePlosiveCV(50, 15, 0, voicebar, burst, burst, v, vMs));
            break;
        }
        case T: {
            double t_f2[5] = {3500.0, 4200.0, 3200.0, 3800.0, 3600.0};
            FormantParams closure = {200.0, 1800.0, 2800.0, 200.0, 300.0, 400.0, 0.0, SourceType::Impulse};
            FormantParams burst   = {400.0, t_f2[vi], 5000.0, 300.0, 500.0, 600.0, 0.4, SourceType::Noise};
            FormantParams vot     = {v.f1, t_f2[vi], v.f3, 100.0, 200.0, 300.0, 0.15, SourceType::Noise};
            append(seq, makePlosiveCV(60, 10, 20, closure, burst, vot, v, vMs));
            break;
        }
        case D: {
            double d_f2[5] = {3200.0, 3900.0, 3000.0, 3500.0, 3300.0};
            FormantParams voicebar = {250.0, 1500.0, 2500.0, 100.0, 150.0, 200.0, 0.25, SourceType::Impulse};
            FormantParams burst    = {350.0, d_f2[vi], 4800.0, 300.0, 500.0, 600.0, 0.35, SourceType::Mixed};
            append(seq, makePlosiveCV(50, 10, 0, voicebar, burst, burst, v, vMs));
            break;
        }
        case P: {
            // ぱ行：両唇閉鎖（F2=800Hzで固定し、か行と完全に差別化）
            FormantParams closure = {200.0, 800.0, 1800.0, 200.0, 300.0, 400.0, 0.0, SourceType::Impulse};
            FormantParams burst   = {300.0, 1000.0, 2300.0, 300.0, 500.0, 600.0, 0.4, SourceType::Noise};
            FormantParams vot     = {v.f1, 1000.0, v.f3, 100.0, 200.0, 300.0, 0.1, SourceType::Noise};
            append(seq, makePlosiveCV(60, 10, 15, closure, burst, vot, v, vMs));
            break;
        }
        case B: {
            FormantParams voicebar = {250.0, 800.0, 2000.0, 100.0, 150.0, 200.0, 0.25, SourceType::Impulse};
            FormantParams burst    = {300.0, 900.0, 2200.0, 300.0, 500.0, 600.0, 0.35, SourceType::Mixed};
            append(seq, makePlosiveCV(50, 10, 0, voicebar, burst, burst, v, vMs));
            break;
        }
        case S:    
            seq.insert(seq.end(), {{kFricS, ms2s(100)}, {v, ms2s(vMs)}}); 
            break;
        case SH: {
            // しゃ行・ひゃ行等の後続：口蓋化摩擦音
            FormantParams fric = is_y_on 
                ? FormantParams{300.0, 3500.0, 5500.0, 400.0, 1500.0, 1500.0, 0.45, SourceType::Noise}
                : kFricSh;
            seq.insert(seq.end(), {{fric, ms2s(100)}, {v, ms2s(vMs)}});
            break;
        }
        case CH: {
            FormantParams burst_ch = is_y_on
                ? FormantParams{400.0, 3500.0, 5500.0, 400.0, 1500.0, 1500.0, 0.4, SourceType::Noise}
                : kBurstTCh;
            append(seq, makePlosiveCV(70, 5, 70, kSilence, burst_ch, kFricSh, v, vMs));
            break;
        }
        case TS:   
            append(seq, makePlosiveCV(70, 5, 70, kSilence, kBurstTs, kFricS, v, vMs)); 
            break;
        case DZH:  
            append(seq, makePlosiveCV(40, 5, 60, kVoiceBarD, kBurstTCh, kFricZh, v, vMs)); 
            break;
        case DZ:   
            append(seq, makePlosiveCV(40, 5, 50, kVoiceBarD, kBurstTs, kFricZ, v, vMs)); 
            break;
        case HA:   
            seq.insert(seq.end(), {{kFricH_A, ms2s(80)}, {v, ms2s(vMs)}}); 
            break;
        case HI: {
            FormantParams fric_hi = is_y_on
                ? FormantParams{300.0, 3200.0, 5000.0, 400.0, 1500.0, 1500.0, 0.35, SourceType::Noise}
                : kFricChi;
            seq.insert(seq.end(), {{fric_hi, ms2s(90)}, {v, ms2s(vMs)}});
            break;
        }
        case HU:   
            seq.insert(seq.end(), {{kFricPhi, ms2s(90)}, {v, ms2s(vMs)}}); 
            break;
        case HE:   
            seq.insert(seq.end(), {{kFricH_E, ms2s(80)}, {v, ms2s(vMs)}}); 
            break;
        case HO:   
            seq.insert(seq.end(), {{kFricH_O, ms2s(80)}, {v, ms2s(vMs)}}); 
            break;
        case NN: {
            // にゃ行：口蓋化鼻音（や行に化けないようF2を高めに維持）
            FormantParams nasal = is_y_on 
                ? FormantParams{250.0, 2200.0, 2900.0, 150.0, 200.0, 250.0, 0.4, SourceType::Impulse}
                : FormantParams{250.0, 1700.0, 2600.0, 150.0, 200.0, 250.0, 0.4, SourceType::Impulse};
            seq.insert(seq.end(), {{nasal, ms2s(80)}, {v, ms2s(vMs)}});
            break;
        }
        case M: {
            // みゃ行：口蓋化両唇鼻音（ら行に化けないよう独立定義）
            FormantParams nasal_m = is_y_on
                ? FormantParams{250.0, 1900.0, 2600.0, 150.0, 200.0, 250.0, 0.45, SourceType::Impulse}
                : FormantParams{250.0, 1000.0, 2200.0, 150.0, 200.0, 250.0, 0.45, SourceType::Impulse};
            seq.insert(seq.end(), {{nasal_m, ms2s(80)}, {v, ms2s(vMs)}});
            break;
        }
        case R: {
            FormantParams tap = is_y_on
                ? FormantParams{400.0, 2100.0, 2800.0, 100.0, 150.0, 200.0, 0.3, SourceType::Impulse}
                : FormantParams{400.0, 1500.0, 2500.0, 100.0, 150.0, 200.0, 0.3, SourceType::Impulse};
            seq.insert(seq.end(), {{tap, ms2s(30)}, {v, ms2s(vMs)}});
            break;
        }
        case J:    
            seq.insert(seq.end(), {{kSemiJ, ms2s(60)}, {v, ms2s(vMs)}}); 
            break;
        case W:    
            seq.insert(seq.end(), {{kSemiW, ms2s(60)}, {v, ms2s(vMs)}}); 
            break;
        default:   
            seq.push_back({v, ms2s(vMs)}); 
            break;
    }
}

static std::vector<PhonemeEntry> textToPhoneme(const std::string& text) {
    auto decode3 = [](const std::string& s, std::size_t i) -> int {
        auto b0 = static_cast<unsigned char>(s[i]);
        auto b1 = static_cast<unsigned char>(s[i+1]);
        auto b2 = static_cast<unsigned char>(s[i+2]);
        if ((b0 & 0xF0) != 0xE0) return -1;
        return ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
    };

    std::vector<PhonemeEntry> seq;
    std::size_t pos = 0;
    int lastVowel = 0;

    while (pos + 3 <= text.size()) {
        int cp = decode3(text, pos);
        if (cp < 0) { pos++; continue; }
        pos += 3;

        if (cp == 0x30FC) {
            seq.push_back({*kVowels[lastVowel], ms2s(120)});
            continue;
        }

        if (cp < 0x3041 || cp > 0x3093) continue;
        auto [c, vi] = kKanaTable[cp - 0x3041];

        if (c == SK) continue;
        if (c == Q) { seq.push_back({kSilence, kSokuonSamples}); continue; }
        if (c == HN) { seq.push_back({kNasalN, kHatsuonSamples}); continue; }

        bool is_y_on = false;
        if (vi == 1 && c != V && pos + 3 <= text.size()) {
            int ncp = decode3(text, pos);
            if (ncp >= 0) {
                if      (ncp == 0x3083) { vi = 0; is_y_on = true; pos += 3; }
                else if (ncp == 0x3085) { vi = 2; is_y_on = true; pos += 3; }
                else if (ncp == 0x3087) { vi = 4; is_y_on = true; pos += 3; }
            }
        }

        emitCV(seq, c, vi, is_y_on);
        if (vi >= 0) lastVowel = vi;
    }
    return seq;
}

// ============ WAV Writer ============
static bool writeWavToStream(std::ostream& ofs,
                           const std::vector<int16_t>& samples,
                           int sampleRate) {
    auto write16 = [&](uint16_t v) { ofs.write(reinterpret_cast<const char*>(&v), 2); };
    auto write32 = [&](uint32_t v) { ofs.write(reinterpret_cast<const char*>(&v), 4); };

    uint32_t dataSize = static_cast<uint32_t>(samples.size()) * 2;
    uint32_t fileSize = 36 + dataSize;

    ofs.write("RIFF", 4);
    write32(fileSize);
    ofs.write("WAVE", 4);

    ofs.write("fmt ", 4);
    write32(16);
    write16(1);
    write16(1);
    write32(static_cast<uint32_t>(sampleRate));
    write32(static_cast<uint32_t>(sampleRate) * 2);
    write16(2);
    write16(16);

    ofs.write("data", 4);
    write32(dataSize);
    ofs.write(reinterpret_cast<const char*>(samples.data()),
              static_cast<std::streamsize>(dataSize));

    return true;
}

// ============ Help Message ============
static void printHelp(const char* progName) {
    std::cout << "Usage: " << progName << " [options] [hiragana text]\n"
              << "日本語フォルマント音声合成エンジン (全発音網羅修正版)\n\n"
              << "オプション:\n"
              << "  -l <val>      目標ラウドネス (LUFS近似, デフォルト: -14.0)\n"
              << "  -a <val>      IIRパラメータ平滑化 alpha (デフォルト: 0.02)\n"
              << "  --f1min <val> 第1フォルマント下限 Hz (デフォルト: 220.0)\n"
              << "  --f0 <val>    基本周波数 F0 Hz (デフォルト: 160.0)\n"
              << "  -o [path]     WAVファイル出力\n"
              << "  -h, --help    ヘルプを表示\n";
}

#if defined(_MSC_VER) || defined(_WIN32)
#include <io.h>
#define ISATTY _isatty
#define FILENO _fileno
#else
#include <unistd.h>
#define ISATTY isatty
#define FILENO fileno
#endif

// ============ Main ============
#ifndef TEST_BUILD
int main(int argc, char* argv[]) {
    double targetLoudness = -14.0;
    double alpha = 0.02;
    double f1min = 220.0;
    double f0 = 160.0;
    std::string text = "";
    std::string outPath = "";
    bool hasOutputOption = false;
    bool outputDefaultName = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printHelp(argv[0]);
            return 0;
        } else if (arg == "-l" || arg == "-a" || arg == "--f1min" || arg == "--f0") {
            if (i + 1 >= argc) {
                std::cerr << "Error: option " << arg << " requires a value.\n\n";
                printHelp(argv[0]);
                return 1;
            }
            try {
                size_t idx = 0;
                double val = std::stod(argv[++i], &idx);
                if (idx != std::string(argv[i]).size()) {
                    std::cerr << "Error: invalid numeric value for " << arg << ": " << argv[i] << "\n\n";
                    printHelp(argv[0]);
                    return 1;
                }
                if (arg == "-l") targetLoudness = val;
                else if (arg == "-a") alpha = val;
                else if (arg == "--f1min") f1min = val;
                else if (arg == "--f0") f0 = val;
            } catch (...) {
                std::cerr << "Error: invalid numeric value for " << arg << ": " << argv[i] << "\n\n";
                printHelp(argv[0]);
                return 1;
            }
        } else if (arg == "-o") {
            hasOutputOption = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            } else {
                outputDefaultName = true;
            }
        } else if (arg[0] == '-') {
            std::cerr << "Error: unknown option: " << arg << "\n\n";
            printHelp(argv[0]);
            return 1;
        } else {
            text = arg;
        }
    }

    if (text.empty()) {
        if (ISATTY(FILENO(stdin))) {
            printHelp(argv[0]);
            return 0;
        }

        std::ostringstream ss;
        ss << std::cin.rdbuf();
        text = ss.str();
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
    }

    if (text.empty()) {
        printHelp(argv[0]);
        return 0;
    }

    auto sequence = textToPhoneme(text);
    if (sequence.empty()) {
        std::cerr << "Error: empty sequence or invalid hiragana text.\n";
        return 1;
    }

    Synthesizer synth;
    synth.alpha = alpha;
    synth.f1_min = f1min;
    synth.f0 = f0;

    std::vector<int16_t> buf;
    synth.synthesize(sequence, buf);

    normalizeLoudnessEBU(buf, targetLoudness);

    if (hasOutputOption) {
        if (outputDefaultName || outPath.empty()) {
            outPath = "output.wav";
        }
        std::ofstream ofs(outPath, std::ios::binary);
        if (!ofs) {
            std::cerr << "Error: cannot open file for writing: " << outPath << "\n";
            return 1;
        }
        writeWavToStream(ofs, buf, static_cast<int>(kSampleRate));
        std::cerr << "Wrote " << outPath << " (" << buf.size() << " samples, "
                  << sequence.size() << " segments)\n";
    } else {
        writeWavToStream(std::cout, buf, static_cast<int>(kSampleRate));
    }

    return 0;
}
#endif // TEST_BUILD
