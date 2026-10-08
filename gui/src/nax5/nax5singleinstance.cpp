#include "nax5/nax5singleinstance.h"

#include <QCryptographicHash>
#include <QGuiApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QWindow>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace {
const char kRaise[] = "raise\n";
QLocalServer *g_server = nullptr;

// Only un-minimise, raise and activate: no other window-state change, so a running
// stream keeps its fullscreen/zoom/stretch mode. Under gamescope this may be a no-op.
void raiseWindows()
{
    for (QWindow *window : QGuiApplication::topLevelWindows())
    {
        if (!window->isVisible())
            continue;
        Qt::WindowStates states = window->windowStates();
        if (states.testFlag(Qt::WindowMinimized))
            window->setWindowStates(states.setFlag(Qt::WindowMinimized, false));  // keeps FullScreen/Maximized
        window->raise();
        window->requestActivate();
    }
}
}

QString nax5SingleInstanceName()
{
#ifdef Q_OS_WIN
    const QByteArray user = qgetenv("USERNAME") + '|' + qgetenv("USERDOMAIN");
#else
    QByteArray user = qgetenv("USER");
#ifdef Q_OS_UNIX
    user += '|' + QByteArray::number(static_cast<qulonglong>(::getuid()));
#endif
#endif
    // Unix: QLocalServer puts a socket file with this name in QDir::tempPath() (/tmp).
    return QStringLiteral("NAX5-client-") + QString::fromLatin1(
        QCryptographicHash::hash(user, QCryptographicHash::Sha256).toHex().left(16));
}

bool nax5AcquireSingleInstance(const QString &name)
{
    {
        // Unix: a missing socket file (ENOENT) or a stale one (ECONNREFUSED) fails at
        // once, so the 500 ms is only spent when a live client is slow to answer.
        QLocalSocket probe;
        probe.connectToServer(name);
        if (probe.waitForConnected(500))
        {
            probe.write(kRaise);
            probe.waitForBytesWritten(500);
            probe.disconnectFromServer();
            return false;
        }
    }
    // Nobody answered: a server name left by a crashed client is stale (Unix: the
    // socket file survives SIGKILL and would make listen() fail with AddressInUse).
    QLocalServer::removeServer(name);
    auto *server = new QLocalServer(qApp);
    server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server->listen(name))
    {
        delete server;
        return true;  // cannot guard (e.g. no permission): never block the player
    }
    g_server = server;
    QObject::connect(server, &QLocalServer::newConnection, server, [server]() {
        while (QLocalSocket *socket = server->nextPendingConnection())
        {
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [socket]() {
                if (socket->readAll().contains("raise"))
                    raiseWindows();
                socket->disconnectFromServer();
            });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    return true;
}

void nax5ReleaseSingleInstance()
{
    // Closing can take seconds (graceful stream stop, up to the 6 s safety exit); a
    // relaunch during that time must start a new client, not "raise" a dying one.
    if (!g_server)
        return;
    g_server->close();  // removes the socket file
    g_server->deleteLater();
    g_server = nullptr;
}
