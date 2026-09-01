#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QLoggingCategory>
#include <QObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickView>
#include <QQuickWindow>
// #include <QTranslator>
#include <QLocale>
#include <QtLogging>
#include <QtSystemDetection>

#include "src/graphicscaps.h"

#include "src/app.h"
#include "src/translator.h"

//

#ifdef Q_OS_WINDOWS
#include <windows.h>  // for the debug terminal window

#include <cstdio>  // for the debug terminal window
#endif

// The transport. An embedded build compiles neither BLE backend -- and must
// not even include their headers, since those name Qt Bluetooth and SimpleBLE
// types that this build does not link.
#ifdef OSYNTHO_EMBEDDED
#include "src/embeddedmanager.h"
#else
#ifdef Q_OS_LINUX
#include "src/bluetoothmanager.h"
#endif
#ifdef Q_OS_ANDROID
#include "src/bluetoothmanager.h"
#endif
#ifdef Q_OS_APPLE
#include "src/bluetoothmanager.h"
#endif
#ifdef Q_OS_WINDOWS
#include "src/bluetoothmanager2.h"
#endif
#endif  // OSYNTHO_EMBEDDED

//

#include "src/singleinstance/singleinstance.h"

//

#include "src/synthcontroller.h"

int main(int argc, char* argv[]) {
#ifdef Q_OS_WINDOWS
  if (argc > 1 and QString::fromLocal8Bit(argv[1]).trimmed() == "--debug") {
    AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
  }
#endif

  qSetMessagePattern(
      "[%{time yyyyMMdd h:mm:ss.zzz}] "
      "%{if-debug}D%{endif}%{if-info}I%{endif}%{if-warning}W%{endif}%{if-critical}C%{endif}%{if-"
      "fatal}F%{endif} %{file}:%{line} - %{message}");

#if defined(Q_OS_WINDOWS) || (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)) || defined(Q_OS_MACOS)
  qputenv("QT_QUICK_CONTROLS_MATERIAL_VARIANT", QByteArray("Dense"));
#endif

#ifdef Q_OS_ANDROID
  // Force OpenGL ES path on Android — more consistent across device vendors
  // than the default Vulkan selection and has lower driver overhead for the 2D
  // painted-item UI. Must be set before QGuiApplication constructs the scene
  // graph backend.
  qputenv("QSG_RHI_BACKEND", "opengl");
#endif

  // app.setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#ifdef Q_OS_WIN
  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::Floor);
#endif

  QGuiApplication app(argc, argv);

  // Not a literal: APP_DISPLAY_NAME is per build variant (CMakeLists.txt), and
  // applicationName is the second half of what keeps the controller and the
  // standalone build apart. APP_ID separates their single-instance locks so both
  // can run; this separates AppDataLocation (<org>/<app>) so that when they do,
  // they are not two processes writing one SQLite file.
  app.setApplicationName(APP_DISPLAY_NAME);

  app.setOrganizationName("Osynth");
  app.setOrganizationDomain("osynth.org");

  app.setApplicationVersion(APP_VERSION);
  app.setWindowIcon(QIcon(":/assets/assets/graph.svg"));

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
  // Names the .desktop entry deploy_linux.sh installs (<APP_ID>.desktop), which
  // is how a Wayland compositor maps this window to its launcher -- app_id comes
  // straight from here. Without it both variants fall back to the executable
  // base name, which is "osyntho" for both, and whichever entry the compositor
  // picks first wins the icon for both windows.
  //
  // On X11 it is the .desktop file's StartupWMClass that has to match instead,
  // and there Qt builds WM_CLASS as <argv[0] basename> NUL <applicationName> --
  // the second field is the one that differs between the variants, so that is
  // what the entry declares.
  QGuiApplication::setDesktopFileName(APP_ID);
