// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/cropper/CropGeometry.h>
#include <cullfinch/cropper/CropSequence.h>
#include <cullfinch/cropper/CropWriter.h>
#include <cullfinch/cropper/Sidecar.h>
#include <cullfinch/testsupport/TempCollection.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QTest>

#include <exiv2/exiv2.hpp>

#include <cmath>
#include <cstdint>

using namespace cullfinch;
using namespace cullfinch::cropper;

namespace {

std::string nativePath(const QString& path) {
    return QFile::encodeName(path).toStdString();
}

QByteArray digestOf(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

/// Write `image` as a JPEG at `path`, at a quality high enough that a flat
/// colour reads back as itself.
bool writeJpeg(const QImage& image, const QString& path) {
    return image.save(path, "JPEG", 98);
}

/// A photo whose left half is red and right half is blue, so where a crop
/// came from can be read from its pixels.
QImage halves(const QSize& size) {
    QImage image(size, QImage::Format_RGB32);
    QPainter painter(&image);
    painter.fillRect(QRect(0, 0, size.width() / 2, size.height()), Qt::red);
    painter.fillRect(QRect(size.width() / 2, 0, size.width() - size.width() / 2, size.height()),
                     Qt::blue);
    return image;
}

/// Tag the JPEG at `path` with EXIF, the way a camera would have.
void tagExif(const QString& path, int orientation) {
    const Exiv2::Image::UniquePtr image = Exiv2::ImageFactory::open(nativePath(path));
    image->readMetadata();
    Exiv2::ExifData& exif = image->exifData();
    exif["Exif.Image.Make"] = std::string("Cullfinch Test Camera");
    exif["Exif.Image.Orientation"] = static_cast<std::uint16_t>(orientation);
    exif["Exif.Photo.DateTimeOriginal"] = std::string("2026:10:18 12:00:00");
    image->writeMetadata();
}

Exiv2::ExifData exifOf(const QString& path) {
    const Exiv2::Image::UniquePtr image = Exiv2::ImageFactory::open(nativePath(path));
    image->readMetadata();
    return image->exifData();
}

Exiv2::XmpData xmpOf(const QString& path) {
    const Exiv2::Image::UniquePtr image = Exiv2::ImageFactory::open(nativePath(path));
    image->readMetadata();
    return image->xmpData();
}

QString exifText(const Exiv2::ExifData& exif, const char* key) {
    const auto found = exif.findKey(Exiv2::ExifKey(key));
    return found == exif.end() ? QString() : QString::fromStdString(found->toString());
}

QString xmpText(const Exiv2::XmpData& xmp, const char* key) {
    const auto found = xmp.findKey(Exiv2::XmpKey(key));
    return found == xmp.end() ? QString() : QString::fromStdString(found->toString());
}

/// The bounding box of the pixels brighter than mid-grey, normalised.
QRectF brightBox(const QImage& image) {
    int left = image.width();
    int top = image.height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qGray(image.pixel(x, y)) > 128) {
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
        }
    }
    if (right < 0) {
        return {};
    }
    return {static_cast<double>(left) / image.width(), static_cast<double>(top) / image.height(),
            static_cast<double>(right + 1 - left) / image.width(),
            static_cast<double>(bottom + 1 - top) / image.height()};
}

bool near(const QRectF& actual, const QRectF& expected, double tolerance) {
    return std::abs(actual.left() - expected.left()) <= tolerance &&
           std::abs(actual.top() - expected.top()) <= tolerance &&
           std::abs(actual.right() - expected.right()) <= tolerance &&
           std::abs(actual.bottom() - expected.bottom()) <= tolerance;
}

QStringList displayNames(const CropPlan& plan) {
    QStringList names;
    for (const CropTarget& target : plan.targets) {
        names.append(target.presentation.displayName);
    }
    return names;
}

