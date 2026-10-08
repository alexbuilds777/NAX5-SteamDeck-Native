// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_STREAMSESSION_H
#define CHIAKI_STREAMSESSION_H

#include <chiaki/session.h>
#include <chiaki/nax5counters.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/opusencoder.h>
#include <chiaki/ffmpegdecoder.h>

#if CHIAKI_LIB_ENABLE_PI_DECODER
#include <chiaki/pidecoder.h>
#endif

#if CHIAKI_GUI_ENABLE_SETSU
#include <setsu.h>
#include <chiaki/orientation.h>
#endif

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
#include <sdeck.h>
#include <chiaki/orientation.h>
#endif

// Using Q_OS_MACOS instead of __APPLE__ doesn't work for the necessary enums to be included
#ifdef __APPLE__
#include <macMicPermission.h>
#endif

#include "exception.h"
#include "sessionlog.h"
#include "controllermanager.h"
#include "settings.h"
#include "nax5/nax5audiotrim.h"

#include <QObject>
#include <QImage>
#include <QMouseEvent>
#include <QMutex>
#include <QTimer>
#include <QQueue>
#include <QElapsedTimer>
#include <QThread>
#include <QWaitCondition>
#include <QAtomicInteger>
#if CHIAKI_GUI_ENABLE_SPEEX
#include <QQueue>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#endif

class QKeyEvent;
class Settings;

class ChiakiException: public Exception
{
	public:
		explicit ChiakiException(const QString &msg) : Exception(msg) {};
};

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
typedef struct haptic_packet_t
{
   	int16_t haptic_packet[30];
    uint64_t timestamp;
} haptic_packet_t;
#endif

	struct StreamSessionConnectInfo
	{
		Settings *settings;
		QMap<Qt::Key, int> key_map;
		Decoder decoder;
		QString hw_decoder;
		AVBufferRef *hw_device_ctx;
		QString audio_out_device;
		QString audio_in_device;
		uint32_t log_level_mask;
		bool log_sanitize = true;
		QString log_file;
		ChiakiTarget target;
		QString host;
		QString nickname;
		QByteArray regist_key;
		QByteArray morning;
		QString initial_login_pin;
		ChiakiConnectVideoProfile video_profile;
		double packet_loss_max;
		unsigned int audio_buffer_size;
		int audio_volume;
		bool fullscreen;
		bool zoom;
		bool stretch;
		bool enable_keyboard;
		bool keyboard_controller_enabled;
		bool mouse_touch_enabled;
		bool enable_dualsense;
		bool auto_regist;
		float haptic_override;
		ChiakiDisableAudioVideo audio_video_disabled;
		RumbleHapticsIntensity rumble_haptics_intensity;
		bool buttons_by_pos;
		bool enable_idr_on_fec_failure;
		bool start_mic_unmuted;
		bool port_guessing_enabled;
		int port_guess_count;
		int port_guess_socket_count;
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
		bool vertical_sdeck;
		bool enable_steamdeck_haptics;
#endif
#if CHIAKI_GUI_ENABLE_SPEEX
		bool speech_processing_enabled;
		int32_t noise_suppress_level;
		int32_t echo_suppress_level;
#endif
		QString duid;
		QString psn_token;
		QString psn_account_id;
		uint16_t dpad_touch_increment;
		uint dpad_touch_shortcut1;
		uint dpad_touch_shortcut2;
		uint dpad_touch_shortcut3;
		uint dpad_touch_shortcut4;

	StreamSessionConnectInfo() {}
	StreamSessionConnectInfo(
			Settings *settings,
			ChiakiTarget target,
			QString host,
			QString nickname,
			QByteArray regist_key,
			QByteArray morning,
			QString initial_login_pin,
			QString duid,
			bool auto_regist,
			bool fullscreen,
			bool zoom,
			bool stretch);
};

struct MicBuf
{
	int16_t *buf;
	uint32_t size_bytes;
	uint32_t current_byte;
};

// NAX5: cumulative GUI-side audio output statistics (StreamSession::GetNax5AudioStats)
struct Nax5AudioStats
{
	quint64 trim_dropped_bytes;  // dropped by the slow standing-latency trim
	quint64 cap_dropped_bytes;   // dropped by the hard latency cap
	quint32 queue_clears;        // SDL queue ran away (> 3 device buffers) and was cleared
	quint32 ring_overflows;      // ring buffer full, oldest audio dropped
	quint32 starved_polls;       // drain found the SDL queue empty while the ring had data
	quint32 reopens;             // InitAudio calls after the first
	int jitter_frames_last;      // audio receiver jitter buffer count, 0 if not available
};