#endif

  // prevents many instances of the app to be launched. Must run BEFORE the
  // first App::instance() call below: constructing App opens the database and
  // runs schema migrations, which a doomed second instance must never do
  // concurrently with the live one.
  SingleInstance instance;
  if (instance.hasPrevious(APP_ID)) {
    return EXIT_SUCCESS;
  }
  // Return value deliberately not fatal: failing to claim the name costs the
  // single-instance guard, not the app. It is logged inside listen().
  instance.listen(APP_ID);
  // --

  const int openSansFontId =
      QFontDatabase::addApplicationFont(":/assets/assets/opensans/opensans.ttf");
  qDebug() << "Font | Open Sans font loaded id: " << openSansFontId;

  const int fontawesomeSolidFontId =
      QFontDatabase::addApplicationFont(":/assets/assets/fontawesome/freesolid6.otf");
  qDebug() << "Font | Font Awesome Solid font loaded id: " << fontawesomeSolidFontId;

  // Regular face of the same "Font Awesome 6 Free" family (weight 400 vs the
  // solid face's 900) — FA glyph Labels pin font.weight so the two never mix.
  const int fontawesomeRegularFontId =
      QFontDatabase::addApplicationFont(":/assets/assets/fontawesome/freeregular6.otf");
  qDebug() << "Font | Font Awesome Regular font loaded id: " << fontawesomeRegularFontId;

  // Translator::instance().setActiveLanguage(QLocale{}.name());
  // Translator::instance().setActiveLanguage("pt_BR");
  Translator::instance().setActiveLanguage(App::instance().setting("force_app_language"));

  QQmlApplicationEngine engine;
  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
      []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

  // metatypes

  // // only constructs singleton
  // Database::instance();
  // BluetoothManager::instance();
  // has the above singletons
  App::instance();
  App::instance().setQmlEngine(&engine);

  if (openSansFontId >= 0) {
    const QStringList fontFamilies = QFontDatabase::applicationFontFamilies(openSansFontId);
    qDebug() << "Font | Open Sans font families: " << fontFamilies;

    if (fontFamilies.count() > 0) {
      const int defaultFontSize = App::instance().setting("app_font_size").toInt();
      app.setFont(QFont{fontFamilies[0], defaultFontSize});
      qDebug() << "Font | Default app font set to: " << fontFamilies[0];
      qDebug() << "Font | Default app font size set to: " << defaultFontSize;
    }
  }

  if (fontawesomeSolidFontId >= 0) {
    const QStringList fontFamilies = QFontDatabase::applicationFontFamilies(fontawesomeSolidFontId);
    qDebug() << "Font | Font families : " << fontFamilies;
    if (not fontFamilies.empty()) {
      App::instance().setFontAwesomeFontName(fontFamilies[0]);
    }
  }

  if (fontawesomeRegularFontId >= 0) {
    const QStringList fontFamilies =
        QFontDatabase::applicationFontFamilies(fontawesomeRegularFontId);
    qDebug() << "Font | Font Awesome Regular font families : " << fontFamilies;
    if (not fontFamilies.empty()) {
      App::instance().setFontAwesomeRegularFontName(fontFamilies[0]);
    }
  }

  qDebug() << "Font | System-discovered Font Awesome Font Name after loading font file: "
           << App::instance().getFontAwesomeFontName();

  // turns on (android, windows) bluetooth logging
  // QLoggingCategory::setFilterRules("qt.bluetooth* = true");

  // turns on binding removal logging
  // QLoggingCategory::setFilterRules(QStringLiteral("qt.qml.binding.removal.info=true"));

  // App, Synth, Tr and BluetoothManager are declared QML singletons of the
  // org.osynth.osyntho module now. They used to be context properties, which
  // no compiler can see through: every binding that touched one fell back to
  // interpreted byte code. See tools/qml_aot_score.py for the coverage.

  engine.loadFromModule("org.osynth.osyntho", "Main");

  // A second launch exits immediately (see hasPrevious() above) — but exiting
  // in silence is indistinguishable from failing to start, especially when this
  // window is minimised or behind something. Bring it forward instead, which is
  // what double-clicking the icon was asking for. The signal had no connection
  // at all before, so the second process simply vanished.
  QObject::connect(&instance, &SingleInstance::newInstance, &app, [&engine]() {
    if (engine.rootObjects().isEmpty()) return;
    auto* win = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    if (win == nullptr) return;
    qDebug() << "App | Another instance was started; raising this window.";
    win->show();
    // Un-minimise without disturbing a maximised/fullscreen window.
    win->setWindowStates(win->windowStates() & ~Qt::WindowMinimized);
    win->raise();
    win->requestActivate();
  });

  // Detect the GL renderer once the scene graph has a live OpenGL context, so
  // the graph drawers can avoid the FBO paint path on drivers that corrupt it
  // (e.g. Adreno 5xx on Android 9). sceneGraphInitialized fires on the render
  // thread with the context current; marshal the resulting string back to the
  // GUI thread for GraphicsCaps. Non-OpenGL backends (D3D, Vulkan) leave the
  // renderer empty, falling back to the platform default render target.
  if (not engine.rootObjects().isEmpty()) {
    if (auto* win = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst())) {
      QObject::connect(
          win, &QQuickWindow::sceneGraphInitialized, win,
          [win]() {
            QString renderer;
            if (QOpenGLContext* ctx = QOpenGLContext::currentContext()) {
              if (QOpenGLFunctions* f = ctx->functions()) {
                if (const GLubyte* s = f->glGetString(GL_RENDERER))
                  renderer = QString::fromUtf8(reinterpret_cast<const char*>(s));
              }
            }
            qDebug() << "Graphics | GL_RENDERER:" << renderer;
            QMetaObject::invokeMethod(
                qApp, [renderer]() { GraphicsCaps::instance().setRenderer(renderer); },
                Qt::QueuedConnection);
          },
          Qt::DirectConnection);
    }
  }

  app.exec();

  qDebug() << "App | Finishing...";

#ifdef OSYNTHO_EMBEDDED
  EmbeddedManager::instance().finish();
#else
  BluetoothManager::instance().finish();
#endif

  return 0;
}