/// No temporary file of an interrupted or finished write is left behind.
bool noPartialFiles(const QString& directory) {
    return QDir(directory)
        .entryList({QStringLiteral("*.part")}, QDir::Files | QDir::Hidden)
        .isEmpty();
}

} // namespace

/// What the crop tool reads and writes: which photos a run covers, the
/// cropped JPG with its metadata, and the RAW's Camera Raw sidecar.
class TestCropFiles : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void aRunCoversPhotosNotCropsOrLoneRaws();
    void aCroppedPhotoIsLeftOutUnlessRedone();
    void subdirectoriesAreEnteredOnlyWhenAsked();

    void theCropIsWrittenBesideTheUntouchedOriginal();
    void aSecondCropReplacesTheFirst();
    void metadataTravelsWithTheCropTurnedUpright();
    void theStoredFrameMatchesWhatTheDecoderShows_data();
    void theStoredFrameMatchesWhatTheDecoderShows();

    void aSidecarIsCreatedWithTheCrop();
    void anExistingSidecarKeepsEverythingElse();
    void anUnreadableSidecarIsReportedNotReplaced();
    void aRawCropIsMeasuredInTheStoredFrame();
};

void TestCropFiles::initTestCase() {
    initialiseMetadataLibrary();
}

void TestCropFiles::aRunCoversPhotosNotCropsOrLoneRaws() {
    testsupport::TempCollection collection;
    QVERIFY(collection.isValid());
    QVERIFY(!collection.addJpeg(QStringLiteral("IMG_10.JPG")).isEmpty());
    QVERIFY(!collection.addJpeg(QStringLiteral("IMG_9.JPG")).isEmpty());
    const QString raw = collection.addRaw(QStringLiteral("IMG_9.CR2"));
    QVERIFY(!raw.isEmpty());
    QVERIFY(!collection.addRaw(QStringLiteral("IMG_2.CR2")).isEmpty());
    // An earlier crop of a photo that is no longer there: still not a step.
    QVERIFY(!collection.addJpeg(QStringLiteral("IMG_1.crop.JPG")).isEmpty());

    const CropPlan plan = planCrops(collection.path(), false, false);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QCOMPARE(displayNames(plan), (QStringList{QStringLiteral("IMG_9"), QStringLiteral("IMG_10")}));
    QCOMPARE(plan.targets.at(0).rawPaths, QStringList{raw});
    QCOMPARE(plan.targets.at(0).jpegPath, collection.filePath(QStringLiteral("IMG_9.JPG")));
    QCOMPARE(plan.withoutPreview, 1);
    QCOMPARE(plan.alreadyCropped, 0);
}

void TestCropFiles::aCroppedPhotoIsLeftOutUnlessRedone() {
    testsupport::TempCollection collection;
    QVERIFY(collection.isValid());
    QVERIFY(!collection.addJpeg(QStringLiteral("IMG_1.jpg")).isEmpty());
    QVERIFY(!collection.addJpeg(QStringLiteral("IMG_1.crop.jpg")).isEmpty());
    QVERIFY(!collection.addJpeg(QStringLiteral("IMG_2.jpg")).isEmpty());
    const QString raw = collection.addRaw(QStringLiteral("IMG_2.NEF"));
    QVERIFY(!collection.addJpeg(QStringLiteral("IMG_3.jpg")).isEmpty());
    // Cropped elsewhere -- in Lightroom, say -- and recorded in the sidecar.
    QVERIFY(writeCameraRawCrop(raw, QRectF(0.1, 0.1, 0.5, 0.5)).isEmpty());

    const CropPlan resumed = planCrops(collection.path(), false, false);
    QCOMPARE(displayNames(resumed), QStringList{QStringLiteral("IMG_3")});
    QCOMPARE(resumed.alreadyCropped, 2);

    const CropPlan redone = planCrops(collection.path(), false, true);
    QCOMPARE(displayNames(redone), (QStringList{QStringLiteral("IMG_1"), QStringLiteral("IMG_2"),
                                                QStringLiteral("IMG_3")}));
    QCOMPARE(redone.alreadyCropped, 0);
}

