#include <QApplication>
#include <QIcon>
#include "MainWindow.h"
#include "ui/IconHelper.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setWindowIcon(themeIcon({"applications-system-symbolic",
                                 "applications-system",
                                 "preferences-system"}));
    MainWindow w;
    w.show();
    return app.exec();
}