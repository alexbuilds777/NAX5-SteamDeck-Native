#pragma once

#include <QString>

// Product mode allows one NAX5 client per OS user. A second launch asks the
// running one to show its window and exits: two clients of one player sent two
// telemetry streams and two probes and shared one report journal (u92, 28.09).
// Returns false when another instance is already running (the caller exits).
// Must be called after QGuiApplication exists and before anything with side
// effects (settings, logs, windows, controllers).
bool nax5AcquireSingleInstance(const QString &name);

// Stop answering as the running instance (call when shutdown starts).
void nax5ReleaseSingleInstance();

// Server name per OS user, so different accounts do not block each other.
// Windows: USERNAME|USERDOMAIN. Unix: USER plus the numeric uid (USER may be unset).
QString nax5SingleInstanceName();