void TestCropFiles::subdirectoriesAreEnteredOnlyWhenAsked() {
    testsupport::TempCollection collection;
    QVERIFY(collection.isValid());
    QVERIFY(!collection.addJpeg(QStringLiteral("IMG_2.jpg")).isEmpty());
    QVERIFY(collection.addDirectory(QStringLiteral("b")));
    QVERIFY(!collection.addJpeg(QStringLiteral("b/IMG_1.jpg")).isEmpty());
    QVERIFY(collection.addDirectory(QStringLiteral("a")));
    QVERIFY(!collection.addJpeg(QStringLiteral("a/IMG_2.jpg")).isEmpty());

    QCOMPARE(displayNames(planCrops(collection.path(), false, false)),
             QStringList{QStringLiteral("IMG_2")});
    // By directory, then by name: a folder's photos stay together.
    QCOMPARE(displayNames(planCrops(collection.path(), true, false)),
             (QStringList{QStringLiteral("IMG_2"), QStringLiteral("a/IMG_2"),
                          QStringLiteral("b/IMG_1")}));
}

void TestCropFiles::theCropIsWrittenBesideTheUntouchedOriginal() {
    testsupport::TempCollection collection;
    QVERIFY(collection.isValid());
    const QString source = collection.filePath(QStringLiteral("IMG_1.jpg"));
    QVERIFY(writeJpeg(halves(QSize(200, 100)), source));
    const QByteArray before = digestOf(source);

    // The right half: blue, and a square.
    const QString error = writeJpegCrop(source, QRectF(0.5, 0.0, 0.5, 1.0));
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const QString output = collection.filePath(QStringLiteral("IMG_1.crop.jpg"));
    QCOMPARE(cropOutputPath(source), output);
    const QImage cropped(output);
    QCOMPARE(cropped.size(), QSize(100, 100));
    const QColor centre = cropped.pixelColor(50, 50);
    QVERIFY2(centre.blue() > 200 && centre.red() < 60, qPrintable(centre.name()));

    QCOMPARE(digestOf(source), before);
    QVERIFY(noPartialFiles(collection.path()));
}

void TestCropFiles::aSecondCropReplacesTheFirst() {
    testsupport::TempCollection collection;
    QVERIFY(collection.isValid());
    const QString source = collection.filePath(QStringLiteral("IMG_1.JPG"));
    QVERIFY(writeJpeg(halves(QSize(200, 100)), source));

    QVERIFY(writeJpegCrop(source, QRectF(0.0, 0.0, 0.5, 1.0)).isEmpty());
    QVERIFY(writeJpegCrop(source, QRectF(0.0, 0.0, 0.25, 0.5)).isEmpty());

    // The original's extension is kept as it is spelt.
    const QString output = collection.filePath(QStringLiteral("IMG_1.crop.JPG"));
    QCOMPARE(QImageReader(output).size(), QSize(50, 50));
    QVERIFY(noPartialFiles(collection.path()));
}

void TestCropFiles::metadataTravelsWithTheCropTurnedUpright() {
    testsupport::TempCollection collection;
    QVERIFY(collection.isValid());
    const QString source = collection.filePath(QStringLiteral("IMG_1.jpg"));
    // Stored landscape, red on the left. Orientation 6 shows it turned
    // clockwise: portrait, 100 x 200, with the red half on top.
    QVERIFY(writeJpeg(halves(QSize(200, 100)), source));
    tagExif(source, 6);

    QVERIFY(writeJpegCrop(source, QRectF(0.0, 0.0, 1.0, 0.5)).isEmpty());

    const QString output = cropOutputPath(source);
    QImageReader reader(output);
    reader.setAutoTransform(true);
    const QImage cropped = reader.read();
    QCOMPARE(cropped.size(), QSize(100, 100));
    const QColor centre = cropped.pixelColor(50, 50);
    QVERIFY2(centre.red() > 200 && centre.blue() < 60, qPrintable(centre.name()));

    const Exiv2::ExifData exif = exifOf(output);
    QCOMPARE(exifText(exif, "Exif.Image.Make"), QStringLiteral("Cullfinch Test Camera"));
    QCOMPARE(exifText(exif, "Exif.Photo.DateTimeOriginal"), QStringLiteral("2026:10:18 12:00:00"));
    // Stored upright now: a viewer must not turn it a second time.
    QCOMPARE(exifText(exif, "Exif.Image.Orientation"), QStringLiteral("1"));
    QCOMPARE(exifText(exif, "Exif.Photo.PixelXDimension"), QStringLiteral("100"));
    QCOMPARE(exifText(exif, "Exif.Photo.PixelYDimension"), QStringLiteral("100"));
}

