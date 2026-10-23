#include "ownedprocess.h"
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace Mandarina {
OwnedProcess::OwnedProcess()
{
#ifdef Q_OS_WIN
    setCreateProcessArgumentsModifier([this](CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW | CREATE_SUSPENDED;
        // Borrow Qt's own PROCESS_INFORMATION until startOwned returns. It is
        // filled by CreateProcess after this callback; never replace or free it.
        m_creationInfo = args->processInformation;
    });
    connect(this, &QProcess::stateChanged, this, [this](QProcess::ProcessState value) {
        if (value == NotRunning) m_creationInfo = nullptr;
    });
#else
    setChildProcessModifier([] { ::setsid(); });
#endif
}
OwnedProcess::~OwnedProcess() { stopOwned(); finishOwnedTree(); }
bool OwnedProcess::startOwned(OpenMode mode, int timeoutMs)
{
#ifdef Q_OS_WIN
    m_creationInfo = nullptr;
    m_job = CreateJobObjectW(nullptr, nullptr);
    if (!m_job) return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        finishOwnedTree();
        return false;
    }
#endif
    start(mode);
    if (!waitForStarted(timeoutMs)) { stopOwned(); finishOwnedTree(); return false; }
    m_ownedPid = processId();
#ifdef Q_OS_WIN
    // With CREATE_SUSPENDED this child cannot exit or spawn descendants yet.
    // Qt 6.8 retains both handles until cleanup; do not close borrowed handles.
    const auto *info = static_cast<const PROCESS_INFORMATION *>(m_creationInfo);
    const bool valid = state() == Running && info && info->dwProcessId == DWORD(m_ownedPid)
                       && info->hProcess && info->hThread;
    const bool assigned = valid && AssignProcessToJobObject(m_job, info->hProcess);
    const bool resumed = assigned && ResumeThread(info->hThread) == DWORD(1);
    m_creationInfo = nullptr;
    if (!assigned || !resumed) {
        // A child that cannot be contained never runs or converts user files.
        kill(); waitForFinished(2000); finishOwnedTree(); return false;
    }
#endif
    return true;
}
void OwnedProcess::stopOwned()
{
    if (state() == NotRunning) return;
#ifdef Q_OS_WIN
    if (m_job) TerminateJobObject(m_job, 1);
    else kill();
    waitForFinished(2000);
#else
    if (m_ownedPid > 1) ::kill(-pid_t(m_ownedPid), SIGTERM);
    terminate();
    if (!waitForFinished(750)) { finishOwnedTree(); kill(); waitForFinished(2000); }
#endif
}
void OwnedProcess::finishOwnedTree()
{
#ifdef Q_OS_WIN
    if (m_job) { CloseHandle(m_job); m_job = nullptr; }
#else
    if (m_ownedPid > 1) ::kill(-pid_t(m_ownedPid), SIGKILL);
#endif
    m_ownedPid = 0;
}
}
