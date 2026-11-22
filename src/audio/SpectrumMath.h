#ifndef SPECTRUMMATH_H
#define SPECTRUMMATH_H

#include <QVector>

namespace SpectrumMath {

/// 与 MusicVisualWidget 的频谱柱数量保持一致。
constexpr int kBandCount = 46;
/// 2048 点窗在 44.1/48 kHz 下约 43~46 ms，低频分辨率足够，延迟也不明显。
constexpr int kFftSize = 2048;

/// 把单声道 PCM（取值约 -1..1）转成 46 个对数频段的归一化能量。
/// 输入可以长于 kFftSize，只分析最后 kFftSize 个采样。
/// 频段使用 dBFS 压缩，返回 0..1；静音返回接近 0。
QVector<float> analyze(const QVector<float> &samples, int sampleRate,
                       int bandCount = kBandCount);

} // namespace SpectrumMath

#endif // SPECTRUMMATH_H
