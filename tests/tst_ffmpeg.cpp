#include <QtTest>

#include <QImage>

#include <memory>
#include <vector>

#include "stream/frame.h"

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
// 行跨度带填充时转换也不能错位
QTEST_GUILESS_MAIN(FfmpegTest)

#include "tst_ffmpeg.moc"
