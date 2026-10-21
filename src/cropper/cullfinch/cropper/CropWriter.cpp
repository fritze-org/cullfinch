// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/cropper/CropWriter.h>

#include <cullfinch/cropper/CropGeometry.h>
#include <cullfinch/cropper/Sidecar.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QSet>
#include <QTemporaryFile>

#include <exiv2/exiv2.hpp>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <system_error>

namespace cullfinch::cropper {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("cullfinch::cropper::CropWriter", text);
}

/// Quality 95: high enough that a crop is not visibly worse than the camera's
/// own JPG, which is usually encoded at about the same setting.
constexpr int kJpegQuality = 95;

std::mutex& xmpMutex() {
    static std::mutex mutex;
    return mutex;
}

/// Serialises the XMP toolkit's namespace registry, which is global state the
/// toolkit only guards when it is handed a lock.
void lockXmp(void* /*data*/, bool lock) {
    if (lock) {
        xmpMutex().lock();
    } else {
        xmpMutex().unlock();
    }
}

std::string nativePath(const QString& path) {
    return QFile::encodeName(path).toStdString();
}

/// Set `key` to `value` only where the original recorded it: dimensions in
/// tags a camera never wrote would be inventing metadata.
template<typename Data, typename Key, typename Value>
void replaceIfPresent(Data& data, const char* key, const Value& value) {
    if (data.findKey(Key(key)) != data.end()) {
        data[key] = value;
    }
}

/// Copy the metadata of `sourcePath` onto the freshly encoded crop at
/// `targetPath`, adjusted to describe the crop rather than the original.
void copyMetadata(const QString& sourcePath, const QString& targetPath, const QSize& size) {
    const Exiv2::Image::UniquePtr source = Exiv2::ImageFactory::open(nativePath(sourcePath));
    source->readMetadata();
    const Exiv2::Image::UniquePtr target = Exiv2::ImageFactory::open(nativePath(targetPath));
    // Reading first keeps the colour profile the encoder wrote.
    target->readMetadata();

    const auto width = static_cast<std::uint32_t>(size.width());
    const auto height = static_cast<std::uint32_t>(size.height());

    Exiv2::ExifData exif = source->exifData();
    if (!exif.empty()) {
        // The thumbnail still shows the whole, uncropped photo.
        Exiv2::ExifThumb(exif).erase();
        // The pixels were decoded upright and are stored that way now;
        // keeping the old tag would turn them a second time.
        exif["Exif.Image.Orientation"] = static_cast<std::uint16_t>(1);
        exif["Exif.Photo.PixelXDimension"] = width;
        exif["Exif.Photo.PixelYDimension"] = height;
        replaceIfPresent<Exiv2::ExifData, Exiv2::ExifKey>(exif, "Exif.Image.ImageWidth", width);
        replaceIfPresent<Exiv2::ExifData, Exiv2::ExifKey>(exif, "Exif.Image.ImageLength", height);
    }
    target->setExifData(exif);
    target->setIptcData(source->iptcData());

    Exiv2::XmpData xmp = source->xmpData();
    replaceIfPresent<Exiv2::XmpData, Exiv2::XmpKey>(xmp, "Xmp.tiff.Orientation", std::string("1"));
    replaceIfPresent<Exiv2::XmpData, Exiv2::XmpKey>(xmp, "Xmp.exif.PixelXDimension",
                                                    std::to_string(width));
    replaceIfPresent<Exiv2::XmpData, Exiv2::XmpKey>(xmp, "Xmp.exif.PixelYDimension",
                                                    std::to_string(height));
    target->setXmpData(xmp);

    // The decoder leaves pixels in the file's colour space, so the original's
    // profile still describes them. The encoder normally writes it already.
    if (!target->iccProfileDefined() && source->iccProfileDefined()) {
        const Exiv2::DataBuf& profile = source->iccProfile();
        target->setIccProfile(Exiv2::DataBuf(profile.c_data(), profile.size()));
    }
    target->writeMetadata();
}

} // namespace

void initialiseMetadataLibrary() {
    static std::once_flag once;
    std::call_once(once, [] {
        Exiv2::XmpParser::initialize(lockXmp, nullptr);
        // A camera's odd maker note is not something to print for every
        // photo; real failures still arrive as exceptions.
        Exiv2::LogMsg::setLevel(Exiv2::LogMsg::error);
    });
}

