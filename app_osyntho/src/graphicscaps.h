#ifndef GRAPHICSCAPS_H
#define GRAPHICSCAPS_H

#include <QObject>
#include <QString>

// Process-wide cache of the live graphics-driver capabilities, populated once
// at startup (from the root QQuickWindow's scene-graph init in main.cpp).
//
// Exists so the QQuickPaintedItem graph drawers can pick a safe render target:
// the OpenGL framebuffer paint path corrupts on some older GLES drivers
// (e.g. Adreno 5xx on Android 9), so those fall back to the Image raster path
// while modern drivers keep the faster FBO path. Pure capabilities holder — no
// App/DB dependency.
class GraphicsCaps : public QObject {
  Q_OBJECT

 public:
  static GraphicsCaps& instance();

  // The GL_RENDERER string of the scene-graph's OpenGL context, e.g.
  // "Adreno (TM) 506". Empty until detected, or when the scene graph uses a
  // non-OpenGL backend (Direct3D, Vulkan).
  QString renderer() const;
  bool rendererKnown() const;

  // True when renderer() matches a known-bad driver for the OpenGL paint
  // engine / FBO render target.
  bool isBlacklisted() const;

  // Called once from main.cpp after the renderer string has been read on the
  // GUI thread. Stores the value and emits changed() (no-op if unchanged).
  void setRenderer(const QString& renderer);

 signals:
  void changed();

 private:
  explicit GraphicsCaps(QObject* parent = nullptr);

  QString m_renderer;
  bool m_rendererKnown = false;
};

#endif  // GRAPHICSCAPS_H
