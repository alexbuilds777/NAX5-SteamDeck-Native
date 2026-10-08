// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <sessionlog.h>
#include <chiaki/log.h>
#include "nax5/nax5processlog.h"

#include <QStandardPaths>
#include <QDir>
#include <QRegularExpression>
#include <QDateTime>
#include <QFile>
#include <QPair>
#include <QVector>
#include <QThread>
#include <QAtomicInt>

// NAX5 buffered session log (see SessionLog::Log)
#define SESSION_LOG_FLUSH_BYTES (16 * 1024)
#define SESSION_LOG_FLUSH_MS 1000
// Qt supports at most "zzz" (milliseconds); the old "zzzzzz" printed the ms twice.
static const char *const SESSION_LOG_DATE_FORMAT = "yyyy-MM-dd HH:mm:ss.zzz";
static const char *const SESSION_LOG_FILE_DATE_FORMAT = "yyyy-MM-dd_HH-mm-ss-zzz";
static const char *const SESSION_LOG_FILE_DATE_FORMAT_LEGACY = "yyyy-MM-dd_HH-mm-ss-zzzzzz";

static void LogCb(ChiakiLogLevel level, const char *msg, void *user);
static QString SanitizeLogMessage(const QString &msg);

// NAX5_LOG_STDERR=1 echoes the engine log to stdout as upstream did; default off.
static bool nax5_log_echo_stdout()
{
	static const bool enabled = []() {
		const QByteArray v = qgetenv("NAX5_LOG_STDERR").trimmed();
		return v == "1";
	}();
	return enabled;
}

// 1-based index of the session log in this process, appended to the session id
static QString nax5_next_session_id()
{
	static QAtomicInt counter(0);
	const int n = counter.fetchAndAddRelaxed(1) + 1;
	return QStringLiteral("%1-%2").arg(nax5SessionIdBase()).arg(n);
}

SessionLog::SessionLog(StreamSession *session, uint32_t level_mask, const QString &filename, bool sanitize)
	: session(session), shutdown(false), sanitize(sanitize), echo_stdout(nax5_log_echo_stdout()),
	session_id(nax5_next_session_id())
{
	chiaki_log_init(&log, level_mask, LogCb, this);
	flush_timer.start();

	if(filename.isEmpty())
	{
		file = nullptr;
		CHIAKI_LOGI(&log, "Logging to file disabled");
	}
	else
	{
		file = new QFile(filename);
		if(!file->open(QIODevice::ReadWrite))
		{
			delete file;
			file = nullptr;
			CHIAKI_LOGI(&log, "Failed to open file %s for logging", filename.toLocal8Bit().constData());
		}
		else
		{
			// First line: identity for the wrapper / report tooling. Written raw (no
			// timestamp, no sanitizer) so that the id stays greppable.
			{
				QMutexLocker lock(&file_mutex);
				write_buf.append(QStringLiteral("NAX5 session_id=%1 build=%2 codec/host omitted\n")
					.arg(session_id, QString::fromUtf8(nax5_build_id())).toUtf8());
				FlushLocked();
			}
			CHIAKI_LOGI(&log, "Logging to file %s", filename.toLocal8Bit().constData());
		}
	}

	CHIAKI_LOGI(&log, "Chiaki Version " CHIAKI_VERSION);
}

SessionLog::~SessionLog()
{
	PrepareShutdown();
}

void SessionLog::PrepareShutdown()
{
	// Signal that we're shutting down - no more logging allowed
	shutdown.storeRelaxed(true);

	// Wait a moment for any in-flight log calls to complete
	QThread::msleep(10);

	// Now safe to clean up
	QMutexLocker lock(&file_mutex);
	if(file)
	{
		FlushLocked();
		delete file;
		file = nullptr;
	}
}

void SessionLog::FlushLocked()
{
	if(file && !write_buf.isEmpty())
	{
		file->write(write_buf);
		file->flush();
	}
	write_buf.clear();
	flush_timer.restart();
}

