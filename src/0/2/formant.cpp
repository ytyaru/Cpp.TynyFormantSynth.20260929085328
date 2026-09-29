// formant.cpp — Approach 2: CrossfadeWindow (Phoneme Boundary Crossfade)
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
#include <unistd.h>

// ============ Constants & Types ============

constexpr double kSampleRate  = 44100.0;
constexpr double kPi          = std::numbers::pi;
constexpr int    kFrameSize   = 220;            // 5 ms at 44.1 kHz
constexpr int    kCrossfadeSamples = 220;       // 5 ms crossfade at boundaries

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

// ============ Synthesizer with CrossfadeWindow ============
struct PhonemeEntry {
    FormantParams params;
    int duration_samples;
};

class Synthesizer {
public:
    double f0 = 160.0;

    void synthesize(const std::vector<PhonemeEntry>& sequence,
                    std::vector<int16_t>& output) {
        if (sequence.empty()) return;

        // 各音素セグメントを個別に生成して一時バッファに貯める
        std::vector<std::vector<double>> segBuffers(sequence.size());
        
        for (std::size_t seg = 0; seg < sequence.size(); ++seg) {
            auto cur = sequence[seg].params;
            const auto& nxt_raw = (seg + 1 < sequence.size())
                                ? sequence[seg + 1].params : cur;
            int dur = sequence[seg].duration_samples;
            SourceType currentSource = cur.source;
            FormantParams currentP = cur;

            std::vector<double>& sbuf = segBuffers[seg];
            sbuf.resize(dur);

            std::array<Resonator, 3> filters{};
            ImpulseTrain impulse;
            NoiseGen noise;

            for (int n = 0; n < dur; ++n) {
                if (n % kFrameSize == 0) {
                    double t = static_cast<double>(n) / dur;
                    double blend = (t > 0.7) ? (t - 0.7) / 0.3 : 0.0;
                    currentP = lerp(cur, nxt_raw, blend);
                }

                filters[0].set(currentP.f1, currentP.bw1, kSampleRate);
                filters[1].set(currentP.f2, currentP.bw2, kSampleRate);
                filters[2].set(currentP.f3, currentP.bw3, kSampleRate);

                double s;
                switch (currentSource) {
                    case SourceType::Noise: s = noise.next(); break;
                    case SourceType::Mixed:
                        s = 0.6 * noise.next() + 0.4 * impulse.next(f0, kSampleRate);
                        break;
                    default: s = impulse.next(f0, kSampleRate); break;
                }

                for (auto& f : filters) s = f.process(s);
                sbuf[n] = s * currentP.gain;
            }
        }

        // セグメント同士をクロスフェード（Overlaps and Add）で繋ぎ合わせる
        std::vector<double> combined;
        for (std::size_t seg = 0; seg < segBuffers.size(); ++seg) {
            const auto& sbuf = segBuffers[seg];
            if (combined.empty()) {
                combined = sbuf;
            } else {
                int xfLen = std::min(kCrossfadeSamples, static_cast<int>(std::min(combined.size(), sbuf.size())));
                
                // combined の末尾 xfLen サンプルと、sbuf の先頭 xfLen サンプルをクロスフェード
                std::size_t overlapStart = combined.size() - xfLen;
                for (int i = 0; i < xfLen; ++i) {
                    double t = static_cast<double>(i) / xfLen;
                    // sin/cosカーブなどを使うとより滑らかだが、ここでは線形クロスフェード
                    double wOld = 1.0 - t;
                    double wNew = t;
                    combined[overlapStart + i] = combined[overlapStart + i] * wOld + sbuf[i] * wNew;
                }

                // 残りの sbuf 部分を追加
                if (sbuf.size() > static_cast<std::size_t>(xfLen)) {
                    combined.insert(combined.end(), sbuf.begin() + xfLen, sbuf.end());
                }
            }
        }

        // int16_t に変換して出力
        output.resize(combined.size());
        for (std::size_t i = 0; i < combined.size(); ++i) {
            double out = combined[i] * 32767.0;
            out = std::clamp(out, -32768.0, 32767.0);
            output[i] = static_cast<int16_t>(out);
        }
    }
};

// ============ Professional Loudness Normalization (EBU R128 Approximation) ============
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

// ============ Phoneme Data & Text to Phoneme (Standard) ============
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

constexpr FormantParams kFricH_A = {800, 1200, 2600, 200, 300, 400, 0.15, SourceType::Noise};
constexpr FormantParams kFricS   = {200, 5500, 7500, 500, 3000, 2000, 0.4, SourceType::Noise};
constexpr FormantParams kFricSh  = {200, 3800, 6000, 500, 2500, 2000, 0.4, SourceType::Noise};
constexpr FormantParams kFricChi = {200, 3000, 5000, 500, 2000, 2000, 0.3, SourceType::Noise};
constexpr FormantParams kFricPhi = {200, 2500, 4000, 500, 3000, 2000, 0.15, SourceType::Noise};
constexpr FormantParams kFricH_E = {500, 1900, 2600, 200, 300, 400, 0.15, SourceType::Noise};
constexpr FormantParams kFricH_O = {500,  800, 2400, 200, 300, 400, 0.15, SourceType::Noise};
constexpr FormantParams kFricZ   = {200, 5500, 7500, 500, 3000, 2000, 0.3, SourceType::Mixed};
constexpr FormantParams kFricZh  = {200, 3800, 6000, 500, 2500, 2000, 0.3, SourceType::Mixed};

