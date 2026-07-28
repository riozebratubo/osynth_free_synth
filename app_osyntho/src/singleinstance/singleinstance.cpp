#include "singleinstance.h"

#include <QDebug>

SingleInstance::SingleInstance(QObject *parent) : QObject(parent)
{
  connect(&m_server, &QLocalServer::newConnection, this, [this]() { emit newInstance(); });
}

bool SingleInstance::listen(const QString &name)
{
    // Only reached once hasPrevious() has said no one is answering, so any
    // socket/pipe still sitting there is a leftover from a run that was killed
    // — removing it is what lets this instance claim the name at all.
    m_server.removeServer(name);
    if (m_server.listen(name)) return true;

    qWarning() << "SingleInstance | Could not listen on" << name << ":"
               << m_server.errorString() << "- a second copy of the app will not be prevented.";
    return false;
}

bool SingleInstance::hasPrevious(const QString &name)
{
    QLocalSocket socket;
    socket.connectToServer(name, QLocalSocket::ReadOnly);

    return socket.waitForConnected();
}