void TestCropFiles::theStoredFrameMatchesWhatTheDecoderShows_data() {
    QTest::addColumn<int>("orientation");
    for (int orientation = 1; orientation <= 8; ++orientation) {
        QTest::addRow("orientation %d", orientation) << orientation;
    }
}

void TestCropFiles::theStoredFrameMatchesWhatTheDecoderShows() {
    // The geometry's orientation table, checked against the decoder rather
    // than against a second copy of the same table: a white block at a known
    // place in the stored pixels is found where the decoder shows it, and
    // mapping that back must land on the block again.
    QFETCH(int, orientation);
    testsupport::TempCollection collection;
    QVERIFY(collection.isValid());
    const QString path = collection.filePath(QStringLiteral("IMG_1.jpg"));
    QImage stored(QSize(200, 100), QImage::Format_RGB32);
    stored.fill(Qt::black);
    const QRect block(20, 10, 60, 40);
    QPainter(&stored).fillRect(block, Qt::white);
    QVERIFY(writeJpeg(stored, path));
    tagExif(path, orientation);

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QRectF shown = brightBox(reader.read());
    QVERIFY(!shown.isEmpty());

    const QRectF expected(0.1, 0.1, 0.3, 0.4);
    const QRectF mapped = toStoredFrame(shown, orientation);
    QVERIFY2(near(mapped, expected, 0.03), qPrintable(QStringLiteral("%1,%2 %3x%4")
                                                          .arg(mapped.x())
                                                          .arg(mapped.y())
                                                          .arg(mapped.width())
                                                          .arg(mapped.height())));
}

void TestCropFiles::aSidecarIsCreatedWithTheCrop() {
    testsupport::TempCollection collection;
    QVERIFY(collection.isValid());
    const QString raw = collection.addRaw(QStringLiteral("IMG_1.CR2"));
    QVERIFY(!hasRecordedCrop(raw));

    const QString error = writeCameraRawCrop(raw, QRectF(0.1, 0.2, 0.5, 0.6));
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // Adobe's name, without the RAW's extension.
    QCOMPARE(sidecarPath(raw), collection.filePath(QStringLiteral("IMG_1.xmp")));
    const Exiv2::XmpData xmp = xmpOf(sidecarPath(raw));
    QCOMPARE(xmpText(xmp, "Xmp.crs.HasCrop"), QStringLiteral("True"));
    QCOMPARE(xmpText(xmp, "Xmp.crs.CropLeft"), QStringLiteral("0.100000"));
    QCOMPARE(xmpText(xmp, "Xmp.crs.CropTop"), QStringLiteral("0.200000"));
    QCOMPARE(xmpText(xmp, "Xmp.crs.CropRight"), QStringLiteral("0.600000"));
    QCOMPARE(xmpText(xmp, "Xmp.crs.CropBottom"), QStringLiteral("0.800000"));
    QCOMPARE(xmpText(xmp, "Xmp.crs.CropAngle"), QStringLiteral("0"));
    QVERIFY(hasRecordedCrop(raw));
    QCOMPARE(QFile(raw).size(), qint64(7)); // "RAWDATA", untouched.
}