class StreamSession : public QObject
{
	friend class StreamSessionPrivate;

	Q_OBJECT
	Q_PROPERTY(QString host READ GetHost CONSTANT)
	Q_PROPERTY(bool connected READ GetConnected NOTIFY ConnectedChanged)
	Q_PROPERTY(double measuredBitrate READ GetMeasuredBitrate NOTIFY MeasuredBitrateChanged)
	Q_PROPERTY(double averagePacketLoss READ GetAveragePacketLoss NOTIFY AveragePacketLossChanged)
	Q_PROPERTY(bool muted READ GetMuted WRITE SetMuted NOTIFY MutedChanged)
	Q_PROPERTY(bool cantDisplay READ GetCantDisplay NOTIFY CantDisplayChanged)
	Q_PROPERTY(int framesLost READ GetFramesLost NOTIFY FramesLostChanged)
	Q_PROPERTY(double targetBitrate READ GetTargetBitrate NOTIFY TargetBitrateChanged)
	Q_PROPERTY(int pingMs READ GetPingMs NOTIFY PingChanged)
	// NAX5: device battery for the stream overlay (-1 = unknown), charging flag
	Q_PROPERTY(int batteryPercent READ GetBatteryPercent NOTIFY BatteryChanged)
	Q_PROPERTY(bool batteryCharging READ GetBatteryCharging NOTIFY BatteryChanged)
	Q_PROPERTY(QString streamInfo READ GetStreamInfo CONSTANT)

	private:
		SessionLog log;
		ChiakiSession session;
		ChiakiOpusDecoder opus_decoder;
		ChiakiOpusEncoder opus_encoder;
		bool connected;
		bool muted;
		bool mic_connected;
		bool port_guessing_enabled;
		int port_guess_count;
		int port_guess_socket_count;
#ifdef Q_OS_MACOS
		bool mic_authorization;
#endif
		bool allow_unmute;
		int input_block;
		QString host;
		int audio_volume;
		double measured_bitrate = 0;
		double target_bitrate = 0;
		QString stream_info;
		double average_packet_loss = 0;
		int live_ping_ms = 0;
		int battery_percent = -1;
		bool battery_charging = false;
		unsigned overlay_status_ticks = 0;
		void UpdateOverlayStatus();
		int32_t frames_lost = 0;
		int32_t pending_frames_lost = 0;
		double packet_loss_max = 0;
		QList<double> packet_loss_history;
		QAtomicInteger<quint64> decoder_flush_generation{0};
		QAtomicInteger<int> fec_failures{0};
		// NAX5 input path counters (GetFeedbackSends / TakeInputLatencyMaxUs)
		QAtomicInteger<quint32> feedback_sends{0};
		QAtomicInteger<quint32> input_latency_max_us{0};
		bool cant_display = false;
		int haptics_handheld;
		float rumble_multiplier;
		int ps5_rumble_intensity;
		int ps5_trigger_intensity;
		uint8_t led_color[3];
		uint8_t player_index;
		QHash<int, Controller *> controllers;
#if CHIAKI_GUI_ENABLE_SETSU
		Setsu *setsu;
		QMap<QPair<QString, SetsuTrackingId>, uint8_t> setsu_ids;
		ChiakiControllerState setsu_state;
		SetsuDevice *setsu_motion_device;
		ChiakiOrientationTracker orient_tracker;
		ChiakiAccelNewZero setsu_accel_zero, setsu_real_accel;
		bool orient_dirty;
#endif

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
		SDeck *sdeck;
		ChiakiControllerState sdeck_state;
		QQueue<haptic_packet_t> sdeck_hapticl;
		QQueue<haptic_packet_t> sdeck_hapticr;
		int16_t * sdeck_haptics_senderl;
		int16_t * sdeck_haptics_senderr;
		int sdeck_queue_segment;
		uint64_t sdeck_last_haptic;
		bool sdeck_skipl, sdeck_skipr;
		bool enable_steamdeck_haptics;
		ChiakiOrientationTracker sdeck_orient_tracker;
		ChiakiAccelNewZero sdeck_accel_zero, sdeck_real_accel;
		bool sdeck_orient_dirty;
		bool vertical_sdeck;
#endif
		QQueue<uint16_t> rumble_haptics;
		bool rumble_haptics_connected;
		bool rumble_haptics_on;
		float PS_TOUCHPAD_MAX_X, PS_TOUCHPAD_MAX_Y;
		ChiakiControllerState keyboard_state;
		ChiakiControllerState touch_state;
		QMap<int, uint8_t> touch_tracker;
		int8_t mouse_touch_id;
		ChiakiControllerState dpad_touch_state;
		uint16_t dpad_touch_increment;
		float trigger_override;
		float haptic_override;
		bool keyboard_controller_enabled;
		bool mouse_touch_enabled;
		bool dpad_regular;
		bool dpad_regular_touch_switched;
		uint dpad_touch_shortcut1;
		uint dpad_touch_shortcut2;
		uint dpad_touch_shortcut3;
		uint dpad_touch_shortcut4;
		int8_t dpad_touch_id;
		QPair<uint16_t, uint16_t> dpad_touch_value;
		QTimer *dpad_touch_timer, *dpad_touch_stop_timer;
		QElapsedTimer double_tap_timer;
		RumbleHapticsIntensity rumble_haptics_intensity;
		bool start_mic_unmuted;
		bool session_started;

