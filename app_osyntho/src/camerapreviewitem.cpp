#include "camerapreviewitem.h"

#include <QCamera>
#include <QImageCapture>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QPainter>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>

CameraPreviewItem::CameraPreviewItem(QQuickItem* parent)
    : QQuickPaintedItem(parent) {
    const QCameraDevice device = QMediaDevices::defaultVideoInput();
    if (device.isNull()) {
        qWarning() << "CameraPreviewItem: no camera device found";
        return;
    }

    m_camera = new QCamera(device, this);
    m_session = new QMediaCaptureSession(this);
    m_imageCapture = new QImageCapture(this);
    m_videoSink = new QVideoSink(this);

    m_session->setCamera(m_camera);
    m_session->setImageCapture(m_imageCapture);
    m_session->setVideoSink(m_videoSink);

    connect(m_videoSink, &QVideoSink::videoFrameChanged, this,
        [this](const QVideoFrame& frame) {
            m_currentImage = frame.toImage();
            update();
        }, Qt::QueuedConnection);

    connect(m_imageCapture, &QImageCapture::readyForCaptureChanged, this,
        [this](bool ready) { setReadyForCapture(ready); });

    connect(m_imageCapture, &QImageCapture::imageSaved, this,
        [this](int, const QString& path) { emit imageSaved(path); });

    connect(m_imageCapture, &QImageCapture::errorOccurred, this,
        [this](int, QImageCapture::Error, const QString& msg) {
            qWarning() << "CameraPreviewItem: capture error:" << msg;
            emit captureError(msg);
        });

    m_camera->start();
}

CameraPreviewItem::~CameraPreviewItem() {
    if (m_camera) m_camera->stop();
}

void CameraPreviewItem::captureToFile(const QString& path) {
  if (m_imageCapture and m_imageCapture->isReadyForCapture()) m_imageCapture->captureToFile(path);
}

void CameraPreviewItem::stop() {
  if (not m_camera) return;
  // Disconnect frame updates immediately so no further repaints are queued.
  // Then defer the actual camera stop by one event-loop tick so any already-queued
  // V4L2 socket-notifier event can fire and complete before the fd is closed.
  if (m_videoSink) QObject::disconnect(m_videoSink, &QVideoSink::videoFrameChanged, this, nullptr);
  QTimer::singleShot(0, m_camera, &QCamera::stop);
}

bool CameraPreviewItem::isReadyForCapture() const {
    return m_readyForCapture;
}

void CameraPreviewItem::setReadyForCapture(bool v) {
    if (m_readyForCapture == v) return;
    m_readyForCapture = v;
    emit readyForCaptureChanged();
}

void CameraPreviewItem::paint(QPainter* painter) {
    painter->fillRect(boundingRect(), Qt::black);
    if (m_currentImage.isNull()) return;

    const QSizeF itemSize = boundingRect().size();
    const QSize scaled = m_currentImage.size().scaled(itemSize.toSize(), Qt::KeepAspectRatio);
    const int x = (itemSize.width() - scaled.width()) / 2;
    const int y = (itemSize.height() - scaled.height()) / 2;
    painter->drawImage(QRect(x, y, scaled.width(), scaled.height()), m_currentImage);
}
