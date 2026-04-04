#include "emailview.h"
#include "ui_emailview.h"

EmailView::EmailView(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::EmailView)
{
    ui->setupUi(this);
}

EmailView::~EmailView()
{
    delete ui;
}
