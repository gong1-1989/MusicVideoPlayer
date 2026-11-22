#include "audio/SpectrumMath.h"

#include <QtMath>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinDb = -70.0;
constexpr double kMaxDb = 0.0;

/// 原地基 2 FFT。只用于 2048 点的小窗口，不引入额外 DSP 依赖。
void fft(QVector<double> &re, QVector<double> &im)
{
    const int n = re.size();
    int j = 0;
    for (int i = 1; i < n; ++i) {
        int bit = n >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * kPi / len;
        const double wLenRe = std::cos(angle);
        const double wLenIm = std::sin(angle);
        const int half = len / 2;
        for (int i = 0; i < n; i += len) {
            double wRe = 1.0;
            double wIm = 0.0;
            for (int k = 0; k < half; ++k) {
                const double uRe = re[i + k];
                const double uIm = im[i + k];
                const double vRe = re[i + k + half] * wRe - im[i + k + half] * wIm;
                const double vIm = re[i + k + half] * wIm + im[i + k + half] * wRe;
                re[i + k] = uRe + vRe;
                im[i + k] = uIm + vIm;
                re[i + k + half] = uRe - vRe;
                im[i + k + half] = uIm - vIm;
                const double nextRe = wRe * wLenRe - wIm * wLenIm;
                wIm = wRe * wLenIm + wIm * wLenRe;
                wRe = nextRe;
            }
        }
    }
}

} // namespace

namespace SpectrumMath {

QVector<float> analyze(const QVector<float> &samples, int sampleRate, int bandCount)
{
    if (bandCount <= 0)
        return {};
    QVector<float> levels(bandCount, 0.0f);
    if (sampleRate <= 0 || samples.size() < kFftSize)
        return levels;

    QVector<double> re(kFftSize, 0.0);
    QVector<double> im(kFftSize, 0.0);
    const int start = samples.size() - kFftSize;
    for (int i = 0; i < kFftSize; ++i) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (kFftSize - 1));
        double sample = double(samples.at(start + i));
        if (!std::isfinite(sample))
            sample = 0.0;
        sample = qBound(-1.0, sample, 1.0);
        re[i] = sample * hann;
    }

    fft(re, im);

    // Hann 窗相干增益为 N/2；乘 2 得到单边幅度谱，因此正弦满幅约等于 1。
    const double amplitudeScale = 4.0 / kFftSize;
    const double binHz = double(sampleRate) / kFftSize;
    const int usableBins = kFftSize / 2 - 1;
    const double nyquist = sampleRate / 2.0;
    const double lowHz = 40.0;
    const double highHz = qMin(16000.0, nyquist * 0.95);
    if (highHz <= lowHz || usableBins <= 1)
        return levels;

    for (int band = 0; band < bandCount; ++band) {
        const double ratio0 = double(band) / bandCount;
        const double ratio1 = double(band + 1) / bandCount;
        const double f0 = lowHz * std::pow(highHz / lowHz, ratio0);
        const double f1 = lowHz * std::pow(highHz / lowHz, ratio1);

        int firstBin = qMax(1, int(std::ceil(f0 / binHz)));
        int lastBin = qMin(usableBins, int(std::floor(f1 / binHz)));
        if (lastBin < firstBin) {
            // 低频段比一个 FFT bin 还窄时，退化为最近的 bin，避免整段恒为 0。
            const double center = std::sqrt(f0 * f1);
            firstBin = qBound(1, int(std::round(center / binHz)), usableBins);
            lastBin = firstBin;
        }

        double peak = 0.0;
        for (int k = firstBin; k <= lastBin; ++k) {
            const double magnitude =
                amplitudeScale * std::sqrt(re[k] * re[k] + im[k] * im[k]);
            peak = qMax(peak, magnitude);
        }

        const double db = 20.0 * std::log10(qMax(peak, 1e-7));
        const double normalized = (db - kMinDb) / (kMaxDb - kMinDb);
        levels[band] = float(qBound(0.0, normalized, 1.0));
    }

    return levels;
}

} // namespace SpectrumMath
