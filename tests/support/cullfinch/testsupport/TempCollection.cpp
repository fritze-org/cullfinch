// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/testsupport/TempCollection.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QPainter>

namespace cullfinch::testsupport {

TempCollection::TempCollection() : directory_(std::make_unique<QTemporaryDir>()) {
    directory_->setAutoRemove(true);
}

bool TempCollection::isValid() const {
    return directory_ != nullptr && directory_->isValid();
}

QString TempCollection::path() const {
    return directory_->path();
}

QString TempCollection::filePath(const QString& relative) const {
    return QDir(directory_->path()).absoluteFilePath(relative);
}

QString TempCollection::addJpeg(const QString& relative, const QSize& size) {
    QString target = filePath(relative);
    QDir().mkpath(QFileInfo(target).absolutePath());

    // A deterministic colour per name, so a test can tell the photos apart.
    const QByteArray digest = QCryptographicHash::hash(relative.toUtf8(), QCryptographicHash::Md5);
    QImage image(size, QImage::Format_RGB32);
    image.fill(QColor(static_cast<quint8>(digest.at(0)), static_cast<quint8>(digest.at(1)),
                      static_cast<quint8>(digest.at(2))));

    QPainter painter(&image);
    painter.setPen(Qt::white);
    painter.drawText(image.rect(), Qt::AlignCenter, QFileInfo(relative).completeBaseName());
    painter.end();

    QImageWriter writer(target, QByteArrayLiteral("jpeg"));
    writer.setQuality(85);
    if (!writer.write(image)) {
        return QString();
    }
    return target;
}

QString TempCollection::addRaw(const QString& relative, const QByteArray& contents) {
    return addFile(relative, contents);
}

QString TempCollection::addFile(const QString& relative, const QByteArray& contents) {
    QString target = filePath(relative);
    QDir().mkpath(QFileInfo(target).absolutePath());
    QFile file(target);
    if (!file.open(QIODevice::WriteOnly)) {
        return QString();
    }
    file.write(contents);
    file.close();
    return target;
}

QString TempCollection::addCorruptJpeg(const QString& relative) {
    QString source = addJpeg(relative);
    if (source.isEmpty()) {
        return QString();
    }
    QFile file(source);
    if (!file.open(QIODevice::ReadWrite)) {
        return QString();
    }
    // Keep the JPEG magic so the format is recognised, then cut the data short.
    const QByteArray head = file.read(24);
    file.resize(0);
    file.write(head);
    file.close();
    return source;
}

bool TempCollection::addDirectory(const QString& relative) {
    return QDir(directory_->path()).mkpath(relative);
}

bool TempCollection::supportsCaseDistinctNames() const {
    // Generated at run time: committing filenames that cannot coexist on a
    // case-insensitive volume would make the repository unusable there.
    const QString lower = filePath(QStringLiteral("__case_probe.tmp"));
    const QString upper = filePath(QStringLiteral("__CASE_PROBE.tmp"));

    QFile first(lower);
    if (!first.open(QIODevice::WriteOnly)) {
        return false;
    }
    first.write(QByteArrayLiteral("a"));
    first.close();

    const bool distinct = !QFileInfo::exists(upper);
    QFile::remove(lower);
    return distinct;
}

} // namespace cullfinch::testsupport