constexpr FormantParams kSilence = {100, 100, 100, 100, 100, 100, 0.0, SourceType::Impulse};

constexpr FormantParams kVoiceBar = {200, 200, 200, 100, 200, 300, 0.08, SourceType::Impulse};
constexpr FormantParams kBurstP   = {300, 1000, 2300, 500, 1500, 2000, 0.30, SourceType::Noise};
constexpr FormantParams kBurstT   = {300, 4000, 5000, 500, 2000, 2000, 0.35, SourceType::Noise};
constexpr FormantParams kBurstK_A = {300, 1800, 2600, 500, 2000, 2000, 0.30, SourceType::Noise};
constexpr FormantParams kBurstTCh = {300, 3800, 6000, 500, 2000, 2000, 0.35, SourceType::Noise};
constexpr FormantParams kBurstTs  = {300, 5500, 7500, 500, 2500, 2000, 0.35, SourceType::Noise};

constexpr FormantParams kVotP_A = {800, 1200, 2600, 300, 400, 500, 0.10, SourceType::Noise};
constexpr FormantParams kVotT_A = {800, 1200, 2600, 300, 400, 500, 0.10, SourceType::Noise};
constexpr FormantParams kVotK_A = {800, 1200, 2600, 300, 400, 500, 0.10, SourceType::Noise};

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

// ============ Help Message (Japanese) ============
static void printHelp(const char* progName) {
    std::cout << "Usage: " << progName << " [options] [hiragana text]\n"
              << "日本語フォルマント音声合成エンジン (CrossfadeWindow 版)\n\n"
              << "オプション:\n"
              << "  -l <val>      目標ラウドネス (LUFS近似, デフォルト: -14.0)\n"
              << "  --f0 <val>    基本周波数 F0 Hz (声の高さ, デフォルト: 160.0)\n"
              << "  -o [path]     WAVファイル出力 (-o のみで output.wav, パス指定でそこに出力)\n"
              << "                ※指定がない場合は標準出力 (stdout) にWAVを出力します\n"
              << "  -h, --help    このヘルプメッセージを表示\n\n"
              << "使用例:\n"
              << "  echo 'あいうえお' | " << progName << " | aplay\n"
              << "  " << progName << " -o ./output.wav 'にほんごをはつわするてすとです。'\n";
}

// ============ Main ============
#ifndef TEST_BUILD
int main(int argc, char* argv[]) {
    double targetLoudness = -14.0;
    double f0 = 160.0;
    std::string text = "";
    std::string outPath = "";
    bool hasOutputOption = false;
    bool outputDefaultName = false;

    if (argc == 1 && isatty(STDIN_FILENO)) {
        printHelp(argv[0]);
        return 0;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printHelp(argv[0]);
            return 0;
        } else if (arg == "-l" && i + 1 < argc) {
            targetLoudness = std::stod(argv[++i]);
        } else if (arg == "--f0" && i + 1 < argc) {
            f0 = std::stod(argv[++i]);
        } else if (arg == "-o") {
            hasOutputOption = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                outPath = argv[++i];
            } else {
                outputDefaultName = true;
            }
        } else if (arg[0] != '-') {
            text = arg;
        } else {
            std::cerr << "Error: 不明なオプションです: " << arg << "\n\n";
            printHelp(argv[0]);
            return 1;
        }
    }

    if (text.empty()) {
        std::ostringstream ss;
        ss << std::cin.rdbuf();
        text = ss.str();
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
    }

    if (text.empty()) {
        printHelp(argv[0]);
        return 1;
    }

    auto sequence = textToPhoneme(text);
    if (sequence.empty()) {
        std::cerr << "Error: empty sequence or invalid hiragana text.\n";
        return 1;
    }

    Synthesizer synth;
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
                  << sequence.size() << " segments)\n"
                  << "Settings: loudness=" << targetLoudness << "LUFS, f0=" << f0 << "Hz (Crossfade)\n";
    } else {
        std::cerr << "Wrote to stdout (" << buf.size() << " samples, "
                  << sequence.size() << " segments)\n"
                  << "Settings: loudness=" << targetLoudness << "LUFS, f0=" << f0 << "Hz (Crossfade)\n";
        writeWavToStream(std::cout, buf, static_cast<int>(kSampleRate));
    }

    return 0;
}
#endif // TEST_BUILD
