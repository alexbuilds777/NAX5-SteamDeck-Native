#pragma once

#include "nax5/session/nax5sessionerror.h"
#include "nax5/session/nax5sessionstate.h"

#include <QByteArray>
#include <QString>
#include <QtGlobal>

enum Nax5TerminalMutation
{
    Nax5TerminalMutationNone = 0,
    Nax5TerminalMutationCancel,
    Nax5TerminalMutationFail,
    Nax5TerminalMutationEnd
};

Nax5TerminalMutation nax5ShutdownMutation(Nax5GameSessionState state, bool stream_was_connected);
Nax5TerminalMutation nax5StreamQuitMutation(bool stream_was_connected, bool operator_test);
Nax5TerminalMutation nax5ReleaseMutation(Nax5GameSessionState state, bool stream_was_connected);
bool nax5SessionTerminalBlocksPlay(Nax5TerminalMutation mutation);
bool nax5SessionNeedsQuitRelease(Nax5GameSessionState state);
bool nax5ShutdownNeedsCurrentSync(Nax5GameSessionState state);
bool nax5ShouldSendTerminalBeforeUnauthLogout(bool has_session_id, bool has_token, Nax5TerminalMutation mutation);
bool nax5PlayEligibilityOk(bool authenticated, bool email_verified, const QString &access_status);
bool nax5AcceptAsync(quint64 live_generation, quint64 event_generation, quint64 live_request_id, quint64 event_request_id);
bool nax5AcceptSessionIdentity(const QString &live_session_id, const QString &event_session_id);
int nax5TerminalRetryLimit();
// Pause before chiaki re-requests a Remote Play session that failed to start
// (e.g. the console is still releasing the previous one: "Remote is already in use").
int nax5StreamRetryDelayMs(qint64 elapsed_since_first_attempt_ms);
int nax5ShutdownGraceMs();
int nax5ShutdownReportGraceMs();
bool nax5MaySleepConsole(bool operator_mode);
bool nax5MayResumeAfterOsSleep(bool operator_mode);
bool nax5ShowsChiakiQuitDialog(bool operator_mode);
bool nax5OperatorHostConnectAllowed(bool operator_mode);
bool nax5StreamConnectedOnNewGeneration();
bool nax5StreamFirstFrameSeenOnNewGeneration();
bool nax5ShouldRetryMarkConnected(Nax5GameSessionState state, bool shutdown_started, Nax5SessionError error);
bool nax5ProductShouldWakeupBeforeCreateSession();
int nax5ProductWakeupSendsPerStartStream();
bool nax5OperatorConnectShouldWakeup(bool discovered, bool standby);
QString nax5WakeupHostKindName(const QString &host);

struct Nax5MaterialWakeup
{
    QString host;
    QByteArray regist_key;
    bool ps5;
    bool ready;

    Nax5MaterialWakeup()
        : ps5(false)
        , ready(false)
    {
    }
};

Nax5MaterialWakeup nax5MaterialWakeupCall(const QString &host, const QByteArray &regist_key, bool ps5);

// Play time for people: whole minutes, never rounded up ("1 ч 12 мин").
QString nax5FormatPlayTime(qint64 seconds);
// 600, 300 or 60 when the time left has just dropped to ten, five or one minute; 0 otherwise.
int nax5LowTimeThreshold(qint64 previous_remaining, qint64 remaining);
QString nax5LowTimeNotice(int threshold);
// How long the notice stays over the stream.
int nax5LowTimeNoticeMs(int threshold);
// One line about the single console for the play panel; empty when the server says nothing.
QString nax5ConsoleStatusText(const QString &state, qint64 free_in_seconds, bool just_freed);
// What to tell the player when the server ended the session; `reason` comes from the closing answer.
QString nax5SessionClosedText(const QString &reason);
bool nax5SessionClosedForBalance(const QString &reason);

// How long «Играть» stays blocked after the server refused; the server's own pause when it tells one.
int nax5ReserveRetryPauseMs(qint64 retry_after_seconds);
