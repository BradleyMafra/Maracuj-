#pragma once
#include <QProcess>

namespace Mandarina {
// Only this object's child and descendants are stopped. Windows starts the
// child suspended, assigns its Job Object, then resumes it: no spawn race.
class OwnedProcess final : public QProcess {
public:
    OwnedProcess();
    ~OwnedProcess() override;
    bool startOwned(OpenMode mode = ReadOnly, int timeoutMs = 5000);
    void stopOwned();
    void finishOwnedTree();
private:
    qint64 m_ownedPid = 0;
#ifdef Q_OS_WIN
    void *m_job = nullptr;
    void *m_creationInfo = nullptr;
#endif
};
}
