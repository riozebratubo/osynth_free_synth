#ifndef DATABASECLIENT_H
#define DATABASECLIENT_H

#include "src/business/database.h"
#include "src/business/idatabase.h"

// Mixin that gives any class an injectable database dependency.
//
// Defaults to the production Database singleton, so objects that are
// default-constructed (e.g. QML-instantiated view models) keep working with no
// wiring. Tests call setDatabase() to inject an in-memory Database
// (Database::create(...)) or a fake IDatabase before exercising the object.
//
// This is intentionally NOT a QObject: view models inherit it alongside their
// QObject/QAbstractListModel base without introducing a second QObject base.
class DatabaseClient {
 public:
  virtual ~DatabaseClient() = default;

  // Virtual so composite view models (those embedding a child view model) can
  // override to forward injection down to their children.
  virtual void setDatabase(IDatabase* db) { m_injectedDb = db; }

 protected:
  IDatabase& db() const { return m_injectedDb ? *m_injectedDb : Database::instance(); }

 private:
  IDatabase* m_injectedDb = nullptr;
};

#endif  // DATABASECLIENT_H
