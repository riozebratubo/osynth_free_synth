#pragma once
#include <QImage>
#include <QQuickPaintedItem>

class QCamera;
class QMediaCaptureSession;
class QImageCapture;
class QVideoSink;

class CameraPreviewItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(bool readyForCapture READ isReadyForCapture NOTIFY readyForCaptureChanged)

public:
    explicit CameraPreviewItem(QQuickItem* parent = nullptr);
    ~CameraPreviewItem() override;

    Q_INVOKABLE void captureToFile(const QString& path);
    Q_INVOKABLE void stop();
    bool isReadyForCapture() const;

    void paint(QPainter* painter) override;

signals:
    void imageSaved(const QString& path);
    void captureError(const QString& msg);
    void readyForCaptureChanged();

private:
    void setReadyForCapture(bool v);

    QCamera* m_camera = nullptr;
    QMediaCaptureSession* m_session = nullptr;
    QImageCapture* m_imageCapture = nullptr;
    QVideoSink* m_videoSink = nullptr;
    QImage m_currentImage;
    bool m_readyForCapture = false;
};