void SessionLog::FlushIfStale()
{
	if(shutdown.loadRelaxed())
		return;
	QMutexLocker lock(&file_mutex);
	if(!file || write_buf.isEmpty())
		return;
	if(buf_age_timer.isValid() && buf_age_timer.elapsed() >= SESSION_LOG_FLUSH_MS)
		FlushLocked();
}

void SessionLog::Log(ChiakiLogLevel level, const char *msg)
{
	QString sanitized_msg = sanitize ? SanitizeLogMessage(QString::fromUtf8(msg)) : QString::fromUtf8(msg);

	if(echo_stdout)
	{
		QByteArray sanitized_utf8 = sanitized_msg.toUtf8();
		chiaki_log_cb_print(level, sanitized_utf8.constData(), nullptr);
	}

	// Check if we're shutting down - if so, skip file logging
	if(shutdown.loadRelaxed())
		return;

	QMutexLocker lock(&file_mutex);
	if(file && !shutdown.loadRelaxed())
	{
		QString str = QString("[%1] [%2] %3\n").arg(
				QDateTime::currentDateTime().toString(QLatin1String(SESSION_LOG_DATE_FORMAT)),
				QString(chiaki_log_level_char(level)),
				sanitized_msg);
		if(write_buf.isEmpty())
			buf_age_timer.start();
		write_buf.append(str.toUtf8());

		const bool urgent = level == CHIAKI_LOG_WARNING || level == CHIAKI_LOG_ERROR;
		if(urgent || write_buf.size() > SESSION_LOG_FLUSH_BYTES || flush_timer.elapsed() > SESSION_LOG_FLUSH_MS)
			FlushLocked();
	}
}

class SessionLogPrivate
{
	public:
		static void Log(SessionLog *log, ChiakiLogLevel level, const char *msg) { log->Log(level, msg); }
};

static void LogCb(ChiakiLogLevel level, const char *msg, void *user)
{
	auto log = reinterpret_cast<SessionLog *>(user);
	SessionLogPrivate::Log(log, level, msg);
}

// Heap-allocated regexes that intentionally outlive static destruction,
// preventing use-after-free when background threads (e.g. takion) log
// while the process is shutting down.
static const QRegularExpression &sanitize_ipv4_re()
{
	static const auto *re = new QRegularExpression(
		R"(\b(?:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\.){3}(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\b)");
	return *re;
}
static const QRegularExpression &sanitize_ipv6_re()
{
	static const auto *re = new QRegularExpression(
		R"((?<![0-9A-Za-z])\[?(?:(?:[0-9A-Fa-f]{1,4}:){3,7}[0-9A-Fa-f]{1,4}|(?:[0-9A-Fa-f]{0,4}:){0,7}::(?:[0-9A-Fa-f]{0,4}:){0,7}[0-9A-Fa-f]{0,4})\]?(?![0-9A-Za-z]))");
	return *re;
}
static const QRegularExpression &sanitize_labeled_secret_re()
{
	static const auto *re = new QRegularExpression(
		R"((((?:console|host|server|session|account|psn|public|remote)\s+(?:id|ip|address)|duid)\s*:\s*)([^\s,;]+))",
		QRegularExpression::CaseInsensitiveOption);
	return *re;
}
static const QRegularExpression &sanitize_session_id_token_re()
{
	static const auto *re = new QRegularExpression(
		R"((session\s+id\s+)([A-Za-z0-9+/=_-]{8,}))",
		QRegularExpression::CaseInsensitiveOption);
	return *re;
}
static const QRegularExpression &sanitize_uuid_re()
{
	static const auto *re = new QRegularExpression(
		R"(\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}\b)");
	return *re;
}
static const QRegularExpression &sanitize_long_hex_re()
{
	static const auto *re = new QRegularExpression(
		R"(\b[a-fA-F0-9]{16,}\b)");
	return *re;
}
static const QRegularExpression &sanitize_account_id_re()
{
	static const auto *re = new QRegularExpression(
		R"((account(?:_id)?\s*=\s*)([^\s,;]+))", QRegularExpression::CaseInsensitiveOption);
	return *re;
}
static const QRegularExpression &sanitize_duid_re()
{
	static const auto *re = new QRegularExpression(
		R"((duid\s*=\s*)([^\s,;]+))", QRegularExpression::CaseInsensitiveOption);
	return *re;
}
static const QRegularExpression &sanitize_session_id_eq_re()
{
	static const auto *re = new QRegularExpression(
		R"((session\s+id\s*=\s*)([^\s,;]+))", QRegularExpression::CaseInsensitiveOption);
	return *re;
}

