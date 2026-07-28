#include "graphicscaps.h"

GraphicsCaps& GraphicsCaps::instance() {
  static GraphicsCaps inst;
  return inst;
}

GraphicsCaps::GraphicsCaps(QObject* parent) : QObject(parent) {}

QString GraphicsCaps::renderer() const { return m_renderer; }

bool GraphicsCaps::rendererKnown() const { return m_rendererKnown; }

bool GraphicsCaps::isBlacklisted() const {
  if (not m_rendererKnown) return false;
  const QString r = m_renderer.toLower();
  // Older Adreno GLES drivers corrupt the OpenGL FBO paint path.
  return r.contains("adreno") and
         (r.contains("505") or r.contains("506") or r.contains("508") or r.contains("509"));
}

void GraphicsCaps::setRenderer(const QString& renderer) {
  if (m_rendererKnown and m_renderer == renderer) return;
  m_renderer = renderer;
  m_rendererKnown = true;
  emit changed();
}