void TestCropFiles::anExistingSidecarKeepsEverythingElse() {
    testsupport::TempCollection collection;
    QVERIFY(collection.isValid());
    const QString raw = collection.addRaw(QStringLiteral("IMG_1.ARW"));
    {
        const Exiv2::Image::UniquePtr sidecar =
            Exiv2::ImageFactory::create(Exiv2::ImageType::xmp, nativePath(sidecarPath(raw)));
        sidecar->xmpData()["Xmp.xmp.Rating"] = std::string("4");
        sidecar->xmpData()["Xmp.crs.Exposure2012"] = std::string("+0.50");
        sidecar->xmpData()["Xmp.crs.CropAngle"] = std::string("2.5");
        sidecar->writeMetadata();
    }

    QVERIFY(writeCameraRawCrop(raw, QRectF(0.0, 0.0, 0.5, 0.5)).isEmpty());

    const Exiv2::XmpData xmp = xmpOf(sidecarPath(raw));
    QCOMPARE(xmpText(xmp, "Xmp.xmp.Rating"), QStringLiteral("4"));
    QCOMPARE(xmpText(xmp, "Xmp.crs.Exposure2012"), QStringLiteral("+0.50"));
    QCOMPARE(xmpText(xmp, "Xmp.crs.CropRight"), QStringLiteral("0.500000"));
    // A straightening angle would tilt a crop drawn without one.
    QCOMPARE(xmpText(xmp, "Xmp.crs.CropAngle"), QStringLiteral("0"));
}

void TestCropFiles::anUnreadableSidecarIsReportedNotReplaced() {
    testsupport::TempCollection collection;
    QVERIFY(collection.isValid());
    const QString raw = collection.addRaw(QStringLiteral("IMG_1.RAF"));
    const QByteArray garbage = QByteArrayLiteral("not xmp at all");
    QVERIFY(!collection.addFile(QStringLiteral("IMG_1.xmp"), garbage).isEmpty());

    QVERIFY(!hasRecordedCrop(raw));
    QVERIFY(!writeCameraRawCrop(raw, QRectF(0.0, 0.0, 0.5, 0.5)).isEmpty());

    QFile sidecar(sidecarPath(raw));
    QVERIFY(sidecar.open(QIODevice::ReadOnly));
    QCOMPARE(sidecar.readAll(), garbage);
}

void TestCropFiles::aRawCropIsMeasuredInTheStoredFrame() {
    testsupport::TempCollection collection;
    QVERIFY(collection.isValid());
    const QString jpeg = collection.filePath(QStringLiteral("IMG_1.jpg"));
    QVERIFY(writeJpeg(halves(QSize(200, 100)), jpeg));
    tagExif(jpeg, 6);
    // Not a RAW the metadata library can read, so the JPG's orientation is
    // taken for it: the same camera wrote both at the same moment.
    const QString raw = collection.addRaw(QStringLiteral("IMG_1.CR2"));

    const QString error = saveCrop({jpeg, {raw}, QRectF(0.0, 0.0, 0.5, 0.25)});
    QVERIFY2(error.isEmpty(), qPrintable(error));

    QVERIFY(QFileInfo::exists(cropOutputPath(jpeg)));
    // The displayed top left, under orientation 6, is the stored bottom left.
    const Exiv2::XmpData xmp = xmpOf(sidecarPath(raw));
    QCOMPARE(xmpText(xmp, "Xmp.crs.CropLeft"), QStringLiteral("0.000000"));
    QCOMPARE(xmpText(xmp, "Xmp.crs.CropTop"), QStringLiteral("0.500000"));
    QCOMPARE(xmpText(xmp, "Xmp.crs.CropRight"), QStringLiteral("0.250000"));
    QCOMPARE(xmpText(xmp, "Xmp.crs.CropBottom"), QStringLiteral("1.000000"));
}

QTEST_MAIN(TestCropFiles)
#include "tst_crop_files.moc"
