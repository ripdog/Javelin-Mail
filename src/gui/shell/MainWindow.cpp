#include "gui/shell/MainWindow.h"

#include "jmap/JmapCore.h"

#include <QFrame>
#include <QLabel>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

namespace
{

    QWidget* createPane(const QString& title, const QString& body, QWidget* parent)
    {
        auto* frame = new QFrame(parent);
        frame->setFrameShape(QFrame::StyledPanel);

        auto* layout = new QVBoxLayout(frame);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(8);

        auto* titleLabel = new QLabel(title, frame);
        auto* bodyLabel = new QLabel(body, frame);

        QFont titleFont = titleLabel->font();
        titleFont.setBold(true);
        titleFont.setPointSize(titleFont.pointSize() + 2);
        titleLabel->setFont(titleFont);

        bodyLabel->setWordWrap(true);

        layout->addWidget(titleLabel);
        layout->addWidget(bodyLabel);
        layout->addStretch();

        return frame;
    }

} // namespace

namespace javelin::gui::shell
{

    MainWindow::MainWindow(javelin::jmap::JmapCore& jmapCore, QWidget* parent)
        : QMainWindow(parent), m_jmapCore(jmapCore)
    {
        setupUi();
    }

    void MainWindow::setupUi()
    {
        setWindowTitle(QStringLiteral("Javelin Mail"));
        resize(1440, 900);

        auto* splitter = new QSplitter(Qt::Horizontal, this);
        splitter->addWidget(createPane(QStringLiteral("Mailboxes"),
                                       QStringLiteral("Cache-backed mailbox queries attach here."),
                                       splitter));
        splitter->addWidget(createPane(
            QStringLiteral("Messages"),
            QStringLiteral("Paged message list models replace the starter UI next."), splitter));
        splitter->addWidget(createPane(
            QStringLiteral("Message View"),
            QStringLiteral("Typed message view services will own message loading and rendering."),
            splitter));
        splitter->setStretchFactor(0, 1);
        splitter->setStretchFactor(1, 2);
        splitter->setStretchFactor(2, 3);

        setCentralWidget(splitter);
        statusBar()->showMessage(m_jmapCore.statusSummary());
    }

} // namespace javelin::gui::shell
