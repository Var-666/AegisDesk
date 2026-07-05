#include "desktop/main_window.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QUrl>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);

    application.setApplicationName("AegisDesk");
    application.setOrganizationName("AegisDesk");

    QCommandLineParser parser;

    parser.setApplicationDescription(
        "AegisDesk service management desktop console"
    );

    parser.addHelpOption();

    QCommandLineOption agent_url_option(
        QStringList{"a", "agent-url"},
        "Agent base URL.",
        "url",
        "http://127.0.0.1:18081"
    );

    parser.addOption(agent_url_option);
    parser.process(application);

    const QUrl agent_url(
        parser.value(agent_url_option)
    );

    if (!agent_url.isValid() ||
        agent_url.scheme() != "http" ||
        agent_url.host().isEmpty()) {
        qCritical()
            << "Invalid --agent-url:"
            << agent_url;

        return 1;
        }

    aegis::desktop::MainWindow window(agent_url);

    window.show();

    return application.exec();
}