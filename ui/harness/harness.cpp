// Offscreen render of Main.qml with a `logos` bridge that forwards every call to a REAL
// reelcut_core running under logosctl (session "reelcut", via ~/port-0.3/ctl.sh). Drives a scripted
// tour and writes screenshots. Not a layout oracle for Basecamp's bundled design system.
#include <QGuiApplication>
#include <QQuickView>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QProcess>
#include <QJSValue>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QImage>

class Bridge : public QObject {
  Q_OBJECT
public:
  QString ctl;
  Q_INVOKABLE void callModuleAsync(QString mod, QString method, QVariantList args, QJSValue cb, int) {
    QStringList a{QString::fromLocal8Bit(qgetenv("SESS").isEmpty() ? "reelcut" : qgetenv("SESS")), "call", mod, method};
    // Typed as the view sent them: bools and numbers bare, the rest as strings.
    for (auto& v : args) {
      const int t = v.typeId();
      if (t == QMetaType::Bool || t == QMetaType::Int || t == QMetaType::Double || t == QMetaType::LongLong) a << v.toString();
      else a << "str:" + v.toString();
    }
    auto* p = new QProcess(this);
    connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [p, cb](int, QProcess::ExitStatus) mutable {
      QByteArray out = p->readAllStandardOutput();
      QJsonObject o = QJsonDocument::fromJson(out.trimmed().split('\n').last()).object();
      QString res = o.value("result").toString();
      if (cb.isCallable()) cb.call({QJSValue(res)});
      p->deleteLater();
    });
    p->start(ctl, a);
  }
};

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  Bridge b; b.ctl = QString::fromLocal8Bit(qgetenv("CTL"));
  QQuickView v;
  v.rootContext()->setContextProperty("logos", &b);
  v.setResizeMode(QQuickView::SizeRootObjectToView);
  v.resize(1200, 760);
  QObject::connect(v.engine(), &QQmlEngine::warnings, [](const QList<QQmlError>& w) { for (auto& e : w) fprintf(stderr, "QMLWARN %s\n", qPrintable(e.toString())); });
  v.setSource(QUrl::fromLocalFile(argv[1]));
  if (v.status() != QQuickView::Ready) { for (auto& e : v.errors()) fprintf(stderr, "ERR %s\n", qPrintable(e.toString())); return 1; }
  v.show();
  QString out = argv[2];
  QStringList steps = QString(argv[3]).split(";;");
  int i = 0;
  auto* t = new QTimer(&app);
  QObject::connect(t, &QTimer::timeout, [&]() {
    QImage img = v.grabWindow();
    img.save(QString("%1/%2.png").arg(out).arg(i, 2, 10, QChar('0')));
    if (i >= steps.size()) { app.quit(); return; }
    QQmlExpression ex(QQmlEngine::contextForObject(v.rootObject()), v.rootObject(), steps[i]);
    ex.evaluate();
    if (ex.hasError()) fprintf(stderr, "STEPERR %s: %s\n", qPrintable(steps[i]), qPrintable(ex.error().toString()));
    i++;
  });
  t->start(2500);
  return app.exec();
}
#include "harness.moc"