		ChiakiFfmpegDecoder *ffmpeg_decoder;
		void TriggerFfmpegFrameAvailable();
#if CHIAKI_LIB_ENABLE_PI_DECODER
		ChiakiPiDecoder *pi_decoder;
#endif

		QString audio_out_device_name;
		QString audio_in_device_name;
		SDL_AudioDeviceID audio_out;
		SDL_AudioDeviceID audio_in;
		size_t audio_out_sample_size;
		QMutex audio_out_ring_mutex;
		QByteArray audio_out_ring_buf;
		size_t audio_out_ring_read_pos = 0;
		size_t audio_out_ring_write_pos = 0;
		size_t audio_out_ring_fill = 0;
		bool audio_out_overflow_logged = false;
		// NAX5 audio latency cap (NAX5_AUDIO_MAX_MS), guarded by audio_out_ring_mutex
		unsigned int audio_out_rate = 0;
		uint64_t audio_out_over_cap_since_ms = 0;
		uint64_t audio_out_cap_dropped_bytes = 0;          // cumulative
		uint64_t audio_out_cap_dropped_bytes_reported = 0; // value at the last log line
		uint64_t audio_out_cap_last_log_ms = 0;
		// NAX5 slow trim of standing latency (NAX5_AUDIO_HEADROOM_MS), same mutex
		Nax5AudioTrim audio_out_trim;
		uint64_t audio_out_trim_dropped_bytes = 0;          // cumulative
		uint64_t audio_out_trim_dropped_bytes_reported = 0; // value at the last log line
		uint64_t audio_out_trim_last_log_ms = 0;
		// NAX5 cumulative audio counters for the stats CSV, same mutex.
		// Note: these (and the *_dropped_bytes above) are never reset when InitAudio reopens
		// the device, also not when the reopen changes channels / rate. Byte totals then mix
		// sample sizes, so converting a cumulative byte total to ms with the current format
		// is only exact for a session with one audio format; per-interval deltas are fine
		// except across the reopen itself.
		uint32_t audio_out_queue_clears = 0;
		uint32_t audio_out_queue_clears_reported = 0;
		uint64_t audio_out_queue_clear_last_log_ms = 0;
		uint32_t audio_out_ring_overflows = 0;
		uint32_t audio_out_starved_polls = 0;
		uint32_t audio_out_reopens = 0;
		bool audio_out_opened_once = false;
		void EnforceAudioOutCapLocked();
		void DropOldestAudioLocked(size_t drop);
		void TrimStandingAudioLocked(uint64_t now_ms, uint64_t queued);
		void HandleAudioQueueRunaway();
		QMutex audio_out_drain_mutex;
		QWaitCondition audio_out_drain_wait;
		QThread *audio_out_drain_thread = nullptr;
		bool audio_out_drain_thread_running = false;
		bool audio_out_drain_requested = false;
		size_t haptics_buffer_size;
		unsigned int audio_buffer_size;
		ChiakiHolepunchSession holepunch_session;
#if CHIAKI_GUI_ENABLE_SPEEX
		SpeexEchoState *echo_state;
		SpeexPreprocessState *preprocess_state;
		bool speech_processing_enabled;
		SDL_AudioCVT mic_speex_cvt;
		SDL_AudioCVT echo_speex_cvt;
		uint8_t *echo_resampler_buf, *mic_resampler_buf;
		QMutex echo_to_cancel_mutex;
		QQueue<QByteArray> echo_to_cancel;
#endif
		SDL_AudioDeviceID haptics_output;
		SDL_AudioCVT haptics_cvt;
		uint8_t *haptics_resampler_buf;
		MicBuf mic_buf;
		QMutex mic_ring_mutex;
		QByteArray mic_ring_buf;
		size_t mic_ring_read_pos = 0;
		size_t mic_ring_write_pos = 0;
		size_t mic_ring_fill = 0;
		bool mic_ring_drain_queued = false;
		bool mic_ring_overflow_logged = false;
		QAtomicInteger<bool> mic_active = true;
		QMap<Qt::Key, int> key_map;
		QElapsedTimer connect_timer;

