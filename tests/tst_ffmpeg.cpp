#include <QtTest>

#include <QImage>

#include <memory>
#include <vector>

#include "stream/frame.h"
#include "stream/yuv_convert.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

// 验证 third_party 里那份预编译 FFmpeg 真的能用：版本够、H.264 解码器在、上下文能开。
class FfmpegTest : public QObject
{
    Q_OBJECT

private slots:
    void VersionIsUsable();
    void H264DecoderIsAvailable();
    void DecoderContextOpens();
    void SwscaleConvertsSyntheticFrame();
    void SwscaleHandlesPaddedStrides();
};

// FFmpeg 版本要够我们用到的 API
void FfmpegTest::VersionIsUsable()
{
    qInfo() << "avcodec" << avcodec_version() << "avutil" << avutil_version();
    QVERIFY(avcodec_version() >= AV_VERSION_INT(58, 0, 0));
}

// H.264 解码器必须存在
void FfmpegTest::H264DecoderIsAvailable()
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    QVERIFY(codec != nullptr);
    QCOMPARE(codec->id, AV_CODEC_ID_H264);
}

// 按 H.264 能打开解码上下文
void FfmpegTest::DecoderContextOpens()
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    QVERIFY(codec != nullptr);

    AVCodecContext *context = avcodec_alloc_context3(codec);
    QVERIFY(context != nullptr);
    QCOMPARE(avcodec_open2(context, codec, nullptr), 0);

    // avcodec_free_context 会把指针置空，顺手把这条也钉住
    avcodec_free_context(&context);
    QVERIFY(context == nullptr);
}

// 自造 32x32 的 YUV420P 帧走一遍转换
void FfmpegTest::SwscaleConvertsSyntheticFrame()
{
    // 自己造一个 32x32 的 YUV420P 帧走一遍转换。
    const int width = 32;
    const int height = 32;
    const int chroma_width = width / 2;
    const int chroma_height = height / 2;

    std::vector<quint8> y(static_cast<size_t>(width * height), 0x80);
    std::vector<quint8> u(static_cast<size_t>(chroma_width * chroma_height), 0x80);
    std::vector<quint8> v(static_cast<size_t>(chroma_width * chroma_height), 0x80);

    const auto owner = std::make_shared<int>(0);
    const qsc::stream::Frame frame(QSize(width, height), y.data(), u.data(), v.data(), width,
                                   chroma_width, chroma_width, owner);

    qsc::stream::YuvConvert convert;
    const QImage image = convert.Convert(frame);

    QVERIFY(!image.isNull());
    QCOMPARE(image.size(), QSize(width, height));
    QVERIFY(image.pixelColor(width / 2, height / 2).isValid());
}

// 行跨度带填充时转换也不能错位
void FfmpegTest::SwscaleHandlesPaddedStrides()
{
    // 用真机的真实尺寸和真实跨度：328x720，Y 跨度 384、UU/VV 跨度 192。
    const int width = 328;
    const int height = 720;
    const int stride_y = 384;
    const int stride_uv = 192;

    std::vector<quint8> y(static_cast<size_t>(stride_y * height), 0x80);
    std::vector<quint8> u(static_cast<size_t>(stride_uv * (height / 2)), 0x80);
    std::vector<quint8> v(static_cast<size_t>(stride_uv * (height / 2)), 0x80);

    const auto owner = std::make_shared<int>(0);
    const qsc::stream::Frame frame(QSize(width, height), y.data(), u.data(), v.data(), stride_y,
                                   stride_uv, stride_uv, owner);

    qsc::stream::YuvConvert convert;
    const QImage image = convert.Convert(frame);

    QVERIFY(!image.isNull());
    QCOMPARE(image.size(), QSize(width, height));
}

QTEST_GUILESS_MAIN(FfmpegTest)

#include "tst_ffmpeg.moc"
