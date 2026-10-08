// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_SESSIONLOG_H
#define CHIAKI_SESSIONLOG_H

#include <chiaki/log.h>

#include <QString>
#include <QDir>
#include <QMutex>
#include <QAtomicInteger>
#include <QByteArray>
#include <QElapsedTimer>

class QFile;
class StreamSession;

class SessionLog
{
	friend class SessionLogPrivate;

	private:
		StreamSession *session;
		ChiakiLog log;
		QFile *file;
		QMutex file_mutex;
		QAtomicInteger<bool> shutdown;
		bool sanitize;
		bool echo_stdout;
		// NAX5: lines are buffered and flushed on WARNING/ERROR, when the buffer exceeds
		// SESSION_LOG_FLUSH_BYTES, when SESSION_LOG_FLUSH_MS passed since the last flush,
		// on shutdown, and from FlushIfStale() (1 s timer in StreamSession) so a quiet session
		// does not keep lines in memory. All guarded by file_mutex.
		QByteArray write_buf;
		QElapsedTimer flush_timer;
		// started when write_buf goes from empty to non-empty (age of the oldest buffered line)
		QElapsedTimer buf_age_timer;
		QString session_id;

		void Log(ChiakiLogLevel level, const char *msg);
		void FlushLocked();

	public:
		SessionLog(StreamSession *session, uint32_t level_mask, const QString &filename, bool sanitize);
		~SessionLog();

		ChiakiLog *GetChiakiLog()	{ return &log; }
		// "<NAX5_SESSION_ID or local-<pid>>-<n>", n = 1-based index of this session in the process
		QString GetSessionId() const	{ return session_id; }
		void PrepareShutdown();
		// Flush if the buffer holds data older than SESSION_LOG_FLUSH_MS. Any thread.
		void FlushIfStale();
};

QString GetLogBaseDir();
QString CreateLogFilename();

#endif //CHIAKI_SESSIONLOG_H