		void StartAudioOutDrainThread();
		void StopAudioOutDrainThread();
		void AudioOutDrainThreadMain();
		void QueueAudioOutData(const QByteArray &audio_data);
		void DrainAudioOutRingBuffer();
		void PushAudioFrame(int16_t *buf, size_t samples_count);
		void PushHapticsFrame(uint8_t *buf, size_t buf_size);
		void CantDisplayMessage(bool cant_display);
		ChiakiErrorCode InitiatePsnConnection(QString psn_token);
#ifdef Q_OS_MACOS
		void SetMicAuthorization(Authorization authorization);
#endif
#if CHIAKI_GUI_ENABLE_SETSU
		void HandleSetsuEvent(SetsuEvent *event);
#endif
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
		void HandleSDeckEvent(SDeckEvent *event);
#endif
		void AdjustAdaptiveTriggerPacket(uint8_t *buf, uint8_t type);
		void WaitHaptics();

	private slots:
		void InitAudio(unsigned int channels, unsigned int rate);
		void InitMic(unsigned int channels, unsigned int rate);
		void InitHaptics();
		void Event(ChiakiEvent *event);
		void DisconnectHaptics();
		void ConnectHaptics();
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
		void SdeckQueueHaptics(haptic_packet_t packetl, haptic_packet_t packetr);
		void ConnectSdeckHaptics();
#endif
		void QueueRumbleHaptics(uint16_t strength);
		void ConnectRumbleHaptics();

	public:
		explicit StreamSession(const StreamSessionConnectInfo &connect_info, QObject *parent = nullptr);
		~StreamSession();

		bool IsConnected()	{ return connected; }
		bool IsConnecting()	{ return connect_timer.isValid(); }

