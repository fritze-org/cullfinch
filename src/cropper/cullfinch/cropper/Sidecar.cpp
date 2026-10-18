// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/cropper/Sidecar.h>

#include <cullfinch/cropper/CropWriter.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <exiv2/exiv2.hpp>

#include <string>

namespace cullfinch::cropper {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("cullfinch::cropper::Sidecar", text);
}

std::string nativePath(const QString& path) {
    return QFile::encodeName(path).toStdString();
}

/// Camera Raw writes six decimals; more would only be noise.
std::string fraction(double value) {
    return QString::number(value, 'f', 6).toStdString();
}

} // namespace

QString sidecarPath(const QString& rawPath) {
    const QFileInfo info(rawPath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral(".xmp"));
}

bool hasRecordedCrop(const QString& rawPath) {
    const QString path = sidecarPath(rawPath);
    if (!QFileInfo::exists(path)) {
        return false;
    }
    initialiseMetadataLibrary();
    try {
        const Exiv2::Image::UniquePtr sidecar = Exiv2::ImageFactory::open(nativePath(path));
        sidecar->readMetadata();
        const Exiv2::XmpData& xmp = sidecar->xmpData();
        const auto found = xmp.findKey(Exiv2::XmpKey("Xmp.crs.HasCrop"));
        return found != xmp.end() &&
               QString::fromStdString(found->toString())
                       .compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
    } catch (const Exiv2::Error&) {
        // Unreadable is not cropped: the photo is offered again, and saving
        // reports the sidecar instead of overwriting it.
        return false;
    }
}

QString writeCameraRawCrop(const QString& rawPath, const QRectF& storedRect) {
    initialiseMetadataLibrary();
    const QString path = sidecarPath(rawPath);
    try {
        Exiv2::Image::UniquePtr sidecar;
        if (QFileInfo::exists(path)) {
            sidecar = Exiv2::ImageFactory::open(nativePath(path));
            sidecar->readMetadata();
        } else {
            sidecar = Exiv2::ImageFactory::create(Exiv2::ImageType::xmp, nativePath(path));
        }
        Exiv2::XmpData& xmp = sidecar->xmpData();
        xmp["Xmp.crs.HasCrop"] = std::string("True");
        xmp["Xmp.crs.CropTop"] = fraction(storedRect.top());
        xmp["Xmp.crs.CropLeft"] = fraction(storedRect.left());
        xmp["Xmp.crs.CropBottom"] = fraction(storedRect.bottom());
        xmp["Xmp.crs.CropRight"] = fraction(storedRect.right());
        // Straightening is out of scope; a stale angle would tilt this crop.
        xmp["Xmp.crs.CropAngle"] = std::string("0");
        sidecar->writeMetadata();
    } catch (const Exiv2::Error& error) {
        return tr("'%1' could not be written: %2").arg(path, QString::fromLocal8Bit(error.what()));
    }
    return {};
}

int storedOrientation(const QString& path) {
    initialiseMetadataLibrary();
    try {
        const Exiv2::Image::UniquePtr image = Exiv2::ImageFactory::open(nativePath(path));
        image->readMetadata();
        const Exiv2::ExifData& exif = image->exifData();
        const auto found = exif.findKey(Exiv2::ExifKey("Exif.Image.Orientation"));
        if (found == exif.end() || found->count() == 0) {
            return 0;
        }
        const auto value = found->toInt64();
        return value >= 1 && value <= 8 ? static_cast<int>(value) : 0;
    } catch (const Exiv2::Error&) {
        return 0;
    }
}

} // namespace cullfinch::cropper
