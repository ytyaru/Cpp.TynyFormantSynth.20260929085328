// formant.cpp — MS6: Hiragana text input (with 1st-order IIR parameter smoothing)
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

// ============ Constants & Types ============

constexpr double kSampleRate  = 44100.0;
constexpr double kF0          = 160.0;
constexpr double kAmplitude   = 0.9 * 32767.0;
constexpr double kPi          = std::numbers::pi;
constexpr int    kFrameSize   = 220;            // 5 ms at 44.1 kHz

enum class SourceType { Impulse, Noise, Mixed };

struct FormantParams {
    double f1, f2, f3;
    double bw1, bw2, bw3;
    double gain = 1.0;
    SourceType source = SourceType::Impulse;
};

// FormantParams 間の線形補間
static FormantParams lerp(const FormantParams& a, const FormantParams& b, double t) {
    return {
        a.f1 + (b.f1 - a.f1) * t,
        a.f2 + (b.f2 - a.f2) * t,
        a.f3 + (b.f3 - a.f3) * t,
        a.bw1 + (b.bw1 - a.bw1) * t,
        a.bw2 + (b.bw2 - a.bw2) * t,
        a.bw3 + (b.bw3 - a.bw3) * t,
        a.gain + (b.gain - a.gain) * t,
        a.source,  // 音源タイプは補間しない（現在のセグメントの値を使用）
    };
}

// ============ 1st-Order IIR Smoother ============
struct SmoothParam {
    double current = 0.0;
    double target = 0.0;
    double alpha = 0.01; // スムージング係数（小さいほどなめらか）

    void setTarget(double t) {
        target = t;
    }

    void init(double val) {
        current = target = val;
    }

    double process() {
        // 1次IIR（指数平滑化）: y[n] = (1 - alpha) * y[n-1] + alpha * x[n]
        current += alpha * (target - current);
        return current;
    }
};

struct SmoothFormantParams {
    SmoothParam f1, f2, f3;
    SmoothParam bw1, bw2, bw3;
    SmoothParam gain;