		void Start();
		void Stop();
		void GoToBed();
	Q_INVOKABLE bool RequestIDR();
		void ToggleMute();
		void SetLoginPIN(const QString &pin);
		void GoHome();
		QString GetHost() { return host; }
		bool GetConnected() { return connected; }
		double GetMeasuredBitrate()	{ return measured_bitrate; }
		double GetAveragePacketLoss()	{ return average_packet_loss; }
		double GetTargetBitrate()	{ return target_bitrate; }
		// Measured once by Senkusha while connecting; 0 until then.
		// NAX5: live RTT from data-channel ACKs once sampled, else the Senkusha ping from connect time
		int GetPingMs() const	{ return connected ? (live_ping_ms > 0 ? live_ping_ms : static_cast<int>((session.rtt_us + 500) / 1000)) : 0; }
		int GetBatteryPercent() const { return battery_percent; }
		bool GetBatteryCharging() const { return battery_charging; }
		QString GetStreamInfo() const	{ return stream_info; }
		quint64 GetInitialRttUs() const { return session.rtt_us; }
		quint32 GetMtuIn() const { return session.mtu_in; }
		quint32 GetMtuOut() const { return session.mtu_out; }
		quint64 DecoderFlushGeneration() const { return decoder_flush_generation.loadRelaxed(); }
		int GetFramesLost()		{ return frames_lost; }
		// For the per-second stats log (QmlMainWindow::writeStreamStatsRow).
		int GetRequestedBitrateKbps() const	{ return session.connect_info.video_profile.bitrate; }
		int GetMaxFps() const	{ return session.connect_info.video_profile.max_fps; }
		QString GetRequestedResolution() const	{ return QStringLiteral("%1x%2").arg(session.connect_info.video_profile.width).arg(session.connect_info.video_profile.height); }
		double GetInstantPacketLoss() const	{ return session.stream_connection.congestion_control.packet_loss; }
		int GetFecFailures() const	{ return fec_failures.loadRelaxed(); }
		// NAX5 stats for the per-second CSV (QmlMainWindow). All thread-safe.
		ChiakiNax5Counters GetNax5Counters();            // snapshot of the lib counters
		Nax5AudioStats GetNax5AudioStats();              // cumulative GUI audio stats
		quint32 GetFeedbackSends()	{ return feedback_sends.loadRelaxed(); } // cumulative controller-state updates handed to the lib (not network packets)
		quint32 TakeInputLatencyMaxUs()	{ return input_latency_max_us.fetchAndStoreRelaxed(0); } // max (send - previous poll tick start), µs, since last call
		int GetHasBFrames();                             // from the decoder, -1 when unknown
		QString GetSessionIdSuffix() const	{ return log.GetSessionId(); } // "<NAX5_SESSION_ID>-<n>"
		bool GetMuted()	{ return muted; }
		// NAX5: audio waiting for playback (ring buffer + SDL queue, excluding the
		// device buffer itself), in milliseconds; 0 when audio is not running
		int GetAudioQueuedMs();
		void SetMuted(bool enable)	{ if (enable != muted) ToggleMute(); }
		void SetAudioVolume(int volume) { audio_volume = volume; }
		bool GetCantDisplay()	{ return cant_display; }
		ChiakiErrorCode ConnectPsnConnection(QString duid, bool ps5);
		void CancelPsnConnection(bool stop_thread);

		ChiakiLog *GetChiakiLog()				{ return log.GetChiakiLog(); }
		QList<Controller *> GetControllers()	{ return controllers.values(); }
		ChiakiFfmpegDecoder *GetFfmpegDecoder()	{ return ffmpeg_decoder; }
#if CHIAKI_LIB_ENABLE_PI_DECODER
		ChiakiPiDecoder *GetPiDecoder()	{ return pi_decoder; }
#endif
		void HandleKeyboardEvent(QKeyEvent *event);
		void HandleTouchEvent(QTouchEvent *event, qreal width, qreal height);
		void HandleDpadTouchEvent(ChiakiControllerState *state, bool placeholder = false);
		void HandleMouseReleaseEvent(QMouseEvent *event);
		void HandleMousePressEvent(QMouseEvent *event);
		void HandleMouseMoveEvent(QMouseEvent *event, qreal width, qreal height);
#if CHIAKI_GUI_ENABLE_SPEEX
		bool ProcessMicFrame(int16_t *echo_buf);
#endif
		void QueueMicData(const uint8_t *micdata, size_t micdata_size);
		void DrainMicRingBuffer();
		void ReadMic(const QByteArray &micdata);

		void BlockInput(bool block) { input_block = block ? 1 : 2; SendFeedbackState(); }

	signals:
		void FfmpegFrameAvailable();
		void RumbleHapticPushed(uint16_t strength);
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
		void SdeckHapticPushed(haptic_packet_t packetl, haptic_packet_t packetr);
#endif
		void DualSenseIntensityChanged(uint8_t intensity);
		void SessionQuit(ChiakiQuitReason reason, const QString &reason_str);
		void LoginPINRequested(bool incorrect);
		void DataHolepunchProgress(bool finished);
		void AutoRegistSucceeded(const ChiakiRegisteredHost &host);
		void NicknameReceived(QString nickname);
		void ConnectedChanged();
		void PingChanged();
		void BatteryChanged();
		void MeasuredBitrateChanged();
		void AveragePacketLossChanged();
		void FramesLostChanged();
		void TargetBitrateChanged();
		void MutedChanged();
		void CantDisplayChanged(bool cant_display);
		void FecFailure();

	private slots:
		void UpdateGamepads();
		void DpadSendFeedbackState();
		void SendFeedbackState();
};

Q_DECLARE_METATYPE(ChiakiQuitReason)
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
Q_DECLARE_METATYPE(haptic_packet_t)
#endif

#endif // CHIAKI_STREAMSESSION_H