QString cropOutputPath(const QString& jpegPath) {
    const QFileInfo info(jpegPath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral(".crop.") + info.suffix());
}

bool isCropOutputStem(const QString& stem) {
    return stem.endsWith(QLatin1String(".crop"));
}

QString writeJpegCrop(const QString& sourcePath, const QRectF& rect) {
    initialiseMetadataLibrary();

    QImageReader reader(sourcePath);
    reader.setAutoTransform(true); // The crop was drawn on the upright photo.

    // The crop needs every pixel, under the same allocation limit as 100%
    // inspection -- QtImageService sets it, and it is process-wide, so it is
    // read here rather than raised. A photo past it still previews, because
    // previews decode scaled; without this check its save would fail as
    // "could not be read", which says nothing about why.
    const QSize native = reader.size();
    const qint64 limitBytes = static_cast<qint64>(QImageReader::allocationLimit()) * 1024 * 1024;
    if (native.isValid() && limitBytes > 0 &&
        static_cast<qint64>(native.width()) * native.height() * 4 > limitBytes) {
        return tr("'%1' is %2 × %3 pixels, more than the %4 MB a decoded photo may use; it "
                  "cannot be cropped here.")
            .arg(sourcePath)
            .arg(native.width())
            .arg(native.height())
            .arg(QImageReader::allocationLimit());
    }

    const QImage image = reader.read();
    if (image.isNull()) {
        return tr("'%1' could not be read: %2").arg(sourcePath, reader.errorString());
    }
    const QImage cropped = image.copy(toPixels(rect, image.size()));

    const QFileInfo source(sourcePath);
    const QString target = cropOutputPath(sourcePath);
    // Hidden and without an image extension, so a scan running meanwhile
    // never takes the unfinished file for a photo.
    QTemporaryFile part(
        source.dir().filePath(QStringLiteral(".%1.XXXXXX.part").arg(source.completeBaseName())));
    if (!part.open()) {
        return tr("'%1' could not be created: %2").arg(target, part.errorString());
    }

    QImageWriter writer(&part, QByteArrayLiteral("jpeg"));
    writer.setQuality(kJpegQuality);
    if (!writer.write(cropped)) {
        return tr("'%1' could not be written: %2").arg(target, writer.errorString());
    }
    part.close();

    try {
        copyMetadata(sourcePath, part.fileName(), cropped.size());
    } catch (const Exiv2::Error& error) {
        return tr("The metadata of '%1' could not be copied: %2")
            .arg(sourcePath, QString::fromLocal8Bit(error.what()));
    }

    // Readable by whoever could read the original, rather than by its owner
    // only, which is all a temporary file allows.
    QFile::setPermissions(part.fileName(), QFile::permissions(sourcePath) | QFile::WriteOwner);

    // rename(2) replaces an earlier crop in one step; QFile::rename refuses
    // to replace at all.
    std::error_code failure;
    std::filesystem::rename(std::filesystem::path(nativePath(part.fileName())),
                            std::filesystem::path(nativePath(target)), failure);
    if (failure) {
        return tr("'%1' could not be written: %2")
            .arg(target, QString::fromStdString(failure.message()));
    }
    part.setAutoRemove(false); // It has become the crop.
    return {};
}

QString saveCrop(const CropJob& job) {
    QStringList errors;
    if (!job.jpegPath.isEmpty()) {
        if (const QString error = writeJpegCrop(job.jpegPath, job.rect); !error.isEmpty()) {
            errors.append(error);
        }
    }

    // Two RAWs of one photo, IMG_1.CR2 and IMG_1.DNG, share Adobe's sidecar.
    QSet<QString> written;
    for (const QString& raw : job.rawPaths) {
        if (written.contains(sidecarPath(raw))) {
            continue;
        }
        written.insert(sidecarPath(raw));
        // The RAW's own tag decides the frame its crop is measured in. A RAW
        // the library cannot read is assumed to be turned like its JPG, which
        // the same camera wrote at the same moment.
        int orientation = storedOrientation(raw);
        if (orientation == 0 && !job.jpegPath.isEmpty()) {
            orientation = storedOrientation(job.jpegPath);
        }
        if (const QString error = writeCameraRawCrop(raw, toStoredFrame(job.rect, orientation));
            !error.isEmpty()) {
            errors.append(error);
        }
    }
    return errors.join(QLatin1Char('\n'));
}

} // namespace cullfinch::cropper