    void init(const FormantParams& p, double alpha) {
        f1.alpha = f2.alpha = f3.alpha = alpha;
        bw1.alpha = bw2.alpha = bw3.alpha = alpha;
        gain.alpha = alpha * 2.0; // ゲインは少し速めに追従させる

        f1.init(p.f1); f2.init(p.f2); f3.init(p.f3);
        bw1.init(p.bw1); bw2.init(p.bw2); bw3.init(p.bw3);
        gain.init(p.gain);
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
            SourceType::Impulse // プレースホルダー（ソースタイプは別途管理）
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

    void reset() { z1 = z2 = 0; }
};

// ============ Source Generation ============

struct NoiseGen {
    uint32_t seed = 22695477;
    double next() {
        seed = seed * 1664525 + 1013904223;  // LCG
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
    void synthesize(const std::vector<PhonemeEntry>& sequence,
                    std::vector<int16_t>& output) {
        int totalSamples = 0;
        for (auto& e : sequence) totalSamples += e.duration_samples;
        output.resize(totalSamples);

        if (sequence.empty()) return;

        // スムーサーの初期化（alpha = 0.005 ぐらいが音素間のなめらかな遷移に効果的）
        SmoothFormantParams smoother;
        smoother.init(sequence[0].params, 0.005);

        int pos = 0;
        SourceType currentSource = sequence[0].params.source;

        for (std::size_t seg = 0; seg < sequence.size(); ++seg) {
            const auto& cur = sequence[seg].params;
            const auto& nxt = (seg + 1 < sequence.size())
                                ? sequence[seg + 1].params : cur;
            int dur = sequence[seg].duration_samples;
            currentSource = cur.source;

            for (int n = 0; n < dur; ++n) {
                // フレーム境界でパラメータのターゲットを更新
                if (n % kFrameSize == 0) {
                    double t = static_cast<double>(n) / dur;
                    double blend = (t > 0.7) ? (t - 0.7) / 0.3 : 0.0;
                    auto p = lerp(cur, nxt, blend);
                    smoother.setTargets(p);
                }

                // 1次IIRでなめらかに補間されたパラメータを取得
                FormantParams smoothed = smoother.process();

                // フィルタ係数を更新
                filters_[0].set(smoothed.f1, smoothed.bw1, kSampleRate);
                filters_[1].set(smoothed.f2, smoothed.bw2, kSampleRate);
                filters_[2].set(smoothed.f3, smoothed.bw3, kSampleRate);

                // 音源生成
                double s;
                switch (currentSource) {
                    case SourceType::Noise: s = noise_.next(); break;
                    case SourceType::Mixed:
                        s = 0.6 * noise_.next() + 0.4 * impulse_.next(kF0, kSampleRate);
                        break;
                    default: s = impulse_.next(kF0, kSampleRate); break;
                }

                // カスケードフィルタ
                for (auto& f : filters_) s = f.process(s);

                // 出力
                double out = s * kAmplitude * smoothed.gain;
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

// ============ Phoneme Data ============

constexpr FormantParams kVowelA = {800, 1200, 2600, 80, 100, 120};
constexpr FormantParams kVowelI = {300, 2300, 3000, 80, 120, 150};
constexpr FormantParams kVowelU = {350, 1300, 2500, 80, 100, 120};
constexpr FormantParams kVowelE = {500, 1900, 2600, 80, 100, 120};
constexpr FormantParams kVowelO = {500,  800, 2400, 80, 100, 120};

static const FormantParams* const kVowels[] = {&kVowelA, &kVowelI, &kVowelU, &kVowelE, &kVowelO};

// 半母音（音源:インパルス、遷移のみで実現）
constexpr FormantParams kSemiJ = {280, 2300, 3000, 80, 100, 120, 1.0, SourceType::Impulse};
constexpr FormantParams kSemiW = {320, 750,  2300, 80, 100, 120, 1.0, SourceType::Impulse};

// 弾き音（音源:インパルス、短い持続、低ゲイン）
constexpr FormantParams kTapR  = {350, 1500, 2500, 80, 120, 150, 0.3, SourceType::Impulse};

// 鼻音（音源:インパルス、F1=250Hz広帯域、低ゲイン）
constexpr FormantParams kNasalM = {250, 1000, 2200, 180, 200, 250, 0.3, SourceType::Impulse};
constexpr FormantParams kNasalN = {250, 1700, 2600, 180, 200, 250, 0.3, SourceType::Impulse};

// 摩擦音（音源:ノイズ）
constexpr FormantParams kFricH_A = {800, 1200, 2600, 200, 300, 400, 0.15, SourceType::Noise};
constexpr FormantParams kFricS   = {200, 5500, 7500, 500, 3000, 2000, 0.4, SourceType::Noise};
constexpr FormantParams kFricSh  = {200, 3800, 6000, 500, 2500, 2000, 0.4, SourceType::Noise};
constexpr FormantParams kFricChi = {200, 3000, 5000, 500, 2000, 2000, 0.3, SourceType::Noise};
constexpr FormantParams kFricPhi = {200, 2500, 4000, 500, 3000, 2000, 0.15, SourceType::Noise};
constexpr FormantParams kFricH_E = {500, 1900, 2600, 200, 300, 400, 0.15, SourceType::Noise};
constexpr FormantParams kFricH_O = {500,  800, 2400, 200, 300, 400, 0.15, SourceType::Noise};
constexpr FormantParams kFricZ   = {200, 5500, 7500, 500, 3000, 2000, 0.3, SourceType::Mixed};
constexpr FormantParams kFricZh  = {200, 3800, 6000, 500, 2500, 2000, 0.3, SourceType::Mixed};

// 無音（gain=0、低周波ダミー値）
constexpr FormantParams kSilence = {100, 100, 100, 100, 100, 100, 0.0, SourceType::Impulse};

// --- 破裂音パラメータ ---
constexpr FormantParams kVoiceBar = {200, 200, 200, 100, 200, 300, 0.08, SourceType::Impulse};
constexpr FormantParams kBurstP   = {300, 1000, 2300, 500, 1500, 2000, 0.30, SourceType::Noise};
constexpr FormantParams kBurstT   = {300, 4000, 5000, 500, 2000, 2000, 0.35, SourceType::Noise};
constexpr FormantParams kBurstK_A = {300, 1800, 2600, 500, 2000, 2000, 0.30, SourceType::Noise};
constexpr FormantParams kBurstTCh = {300, 3800, 6000, 500, 2000, 2000, 0.35, SourceType::Noise};
constexpr FormantParams kBurstTs  = {300, 5500, 7500, 500, 2500, 2000, 0.35, SourceType::Noise};

constexpr FormantParams kVotP_A = {800, 1200, 2600, 300, 400, 500, 0.10, SourceType::Noise};
constexpr FormantParams kVotT_A = {800, 1200, 2600, 300, 400, 500, 0.10, SourceType::Noise};
constexpr FormantParams kVotK_A = {800, 1200, 2600, 300, 400, 500, 0.10, SourceType::Noise};

constexpr int kSokuonSamples = 5292;  // 120 ms
constexpr int kHatsuonSamples = 3528; // 80 ms

// --- ヘルパー関数 ---

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

// ============ Text to Phoneme ============

enum CType : int8_t {
    V = 0,              // vowel only
    K, G, T, D, P, B,   // plosives
    CH, TS, DZH, DZ,    // affricates (/tɕ/, /ts/, /dʑ/, /dz/)
    S, SH,              // voiceless sibilants /s/, /ɕ/
    HA, HI, HU, HE, HO, // は行 allophones
    NN, M, R, J, W,     // sonorants
    Q, HN, SK           // special: sokuon, hatsuon, skip
};

struct KanaEntry { CType c; int8_t v; };

static constexpr KanaEntry kKanaTable[83] = {
    {V,0},{V,0},{V,1},{V,1},{V,2},{V,2},{V,3},{V,3},{V,4},{V,4},
    {K,0},{G,0},{K,1},{G,1},{K,2},{G,2},{K,3},{G,3},{K,4},{G,4},
    {S,0},{DZ,0},{SH,1},{DZH,1},{S,2},{DZ,2},{S,3},{DZ,3},{S,4},{DZ,4},
    {T,0},{D,0},{CH,1},{DZH,1},{Q,-1},{TS,2},{DZ,2},{T,3},{D,3},{T,4},{D,4},
    {NN,0},{NN,1},{NN,2},{NN,3},{NN,4},
    {HA,0},{B,0},{P,0},{HI,1},{B,1},{P,1},{HU,2},{B,2},{P,2},{HE,3},{B,3},{P,3},{HO,4},{B,4},{P,4},
    {M,0},{M,1},{M,2},{M,3},{M,4},
    {SK,-1},{J,0},{SK,-1},{J,2},{SK,-1},{J,4},
    {R,0},{R,1},{R,2},{R,3},{R,4},
    {SK,-1},{W,0},{SK,-1},{SK,-1},{V,4},{HN,-1},
};

static void emitCV(std::vector<PhonemeEntry>& seq, CType c, int vi) {
    const auto& v = *kVowels[vi];
    constexpr int vMs = 120;
    switch (c) {
        case V:   seq.push_back({v, ms2s(vMs)}); break;
        case K:   append(seq, makePlosiveCV(80,10,30, kSilence,kBurstK_A,kVotK_A, v,vMs)); break;
        case G:   append(seq, makePlosiveCV(60,5,0, kVoiceBar,kBurstK_A,kVotK_A, v,vMs)); break;
        case T:   append(seq, makePlosiveCV(70,10,20, kSilence,kBurstT,kVotT_A, v,vMs)); break;
        case D:   append(seq, makePlosiveCV(50,5,0, kVoiceBar,kBurstT,kVotT_A, v,vMs)); break;
        case P:   append(seq, makePlosiveCV(70,10,15, kSilence,kBurstP,kVotP_A, v,vMs)); break;
        case B:   append(seq, makePlosiveCV(50,5,0, kVoiceBar,kBurstP,kVotP_A, v,vMs)); break;
        case CH:  append(seq, makePlosiveCV(70,5,80, kSilence,kBurstTCh,kFricSh, v,vMs)); break;
        case TS:  append(seq, makePlosiveCV(70,5,70, kSilence,kBurstTs,kFricS, v,vMs)); break;
        case DZH: append(seq, makePlosiveCV(40,5,60, kVoiceBar,kBurstTCh,kFricZh, v,vMs)); break;
        case DZ:  append(seq, makePlosiveCV(40,5,50, kVoiceBar,kBurstTs,kFricZ, v,vMs)); break;
        case S:   seq.insert(seq.end(), {{kFricS,ms2s(120)},{v,ms2s(vMs)}}); break;
        case SH:  seq.insert(seq.end(), {{kFricSh,ms2s(120)},{v,ms2s(vMs)}}); break;
        case HA:  seq.insert(seq.end(), {{kFricH_A,ms2s(80)},{v,ms2s(vMs)}}); break;
        case HI:  seq.insert(seq.end(), {{kFricChi,ms2s(100)},{v,ms2s(vMs)}}); break;
        case HU:  seq.insert(seq.end(), {{kFricPhi,ms2s(100)},{v,ms2s(vMs)}}); break;
        case HE:  seq.insert(seq.end(), {{kFricH_E,ms2s(80)},{v,ms2s(vMs)}}); break;
        case HO:  seq.insert(seq.end(), {{kFricH_O,ms2s(80)},{v,ms2s(vMs)}}); break;
        case NN:  seq.insert(seq.end(), {{kNasalN,ms2s(80)},{v,ms2s(vMs)}}); break;
        case M:   seq.insert(seq.end(), {{kNasalM,ms2s(80)},{v,ms2s(vMs)}}); break;
        case R:   seq.insert(seq.end(), {{kTapR,ms2s(30)},{v,ms2s(vMs)}}); break;
        case J:   seq.insert(seq.end(), {{kSemiJ,ms2s(60)},{v,ms2s(vMs)}}); break;
        case W:   seq.insert(seq.end(), {{kSemiW,ms2s(60)},{v,ms2s(vMs)}}); break;
        default: break;
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

        if (vi == 1 && c != V && pos + 3 <= text.size()) {
            int ncp = decode3(text, pos);
            if (ncp >= 0) {
                if      (ncp == 0x3083) { vi = 0; pos += 3; }
                else if (ncp == 0x3085) { vi = 2; pos += 3; }
                else if (ncp == 0x3087) { vi = 4; pos += 3; }
            }
        }

        emitCV(seq, c, vi);
        if (vi >= 0) lastVowel = vi;
    }
    return seq;
}

// ============ WAV Writer ============

static bool writeWav(const char* filename,
                     const std::vector<int16_t>& samples,
                     int sampleRate) {
    std::ofstream ofs(filename, std::ios::binary);
    if (!ofs) {
        std::cerr << "Error: cannot open " << filename << "\n";
        return false;
    }

    auto write16 = [&](uint16_t v) {
        ofs.write(reinterpret_cast<const char*>(&v), 2);
    };
    auto write32 = [&](uint32_t v) {
        ofs.write(reinterpret_cast<const char*>(&v), 4);
    };

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

// ============ Main ============

#ifndef TEST_BUILD
int main(int argc, char* argv[]) {
    std::string input = (argc > 1) ? argv[1] : "あいうえお";
    auto sequence = textToPhoneme(input);

    if (sequence.empty()) {
        std::cerr << "Usage: " << argv[0] << " <hiragana text>\n";
        return 1;
    }

    Synthesizer synth;
    std::vector<int16_t> buf;
    synth.synthesize(sequence, buf);

    if (!writeWav("output.wav", buf, static_cast<int>(kSampleRate))) {
        return 1;
    }

    std::cout << "Wrote output.wav (" << buf.size() << " samples, "
              << sequence.size() << " segments)\n";
    return 0;
}
#endif // TEST_BUILD
