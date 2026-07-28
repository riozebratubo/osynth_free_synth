#ifndef SINGLEINSTANCE_H
#define SINGLEINSTANCE_H

#include <QObject>
#include <QLocalServer>
#include <QLocalSocket>

class SingleInstance : public QObject
{
    Q_OBJECT
public:
    explicit SingleInstance(QObject *parent = 0);

    // Starts answering hasPrevious() for this name. False if the socket/pipe
    // could not be claimed, which means this run is NOT guarding anything and
    // a later launch will start a second copy — worth logging rather than
    // discarding, since the symptom (two apps on one database) shows up far
    // from the cause.
    bool listen(const QString &name);
    bool hasPrevious(const QString &name);

signals:
    // Another copy of the app was started and exited, handing over to this one.
    // Connect it to whatever brings the window forward: without that, a second
    // launch is a process that quits in silence, which reads as the app having
    // failed to start.
    void newInstance();

private:
 // QLocalSocket *m_socket;
 QLocalServer m_server;
};

#endif // SINGLEINSTANCE_H
