#include <QColorSpace>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QImageReader>
#include <QSize>
#include <QTextStream>

#include "format-enum.h"
#include "fuzzyeq.cpp"

static QImage::Format preferredFormat(QImage::Format fmt)
{
    switch (fmt) {
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32:
    case QImage::Format_RGBX64:
    case QImage::Format_RGBA64:
        return fmt;
    default:
        return QImage::Format_ARGB32;
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QCoreApplication::setApplicationName(QStringLiteral("animationdump"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("KDE tool to check animated files"));
    parser.addHelpOption();

    QCommandLineOption saveframes_option(QStringList() << QStringLiteral("s") << QStringLiteral("save"),
                                         QStringLiteral("Save individual frames as prefix00.png, prefix01.png, ..."),
                                         QStringLiteral("prefix"),
                                         QStringLiteral("frame"));

    QCommandLineOption format(QStringList() << QStringLiteral("e") << QStringLiteral("extension"),
                              QStringLiteral("Image file format to use for invidudual frames (png is default)"),
                              QStringLiteral("format"),
                              QStringLiteral("png"));

    QCommandLineOption testframes_option(QStringList() << QStringLiteral("t") << QStringLiteral("test-frames"),
                                         QStringLiteral("Compare if individual frames match prefix00.png, prefix01.png, ..."),
                                         QStringLiteral("prefix"),
                                         QStringLiteral("frame"));

    QCommandLineOption fuzz_option(QStringList() << QStringLiteral("f") << QStringLiteral("fuzz"),
                                   QStringLiteral("Allow for some deviation in ARGB data."),
                                   QStringLiteral("max"));

    QCommandLineOption jump_option(QStringList() << QStringLiteral("j") << QStringLiteral("jump") << QStringLiteral("jumptoimage"),
                                   QStringLiteral("call jumpToImage before reading"),
                                   QStringLiteral("imageNumber"),
                                   QStringLiteral("0"));

    parser.addOption(saveframes_option);
    parser.addOption(format);
    parser.addOption(testframes_option);
    parser.addOption(fuzz_option);
    parser.addOption(jump_option);
    parser.process(app);

    const QStringList filenames = parser.positionalArguments();

    if (filenames.count() != 1) {
        QTextStream(stderr) << "Must provide one file\n";
        parser.showHelp(1);
    }

    if (parser.isSet(saveframes_option) && parser.isSet(testframes_option)) {
        QTextStream(stderr) << "Cannot specify both -save and -test-frames options\n";
        parser.showHelp(1);
    }

    uchar fuzziness = 0;
    if (parser.isSet(fuzz_option)) {
        bool ok;
        uint fuzzarg = parser.value(fuzz_option).toUInt(&ok);
        if (!ok || fuzzarg > 255) {
            QTextStream(stderr) << "Error: max fuzz argument must be a number between 0 and 255\n";
            parser.showHelp(1);
        }
        fuzziness = uchar(fuzzarg);
    }

    QImageReader reader(filenames.at(0));
    if (!reader.canRead()) {
        QTextStream(stderr) << "Unable to read " << filenames.at(0) << ": " << reader.errorString() << "\n";
        return 2;
    }

    int image_count = reader.imageCount();
    int loop_count = reader.loopCount();

    int current_image_number = reader.currentImageNumber();
    const bool animation_supported = reader.supportsAnimation();

    QTextStream out(stdout);

    out << "imageCount = " << image_count << (animation_supported ? " animated" : " static") << "\nloopCount = " << loop_count
        << "\ncurrentImageNumber (before reading) = " << current_image_number << "\n";
    if (reader.supportsOption(QImageIOHandler::Size)) {
        QSize size_at_start = reader.size();
        out << "Size: ";
        if (size_at_start.isValid()) {
            out << size_at_start.width() << 'x' << size_at_start.height() << "\n";
        } else {
            out << "Invalid!\n";
        }
    }

    int expected_image_number = 0;
    bool finished = false;

    if (parser.isSet(jump_option)) {
        bool ok;
        int imageNumber = parser.value(jump_option).toInt(&ok, 10);
        if (!ok || imageNumber < 0 || imageNumber >= image_count) {
            QTextStream(stderr) << "Error: imageNumber must be a number between 0 and imageCount-1\n";
            parser.showHelp(1);
        }

        out << "Trying to call jumpToImage(" << imageNumber << ")\n";
        if (reader.jumpToImage(imageNumber)) {
            current_image_number = reader.currentImageNumber();
            out << "currentImageNumber (after jumpToImage) = " << current_image_number << "\n";
            expected_image_number = imageNumber;
        } else {
            out << "jumpToImage failed! (maybe not supported/implemented)\n";
        }
    }

    do {
        out.flush();
        out << "#" << expected_image_number << '\t';
        QImage img;
        QElapsedTimer timer;
        timer.start();

        if (!reader.read(&img)) {
            out << "reading failed!\n";
            return 3;
        }

        qint64 mselapsed = timer.elapsed();

        int duration = reader.nextImageDelay();
        current_image_number = reader.currentImageNumber();

        out << img.width() << 'x' << img.height() << " " << formatToString(img.format()) << " " << duration << "ms\tsequence=" << current_image_number
            << "\tdecoding=" << mselapsed << "ms\n";

        if (animation_supported) {
            if (image_count == 0) { // unknown number of frames
                image_count = reader.imageCount();
                if (image_count > 0) {
                    out << "NEW value for imageCount = " << image_count << '\n';
                }
            }
            int actual_loop_count = reader.loopCount();
            if (actual_loop_count != loop_count) {
                loop_count = actual_loop_count;
                out << "NEW value for loopCount = " << loop_count << '\n';
            }
        }

        if (parser.isSet(saveframes_option)) {
            QString out_filename = QString("%1%2.%3").arg(parser.value(saveframes_option)).arg(expected_image_number, 2, 10, '0').arg(parser.value(format));
            if (img.save(out_filename, nullptr, 100)) {
                out << "Saved " << out_filename << '\n';
            } else {
                QTextStream(stderr) << "Unable to save " << out_filename << '\n';
                return 4;
            }

        } else if (parser.isSet(testframes_option)) {
            QString in_filename = QString("%1%2.%3").arg(parser.value(testframes_option)).arg(expected_image_number, 2, 10, '0').arg(parser.value(format));
            QImage testimage;
            if (testimage.load(in_filename)) {
                out << "Comparing with " << in_filename << "...";
                if (img.width() != testimage.width() || img.height() != testimage.height()) {
                    out << "size do not match!\n";
                    return 6;
                }

                QImage::Format cmpFormat = preferredFormat(testimage.format());
                if (testimage.format() != cmpFormat) {
                    testimage.convertTo(cmpFormat);
                }
                if (img.format() != cmpFormat) {
                    img.convertTo(cmpFormat);
                }

                if (fuzzyeq(img, testimage, fuzziness, true)) {
                    out << "ok\n";
                } else {
                    out << "image are different!\n";
                    return 7;
                }
            } else {
                QTextStream(stderr) << "Unable open " << in_filename << '\n';
                return 5;
            }
        }

        expected_image_number++;

        if (expected_image_number >= image_count) {
            if (image_count > 0) {
                finished = true;
            } else {
                // check if we can read when there is unknown number of frames
                if (!reader.canRead()) {
                    finished = true;
                    out << "*** END of reading ***\n";
                }
            }
        }
    } while (!finished);

    if (reader.canRead()) {
        out << "canRead() returned true, more frames are probably available!\n";
    }

    if (reader.jumpToNextImage()) {
        out << "jumpToNextImage() returned true, but it is unexpected at the last frame\n";
    }
    return 0;
}