static QString SanitizeLogMessage(const QString &msg)
{
	QString sanitized = msg;

	sanitized.replace(sanitize_ipv4_re(), QStringLiteral("<redacted-ipv4>"));
	sanitized.replace(sanitize_ipv6_re(), QStringLiteral("<redacted-ipv6>"));
	sanitized.replace(sanitize_labeled_secret_re(), QStringLiteral("\\1<redacted>"));
	sanitized.replace(sanitize_account_id_re(), QStringLiteral("\\1<redacted>"));
	sanitized.replace(sanitize_duid_re(), QStringLiteral("\\1<redacted>"));
	sanitized.replace(sanitize_session_id_eq_re(), QStringLiteral("\\1<redacted>"));
	sanitized.replace(sanitize_session_id_token_re(), QStringLiteral("\\1<redacted>"));
	sanitized.replace(sanitize_uuid_re(), QStringLiteral("<redacted-uuid>"));

	// Catch unlabeled long hex identifiers, which commonly occur in console IDs and device UIDs.
	sanitized.replace(sanitize_long_hex_re(), QStringLiteral("<redacted-hex>"));

	return sanitized;
}

#define KEEP_LOG_FILES_COUNT 5

QString GetLogBaseDir()
{
	auto base_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	if(base_dir.isEmpty())
		return QString();

	QDir dir(base_dir);
	if(!dir.mkpath("log"))
		return QString();
	if(!dir.cd("log"))
		return QString();

	return dir.absolutePath();
}

QString CreateLogFilename()
{
	static const QString date_format = QLatin1String(SESSION_LOG_FILE_DATE_FORMAT);
	static const QString date_format_legacy = QLatin1String(SESSION_LOG_FILE_DATE_FORMAT_LEGACY);
	static const QString session_log_wildcard = "chiaki_session_*.log";
	static const QRegularExpression session_log_regex("chiaki_session_(.*).log");

	QString dir_str = GetLogBaseDir();
	if(dir_str.isEmpty())
		return QString();
	QDir dir = QDir(dir_str);

	dir.setNameFilters({ session_log_wildcard });
	auto existing_files = dir.entryList();
	QVector<QPair<QString, QDateTime>> existing_files_date;
	existing_files_date.resize(existing_files.count());
	std::transform(existing_files.begin(), existing_files.end(), existing_files_date.begin(), [](const QString &filename) {
		QDateTime date;
		auto match = session_log_regex.match(filename);
		if(match.hasMatch())
		{
			date = QDateTime::fromString(match.captured(1), date_format);
			if(!date.isValid()) // files from before the format fix
				date = QDateTime::fromString(match.captured(1), date_format_legacy);
		}
		return QPair<QString, QDateTime>(filename, date);
	});
	std::sort(existing_files_date.begin(), existing_files_date.end(), [](const QPair<QString, QDateTime> &a, const QPair<QString, QDateTime> &b) {
		return a.second > b.second;
	});

	for(int i=KEEP_LOG_FILES_COUNT; i<existing_files_date.count(); i++)
	{
		const auto &pair = existing_files_date[i];
		if(!pair.second.isValid())
			break;
		dir.remove(pair.first);
	}

	QString filename = "chiaki_session_" + QDateTime::currentDateTime().toString(date_format) + ".log";
	return dir.absoluteFilePath(filename);
}
