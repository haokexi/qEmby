#include "components/detailbottominfowidget.h"

#include <QApplication>
#include <QEvent>
#include <QEventLoop>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtTest>

namespace {

class LayoutEventCounter : public QObject {
public:
  explicit LayoutEventCounter(QWidget *root) : m_root(root) {
    qApp->installEventFilter(this);
  }

  ~LayoutEventCounter() override { qApp->removeEventFilter(this); }

  void reset() {
    layoutRequests = 0;
    resizeEvents = 0;
  }

  int layoutRequests = 0;
  int resizeEvents = 0;

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget || !m_root ||
        (widget != m_root && !m_root->isAncestorOf(widget))) {
      return false;
    }

    if (event->type() == QEvent::LayoutRequest)
      ++layoutRequests;
    else if (event->type() == QEvent::Resize)
      ++resizeEvents;
    return false;
  }

private:
  QWidget *m_root;
};

MediaItem mediaItemWithMetadata(int tagCount) {
  MediaItem item;
  for (int i = 0; i < tagCount; ++i)
    item.tags.append(QStringLiteral("Long metadata tag %1").arg(i));

  MediaStudioInfo studio;
  studio.id = QStringLiteral("studio-id");
  studio.name = QStringLiteral("Studio");
  item.studios.append(studio);

  MediaExternalUrlInfo externalUrl;
  externalUrl.name = QStringLiteral("External database");
  externalUrl.url = QStringLiteral("https://example.com");
  item.externalUrls.append(externalUrl);
  return item;
}

void waitForLayout() {
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QTest::qWait(80);
}

QList<QPushButton *> metadataButtons(const QWidget *root) {
  return root->findChildren<QPushButton *>(QStringLiteral("detail-genre-tag"));
}

} // namespace

class LayoutFeedbackTest : public QObject {
  Q_OBJECT

private slots:
  void detailMetadataLayoutSettles();
};

void LayoutFeedbackTest::detailMetadataLayoutSettles() {
  QWidget window;
  auto *windowLayout = new QVBoxLayout(&window);
  auto *details = new DetailBottomInfoWidget(&window);
  windowLayout->addWidget(details);

  LayoutEventCounter events(details);
  window.resize(1000, 700);
  window.show();
  details->setInfo(mediaItemWithMetadata(12), {});
  waitForLayout();

  auto buttons = metadataButtons(details);
  QCOMPARE(buttons.size(), 14);
  QWidget *tagHost = buttons.first()->parentWidget();
  QVERIFY(tagHost);
  const int wideWidth = tagHost->width();
  const int wideHeight = tagHost->height();
  QVERIFY(wideWidth > 0);
  QVERIFY(wideHeight > 0);

  events.reset();
  waitForLayout();
  QVERIFY2(events.layoutRequests <= 2,
           qPrintable(QStringLiteral("idle layout requests: %1")
                          .arg(events.layoutRequests)));
  QVERIFY2(events.resizeEvents <= 2,
           qPrintable(QStringLiteral("idle resize events: %1")
                          .arg(events.resizeEvents)));

  window.resize(420, 700);
  QTRY_VERIFY_WITH_TIMEOUT(tagHost->width() < wideWidth, 500);
  QTRY_VERIFY_WITH_TIMEOUT(tagHost->height() > wideHeight, 500);
  waitForLayout();

  events.reset();
  waitForLayout();
  QVERIFY2(events.layoutRequests <= 2,
           qPrintable(QStringLiteral("post-resize layout requests: %1")
                          .arg(events.layoutRequests)));

  details->setInfo(mediaItemWithMetadata(18), {});
  waitForLayout();
  QCOMPARE(metadataButtons(details).size(), 20);

  events.reset();
  waitForLayout();
  QVERIFY2(events.layoutRequests <= 2,
           qPrintable(QStringLiteral("post-content layout requests: %1")
                          .arg(events.layoutRequests)));

  for (int i = 0; i < 12; ++i) {
    window.resize((i % 2 == 0) ? 900 : 440, 700);
    details->setInfo(mediaItemWithMetadata(6 + (i % 5)), {});
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
  }
  waitForLayout();
  QCOMPARE(metadataButtons(details).size(), 9);

  events.reset();
  waitForLayout();
  QVERIFY2(events.layoutRequests <= 2,
           qPrintable(QStringLiteral("post-stress layout requests: %1")
                          .arg(events.layoutRequests)));

  details->clear();
  waitForLayout();
  QVERIFY(metadataButtons(details).isEmpty());

  events.reset();
  waitForLayout();
  QVERIFY2(events.layoutRequests <= 2,
           qPrintable(QStringLiteral("post-clear layout requests: %1")
                          .arg(events.layoutRequests)));
}

QTEST_MAIN(LayoutFeedbackTest)
#include "layoutfeedbacktest.moc"
