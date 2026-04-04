#ifndef EMAILVIEW_H
#define EMAILVIEW_H

#include <QMainWindow>

QT_BEGIN_NAMESPACE
namespace Ui {
class EmailView;
}
QT_END_NAMESPACE

class EmailView : public QMainWindow
{
    Q_OBJECT

public:
    explicit EmailView(QWidget *parent = nullptr);
    ~EmailView() override;

private:
    Ui::EmailView *ui;
};
#endif // EMAILVIEW_H
