// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <streamsession.h>
#include "nax5/session/nax5sessionlifecycle.h"
#include <settings.h>
#include <controllermanager.h>

#include <chiaki/base64.h>
#include <chiaki/streamconnection.h>
#include <chiaki/remote/holepunch.h>
#include <chiaki/session.h>
#include <chiaki/time.h>
#include "../../lib/src/utils.h"

#include <QKeyEvent>
#include <QMutexLocker>
#include <QtMath>
#include <QFile>
#include <QTimer>
#include <atomic>

#include <algorithm>

#include <cstring>

#define SETSU_UPDATE_INTERVAL_MS 4
#define STEAMDECK_UPDATE_INTERVAL_MS 4
#define STEAMDECK_HAPTIC_INTERVAL_MS 10 // check every interval
#define NEW_DPAD_TOUCH_INTERVAL_MS 500
#define DPAD_TOUCH_UPDATE_INTERVAL_MS 10
#define STEAMDECK_HAPTIC_PACKETS_PER_ANALYSIS 4 // send packets every interval * packets per analysis
#define RUMBLE_HAPTICS_PACKETS_PER_RUMBLE 3
#define STEAMDECK_HAPTIC_SAMPLING_RATE 3000
// DualShock4 touchpad is 1920 x 942
#define PS4_TOUCHPAD_MAX_X 1920.0f
#define PS4_TOUCHPAD_MAX_Y 942.0f
// DualSense touchpad is 1919 x 1079
#define PS5_TOUCHPAD_MAX_X 1919.0f
#define PS5_TOUCHPAD_MAX_Y 1079.0f
#define SESSION_RETRY_SECONDS 20
#define HAPTIC_RUMBLE_MIN_STRENGTH 100

#define MICROPHONE_SAMPLES 480
#ifdef Q_OS_LINUX
#define DUALSENSE_AUDIO_DEVICE_NEEDLE "DualSense"
#else
#define DUALSENSE_AUDIO_DEVICE_NEEDLE "Wireless Controller"
#endif
#if CHIAKI_GUI_ENABLE_SPEEX
#define ECHO_QUEUE_MAX 40
#endif

// NAX5 audio latency cap: once audio waiting for playback (ring buffer + SDL queue)
// stays above the cap for AUDIO_CAP_SUSTAIN_MS, the oldest queued audio is dropped so
// that audio lag caused by a network stall recovers instead of persisting forever.
//   NAX5_AUDIO_MAX_MS=<ms>  cap in milliseconds (default 150)
//   NAX5_AUDIO_MAX_MS=0     no cap, only the ring overflow trims (upstream behaviour)
// Normal operation keeps ~100-110 ms queued (SDL top-up + ring), so leave headroom.
#define AUDIO_CAP_DEFAULT_MS 150
#define AUDIO_CAP_SUSTAIN_MS 500
#define AUDIO_CAP_LOG_INTERVAL_MS 10000

// NAX5 slow trim (see nax5audiotrim.h): drops audio that stayed queued for a whole
// window, down to the headroom, in small steps. Handles the standing latency the hard
// cap above misses because the queue saw-tooths across it.
//   NAX5_AUDIO_HEADROOM_MS=<ms>  standing minimum to keep (default 70)
//   NAX5_AUDIO_HEADROOM_MS=0     slow trim off (also off when NAX5_AUDIO_MAX_MS=0)
// The minimum is sampled when a packet arrives, i.e. at the queue's peak, and the device
// then pulls 10-50 ms at once: with 40 ms the queue ran dry a minute after some trims.
#define AUDIO_TRIM_HEADROOM_DEFAULT_MS 70
#define AUDIO_TRIM_WINDOW_MS 3000
#define AUDIO_TRIM_HYSTERESIS_MS 20
#define AUDIO_TRIM_STEP_MS 40
// Crossfade over the cut of every trim/cap drop, so it is not heard as a click
#define AUDIO_SPLICE_FADE_MS 10
#define AUDIO_RING_MIN_MS 500
// Requested device buffer (settings, bytes) is rounded down to whole sample frames
// and clamped to this range before it is handed to SDL.
#define AUDIO_BUFFER_MIN_BYTES 960
#define AUDIO_BUFFER_MAX_BYTES 48000

static unsigned int nax5_audio_max_ms()
{
	static const unsigned int value = []() -> unsigned int {
		const QByteArray v = qgetenv("NAX5_AUDIO_MAX_MS").trimmed();
		if(v.isEmpty())
			return AUDIO_CAP_DEFAULT_MS;
		bool ok = false;
		const unsigned int ms = v.toUInt(&ok);
		if(!ok || ms > 5000)
			return AUDIO_CAP_DEFAULT_MS;
		return ms;
	}();
	return value;
}

static unsigned int nax5_audio_headroom_ms()
{
	static const unsigned int value = []() -> unsigned int {
		const QByteArray v = qgetenv("NAX5_AUDIO_HEADROOM_MS").trimmed();
		if(v.isEmpty())
			return AUDIO_TRIM_HEADROOM_DEFAULT_MS;
		bool ok = false;
		const unsigned int ms = v.toUInt(&ok);
		if(!ok || ms > 1000)
			return AUDIO_TRIM_HEADROOM_DEFAULT_MS;
		return ms;
	}();
	return value;
}

static uint64_t nax5_audio_bytes_for_ms(uint64_t ms, unsigned int rate, size_t frame_size)
{
	return (uint64_t)rate * frame_size * ms / 1000;
}

static bool isLocalAddress(QString host)
{
    if(host.contains("."))
    {
        if(host.startsWith("10."))
            return true;
        else if(host.startsWith("192.168."))
            return true;
        for (int j = 16; j < 32; j++)
        {
            if(host.startsWith(QString("172.") + QString::number(j) + QString(".")))
                return true;
        }
    }
    else if(host.contains(":"))
    {
        if(host.startsWith("FC", Qt::CaseInsensitive))
            return true;
        if(host.startsWith("FD", Qt::CaseInsensitive))
            return true;
    }
    return false;
}

StreamSessionConnectInfo::StreamSessionConnectInfo(
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
		bool stretch)
	: settings(settings)
{
	key_map = settings->GetControllerMappingForDecoding();
	decoder = settings->GetDecoder();
	hw_decoder = settings->GetHardwareDecoder();
	hw_device_ctx = nullptr;
	audio_out_device = settings->GetAudioOutDevice();
	audio_in_device = settings->GetAudioInDevice();
	log_level_mask = settings->GetLogLevelMask();
	log_sanitize = settings->GetLogSanitize();
	audio_volume = settings->GetAudioVolume();
	log_file = CreateLogFilename();
	// local connection
	if(duid.isEmpty() && isLocalAddress(host))
		video_profile = chiaki_target_is_ps5(target) ? settings->GetVideoProfileLocalPS5(): settings->GetVideoProfileLocalPS4();
	// remote connection
	else
		video_profile = chiaki_target_is_ps5(target) ? settings->GetVideoProfileRemotePS5(): settings->GetVideoProfileRemotePS4();
	this->target = target;
	this->nickname = std::move(nickname);
	this->host = std::move(host);
	this->regist_key = std::move(regist_key);
	this->morning = std::move(morning);
	this->initial_login_pin = std::move(initial_login_pin);
	audio_buffer_size = settings->GetAudioBufferSize();
	this->fullscreen = fullscreen;
	this->zoom = zoom;
	this->stretch = stretch;
	const bool keyboard_controller_enabled_setting = settings->GetKeyboardEnabled();
	this->keyboard_controller_enabled = keyboard_controller_enabled_setting;
	this->mouse_touch_enabled = settings->GetMouseTouchEnabled();
	this->enable_keyboard = false;
	this->enable_dualsense = true;
	this->enable_idr_on_fec_failure = settings->GetIDROnFECFailureEnabled();
	this->rumble_haptics_intensity = settings->GetRumbleHapticsIntensity();
	this->buttons_by_pos = settings->GetButtonsByPosition();
	this->start_mic_unmuted = settings->GetStartMicUnmuted();
	this->port_guessing_enabled = settings->GetPortGuessingEnabled();
	this->port_guess_count = settings->GetPortGuessCount();
	this->port_guess_socket_count = settings->GetPortGuessSocketCount();
	this->packet_loss_max = settings->GetPacketLossReportedMax();
	this->audio_video_disabled = settings->GetAudioVideoDisabled();
	this->haptic_override = settings->GetHapticOverride();
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	this->enable_steamdeck_haptics = settings->GetSteamDeckHapticsEnabled();
	this->vertical_sdeck = settings->GetVerticalDeckEnabled();
#endif
#if CHIAKI_GUI_ENABLE_SPEEX
	this->speech_processing_enabled = settings->GetSpeechProcessingEnabled();
	this->noise_suppress_level = settings->GetNoiseSuppressLevel();
	this->echo_suppress_level = settings->GetEchoSuppressLevel();
#endif
	this->psn_token = settings->GetPsnAuthToken();
	this->psn_account_id = settings->GetPsnAccountId();
	this->duid = std::move(duid);
	this->auto_regist = auto_regist;
	this->dpad_touch_increment = settings->GetDpadTouchEnabled() ? settings->GetDpadTouchIncrement(): 0;
	this->dpad_touch_shortcut1 = settings->GetDpadTouchShortcut1();
	if(this->dpad_touch_shortcut1 > 0)
		this->dpad_touch_shortcut1 = 1 << (this->dpad_touch_shortcut1 - 1);
	this->dpad_touch_shortcut2 = settings->GetDpadTouchShortcut2();
	if(this->dpad_touch_shortcut2 > 0)
		this->dpad_touch_shortcut2 = 1 << (this->dpad_touch_shortcut2 - 1);
	this->dpad_touch_shortcut3 = settings->GetDpadTouchShortcut3();
	if(this->dpad_touch_shortcut3 > 0)
		this->dpad_touch_shortcut3 = 1 << (this->dpad_touch_shortcut3 - 1);
	this->dpad_touch_shortcut4 = settings->GetDpadTouchShortcut4();
	if(this->dpad_touch_shortcut4 > 0)
		this->dpad_touch_shortcut4 = 1 << (this->dpad_touch_shortcut4 - 1);
}

static void AudioSettingsCb(uint32_t channels, uint32_t rate, void *user);
static void AudioFrameCb(int16_t *buf, size_t samples_count, void *user);
static void HapticsFrameCb(uint8_t *buf, size_t buf_size, void *user);
#ifdef Q_OS_MACOS
static void MacMicRequestCb(Authorization authorization, void *user);
#endif
static void CantDisplayCb(void *user, bool cant_display);
static void EventCb(ChiakiEvent *event, void *user);
#if CHIAKI_GUI_ENABLE_SETSU
static void SessionSetsuCb(SetsuEvent *event, void *user);
#endif
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
static void SessionSDeckCb(SDeckEvent *event, void *user);
#endif
static void FfmpegFrameCb(ChiakiFfmpegDecoder *decoder, void *user);

StreamSession::StreamSession(const StreamSessionConnectInfo &connect_info, QObject *parent)
	: QObject(parent),
		log(this, connect_info.log_level_mask, connect_info.log_file, connect_info.log_sanitize),
	ffmpeg_decoder(nullptr),
#if CHIAKI_LIB_ENABLE_PI_DECODER
	pi_decoder(nullptr),
#endif
	audio_out(0),
	audio_in(0),
	haptics_output(0),
	haptics_handheld(0),
	session_started(false),
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	sdeck_haptics_senderl(nullptr),
	sdeck_haptics_senderr(nullptr),
	sdeck(nullptr),
#endif
#if CHIAKI_GUI_ENABLE_SPEEX
		echo_resampler_buf(nullptr),
		mic_resampler_buf(nullptr),
#endif
	haptics_resampler_buf(nullptr),
	holepunch_session(nullptr),
	rumble_multiplier(1),
	ps5_rumble_intensity(0x00),
	ps5_trigger_intensity(0x00),
	rumble_haptics_connected(false),
	rumble_haptics_on(false)
{
	mic_buf.buf = nullptr;
	connected = false;
	muted = true;
	mic_connected = false;
	port_guessing_enabled = connect_info.port_guessing_enabled;
	port_guess_count = connect_info.port_guess_count;
	port_guess_socket_count = connect_info.port_guess_socket_count;
#ifdef Q_OS_MACOS
	mic_authorization = false;
#endif
	allow_unmute = false;
	dpad_regular = true;
	dpad_regular_touch_switched = false;
	rumble_haptics_intensity = RumbleHapticsIntensity::Off;
	input_block = 0;
	player_index = 0;
	memset(led_color, 0, sizeof(led_color));
	packet_loss_max = connect_info.packet_loss_max;
	ChiakiErrorCode err;
#if CHIAKI_LIB_ENABLE_PI_DECODER
	if(connect_info.decoder == Decoder::Pi)
	{
		pi_decoder = CHIAKI_NEW(ChiakiPiDecoder);
		if(chiaki_pi_decoder_init(pi_decoder, log.GetChiakiLog()) != CHIAKI_ERR_SUCCESS)
			throw ChiakiException("Failed to initialize Raspberry Pi Decoder");
	}
	else
	{
#endif
		ffmpeg_decoder = new ChiakiFfmpegDecoder;
		ChiakiLogSniffer sniffer;
		chiaki_log_sniffer_init(&sniffer, CHIAKI_LOG_ALL, GetChiakiLog());
		err = chiaki_ffmpeg_decoder_init(ffmpeg_decoder,
				chiaki_log_sniffer_get_log(&sniffer),
				chiaki_target_is_ps5(connect_info.target) ? connect_info.video_profile.codec : CHIAKI_CODEC_H264,
				connect_info.video_profile.max_fps,
				connect_info.hw_decoder.isEmpty() ? NULL : connect_info.hw_decoder.toUtf8().constData(),
				connect_info.hw_device_ctx, FfmpegFrameCb, this);
		if(err != CHIAKI_ERR_SUCCESS)
		{
			QString log = QString::fromUtf8(chiaki_log_sniffer_get_buffer(&sniffer));
			chiaki_log_sniffer_fini(&sniffer);
			throw ChiakiException("Failed to initialize FFMPEG Decoder:\n" + log);
		}
		chiaki_log_sniffer_fini(&sniffer);
		ffmpeg_decoder->log = GetChiakiLog();
#if CHIAKI_LIB_ENABLE_PI_DECODER
	}
#endif
	audio_volume = connect_info.audio_volume;
	start_mic_unmuted = connect_info.start_mic_unmuted;
	audio_out_device_name = connect_info.audio_out_device;
	audio_in_device_name = connect_info.audio_in_device;

	chiaki_opus_decoder_init(&opus_decoder, log.GetChiakiLog());
	chiaki_opus_encoder_init(&opus_encoder, log.GetChiakiLog());
#if CHIAKI_GUI_ENABLE_SPEEX
	speech_processing_enabled = connect_info.speech_processing_enabled;
	if(speech_processing_enabled)
	{
		echo_state = speex_echo_state_init(MICROPHONE_SAMPLES, MICROPHONE_SAMPLES * 10);
		preprocess_state = speex_preprocess_state_init(MICROPHONE_SAMPLES, MICROPHONE_SAMPLES * 100);
		int32_t noise_suppress_level = -1 * connect_info.noise_suppress_level;
		int32_t echo_suppress_level = -1 * connect_info.echo_suppress_level;
		speex_preprocess_ctl(preprocess_state, SPEEX_PREPROCESS_SET_ECHO_STATE, echo_state);
		speex_preprocess_ctl(preprocess_state, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noise_suppress_level);
		speex_preprocess_ctl(preprocess_state, SPEEX_PREPROCESS_GET_NOISE_SUPPRESS, &noise_suppress_level);
		CHIAKI_LOGI(GetChiakiLog(), "Noise suppress level is %i dB", noise_suppress_level);
		speex_preprocess_ctl(preprocess_state, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &echo_suppress_level);
		speex_preprocess_ctl(preprocess_state, SPEEX_PREPROCESS_GET_ECHO_SUPPRESS, &echo_suppress_level);
		CHIAKI_LOGI(GetChiakiLog(), "Echo suppress level is %i dB", echo_suppress_level);
		CHIAKI_LOGI(GetChiakiLog(), "Started microphone echo cancellation and noise suppression");
	}
#endif
	audio_buffer_size = connect_info.audio_buffer_size;
	mouse_touch_enabled = connect_info.mouse_touch_enabled;
	keyboard_controller_enabled = connect_info.keyboard_controller_enabled;
	host = connect_info.host;
	QByteArray host_str = connect_info.host.toUtf8();

	ChiakiConnectInfo chiaki_connect_info = {};
	chiaki_connect_info.ps5 = chiaki_target_is_ps5(connect_info.target);
	chiaki_connect_info.host = host_str.constData();
	chiaki_connect_info.video_profile = connect_info.video_profile;
	chiaki_connect_info.video_profile_auto_downgrade = true;
	chiaki_connect_info.enable_keyboard = false;
	chiaki_connect_info.enable_idr_on_fec_failure = connect_info.enable_idr_on_fec_failure;
	chiaki_connect_info.enable_dualsense = connect_info.enable_dualsense;
	chiaki_connect_info.packet_loss_max = connect_info.packet_loss_max;
	chiaki_connect_info.auto_regist = connect_info.auto_regist;
	chiaki_connect_info.audio_video_disabled = connect_info.audio_video_disabled;

	dpad_touch_shortcut1 = connect_info.dpad_touch_shortcut1;
	dpad_touch_shortcut2 = connect_info.dpad_touch_shortcut2;
	dpad_touch_shortcut3 = connect_info.dpad_touch_shortcut3;
	dpad_touch_shortcut4 = connect_info.dpad_touch_shortcut4;
	haptic_override = connect_info.haptic_override;
#if CHIAKI_LIB_ENABLE_PI_DECODER
	if(connect_info.decoder == Decoder::Pi && chiaki_connect_info.video_profile.codec != CHIAKI_CODEC_H264)
	{
		CHIAKI_LOGW(GetChiakiLog(), "A codec other than H264 was requested for Pi Decoder. Falling back to it.");
		chiaki_connect_info.video_profile.codec = CHIAKI_CODEC_H264;
	}
#endif
	{
		const ChiakiCodec codec = chiaki_target_is_ps5(connect_info.target) ? chiaki_connect_info.video_profile.codec : CHIAKI_CODEC_H264;
		const char *codec_name = codec == CHIAKI_CODEC_H265_HDR ? "HEVC HDR" : codec == CHIAKI_CODEC_H265 ? "HEVC" : "H264";
		const QString decoder_name = connect_info.hw_decoder.isEmpty() ? QStringLiteral("software") : connect_info.hw_decoder;
		stream_info = QStringLiteral("%1 · %2 · запрошено %3 Мбит/с")
			.arg(QLatin1String(codec_name), decoder_name)
			.arg(chiaki_connect_info.video_profile.bitrate / 1000.0, 0, 'f', 0);
	}
	if(connect_info.duid.isEmpty())
	{
		if(connect_info.regist_key.size() != sizeof(chiaki_connect_info.regist_key))
			throw ChiakiException("RegistKey invalid");
		memcpy(chiaki_connect_info.regist_key, connect_info.regist_key.constData(), sizeof(chiaki_connect_info.regist_key));

		if(connect_info.morning.size() != sizeof(chiaki_connect_info.morning))
			throw ChiakiException("Morning invalid");
		memcpy(chiaki_connect_info.morning, connect_info.morning.constData(), sizeof(chiaki_connect_info.morning));
	}

	if(chiaki_connect_info.ps5)
	{
		PS_TOUCHPAD_MAX_X = PS5_TOUCHPAD_MAX_X;
		PS_TOUCHPAD_MAX_Y = PS5_TOUCHPAD_MAX_Y;
	}
	else
	{
		PS_TOUCHPAD_MAX_X = PS4_TOUCHPAD_MAX_X;
		PS_TOUCHPAD_MAX_Y = PS4_TOUCHPAD_MAX_Y;
	}

	chiaki_controller_state_set_idle(&keyboard_state);
	chiaki_controller_state_set_idle(&touch_state);
	touch_tracker=QMap<int, uint8_t>();
	mouse_touch_id=-1;
	dpad_touch_id =-1;
	chiaki_controller_state_set_idle(&dpad_touch_state);
	dpad_touch_value = QPair<uint16_t, uint16_t>(0,0);
	dpad_touch_increment = connect_info.dpad_touch_increment;
	dpad_touch_timer = new QTimer(this);
	connect(dpad_touch_timer, &QTimer::timeout, this, &StreamSession::DpadSendFeedbackState);
	dpad_touch_timer->setInterval(DPAD_TOUCH_UPDATE_INTERVAL_MS);
	dpad_touch_stop_timer = new QTimer(this);
	dpad_touch_stop_timer->setSingleShot(true);
	connect(dpad_touch_stop_timer, &QTimer::timeout, this, [this]{
		if(dpad_touch_id >= 0)
		{
			dpad_touch_timer->stop();
			chiaki_controller_state_stop_touch(&dpad_touch_state, (uint8_t)dpad_touch_id);
			dpad_touch_id = -1;
			SendFeedbackState();
		}
	});
	// If duid isn't empty connect with psn
	chiaki_connect_info.holepunch_session = NULL;
	if(!connect_info.duid.isEmpty())
	{
		err = InitiatePsnConnection(connect_info.psn_token);
		if (err != CHIAKI_ERR_SUCCESS)
			throw ChiakiException("Psn Connection Failed " + QString::fromLocal8Bit(chiaki_error_string(err)));
		chiaki_connect_info.holepunch_session = holepunch_session;
        QByteArray psn_account_id = QByteArray::fromBase64(connect_info.psn_account_id.toUtf8());
        if (psn_account_id.size() != CHIAKI_PSN_ACCOUNT_ID_SIZE) {
            throw ChiakiException((tr("Invalid Account-ID"), tr("The PSN Account-ID must be exactly %1 bytes encoded as base64.")).arg(CHIAKI_PSN_ACCOUNT_ID_SIZE));
        }
        memcpy(chiaki_connect_info.psn_account_id, psn_account_id.constData(), CHIAKI_PSN_ACCOUNT_ID_SIZE);
	}
	err = chiaki_session_init(&session, &chiaki_connect_info, GetChiakiLog());
	if(err != CHIAKI_ERR_SUCCESS)
		throw ChiakiException("Chiaki Session Init failed: " + QString::fromLocal8Bit(chiaki_error_string(err)));
	ChiakiCtrlDisplaySink display_sink;
	display_sink.user = this;
	display_sink.cantdisplay_cb = CantDisplayCb;
	chiaki_session_ctrl_set_display_sink(&session, &display_sink);
	chiaki_opus_decoder_set_cb(&opus_decoder, AudioSettingsCb, AudioFrameCb, this);
	ChiakiAudioSink audio_sink;
	chiaki_opus_decoder_get_sink(&opus_decoder, &audio_sink);
	chiaki_session_set_audio_sink(&session, &audio_sink);
	ChiakiAudioHeader audio_header;
	chiaki_audio_header_set(&audio_header, 2, 16, MICROPHONE_SAMPLES * 100, MICROPHONE_SAMPLES);
	chiaki_opus_encoder_header(&audio_header, &opus_encoder, &session);

	if (connect_info.enable_dualsense)
	{
		ChiakiAudioSink haptics_sink;
		haptics_sink.user = this;
		haptics_sink.frame_cb = HapticsFrameCb;
		chiaki_session_set_haptics_sink(&session, &haptics_sink);
	}
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	if (connect_info.enable_steamdeck_haptics)
		enable_steamdeck_haptics = true;
	else
		enable_steamdeck_haptics = false;
#endif
#if CHIAKI_LIB_ENABLE_PI_DECODER
	if(pi_decoder)
		chiaki_session_set_video_sample_cb(&session, chiaki_pi_decoder_video_sample_cb, pi_decoder);
	else
	{
#endif
		chiaki_session_set_video_sample_cb(&session, chiaki_ffmpeg_decoder_video_sample_cb, ffmpeg_decoder);
#if CHIAKI_LIB_ENABLE_PI_DECODER
	}
#endif

	chiaki_session_set_event_cb(&session, EventCb, this);

#if CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER
	connect(ControllerManager::GetInstance(), &ControllerManager::AvailableControllersUpdated, this, &StreamSession::UpdateGamepads);
	connect(this, &StreamSession::DualSenseIntensityChanged, ControllerManager::GetInstance(), &ControllerManager::SetDualSenseIntensity);
	if(connect_info.buttons_by_pos)
		ControllerManager::GetInstance()->SetButtonsByPos();
#endif
#if CHIAKI_GUI_ENABLE_SETSU
	setsu_motion_device = nullptr;
	chiaki_controller_state_set_idle(&setsu_state);
	chiaki_accel_new_zero_set_inactive(&setsu_accel_zero, false);
	chiaki_accel_new_zero_set_inactive(&setsu_real_accel, true);
	setsu_ids=QMap<QPair<QString, SetsuTrackingId>, uint8_t>();
	orient_dirty = true;
	chiaki_orientation_tracker_init(&orient_tracker);
	setsu = setsu_new();
	auto timer = new QTimer(this);
	connect(timer, &QTimer::timeout, this, [this]{
		setsu_poll(setsu, SessionSetsuCb, this);
		if(orient_dirty)
		{
			chiaki_orientation_tracker_apply_to_controller_state(&orient_tracker, &setsu_state);
			SendFeedbackState();
			orient_dirty = false;
		}
	});
	timer->start(SETSU_UPDATE_INTERVAL_MS);
#endif

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	chiaki_controller_state_set_idle(&sdeck_state);
	sdeck = sdeck_new();
	// no concept of hotplug for Steam Deck so turn off if not detected immediately
	if(!sdeck)
		CHIAKI_LOGI(GetChiakiLog(), "Steam Deck not found ... Steam Deck native features disabled\n\n");
	else
	{
		CHIAKI_LOGI(GetChiakiLog(), "Connected Steam Deck ... gyro online\n\n");
		vertical_sdeck = connect_info.vertical_sdeck;
		chiaki_accel_new_zero_set_inactive(&sdeck_accel_zero, false);
		chiaki_accel_new_zero_set_inactive(&sdeck_real_accel, true);
		chiaki_orientation_tracker_init(&sdeck_orient_tracker);
		sdeck_orient_dirty = true;

		auto sdeck_timer = new QTimer(this);
		connect(sdeck_timer, &QTimer::timeout, this, [this]{
			sdeck_read(sdeck, SessionSDeckCb, this);
			if(sdeck_orient_dirty)
			{
				chiaki_orientation_tracker_apply_to_controller_state(&sdeck_orient_tracker, &sdeck_state);
				SendFeedbackState();
				sdeck_orient_dirty = false;
			}
		});
		sdeck_timer->start(STEAMDECK_UPDATE_INTERVAL_MS);
	}
#endif
	key_map = connect_info.key_map;
	if (connect_info.enable_dualsense)
	{
		InitHaptics();
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
		// Connect Steam Deck haptics with a delay to give other potential haptics time to set up
		if(sdeck)
		{
			haptics_handheld++;
			QTimer::singleShot(1100, this, &StreamSession::ConnectSdeckHaptics);
		}
#endif
		rumble_haptics_intensity = connect_info.rumble_haptics_intensity;
		ConnectRumbleHaptics();
	}
	UpdateGamepads();
	QTimer *packet_loss_timer = new QTimer(this);
	packet_loss_timer->setInterval(200);
	packet_loss_timer->start();
	connect(packet_loss_timer, &QTimer::timeout, this, [this]() {
		if(packet_loss_history.size() > 10)
			packet_loss_history.takeFirst();
		packet_loss_history.append(session.stream_connection.congestion_control.packet_loss);
		double packet_loss = 0;
		for(auto v : std::as_const(packet_loss_history))
			packet_loss += v / packet_loss_history.size();
		if(packet_loss != average_packet_loss)
		{
			average_packet_loss = packet_loss;
			emit AveragePacketLossChanged();
		}
		if(session.stream_connection.video_receiver)
		{
			int32_t total = chiaki_video_receiver_get_frames_lost_total(session.stream_connection.video_receiver);
			if(total > pending_frames_lost)
			{
				frames_lost += total - pending_frames_lost;
				emit FramesLostChanged();
			}
			pending_frames_lost = total;
		}
	});

	// NAX5: overlay status tick: live ping every 1 s, battery every 10 s
	QTimer *overlay_status_timer = new QTimer(this);
	overlay_status_timer->setInterval(1000);
	connect(overlay_status_timer, &QTimer::timeout, this, &StreamSession::UpdateOverlayStatus);
	overlay_status_timer->start();
	UpdateOverlayStatus();

	StartAudioOutDrainThread();
}

void StreamSession::UpdateOverlayStatus()
{
	ChiakiNax5Counters snap;
	chiaki_session_get_nax5_counters(&session, &snap);
	const int ping = snap.data_rtt_samples > 0 ? static_cast<int>((snap.data_rtt_us_smoothed + 500) / 1000) : 0;
	if(ping != live_ping_ms)
	{
		live_ping_ms = ping;
		emit PingChanged();
	}

	// Buffered session log: write out lines that have been sitting in memory for >= 1 s so a
	// quiet session (no new line to trigger the flush) still reaches the file / live tail.
	log.FlushIfStale();

	// Battery changes slowly: read sysfs every 10th tick (10 s); the ping above is free (heartbeat ACKs).
	if(overlay_status_ticks++ % 10 != 0)
		return;
	int pct = -1;
	bool charging = false;
	QFile cap(QStringLiteral("/sys/class/power_supply/BAT1/capacity"));
	if(cap.open(QIODevice::ReadOnly))
	{
		bool ok = false;
		const int v = QString::fromLatin1(cap.readAll()).trimmed().toInt(&ok);
		if(ok)
			pct = qBound(0, v, 100);
	}
	QFile status(QStringLiteral("/sys/class/power_supply/BAT1/status"));
	if(status.open(QIODevice::ReadOnly))
	{
		const QByteArray st = status.readAll().trimmed();
		charging = st == "Charging" || st == "Full";
	}
	if(pct != battery_percent || charging != battery_charging)
	{
		battery_percent = pct;
		battery_charging = charging;
		emit BatteryChanged();
	}
}

StreamSession::~StreamSession()
{
	mic_active.storeRelaxed(false);
	StopAudioOutDrainThread();
	// Best effort only: audio_out is cleared under audio_out_ring_mutex before the device
	// is closed, so the ring-locked paths (cap/trim, runaway re-check, GetAudioQueuedMs)
	// stop using it. PushAudioFrame / ReadMic on the Takion thread read audio_out /
	// audio_in without a lock and can still pass the old id until the session threads are
	// joined further below; SDL rejects a closed device id (queue calls fail or return 0).
	if(audio_out)
	{
		SDL_AudioDeviceID dev = audio_out;
		{
			QMutexLocker locker(&audio_out_ring_mutex);
			audio_out = 0;
		}
		SDL_CloseAudioDevice(dev);
	}
	{
		QMutexLocker locker(&audio_out_ring_mutex);
		audio_out_ring_buf.clear();
		audio_out_ring_read_pos = 0;
		audio_out_ring_write_pos = 0;
		audio_out_ring_fill = 0;
		audio_out_overflow_logged = false;
	}
	if(audio_in)
	{
		SDL_AudioDeviceID dev = audio_in;
		audio_in = 0;
		SDL_CloseAudioDevice(dev);
	}

	// Prepare log for shutdown BEFORE joining session threads
	// This prevents crashes from log callbacks during shutdown
	log.PrepareShutdown();

	if(session_started)
		chiaki_session_join(&session);
	chiaki_session_fini(&session);
	chiaki_opus_decoder_fini(&opus_decoder);
	chiaki_opus_encoder_fini(&opus_encoder);
#if CHIAKI_GUI_ENABLE_SPEEX
	if(speech_processing_enabled)
	{
		speex_echo_state_destroy(echo_state);
		speex_preprocess_state_destroy(preprocess_state);
	}
#endif
#if CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER
	QMetaObject::invokeMethod(this, [this]() {
		for(auto controller : controllers)
		{
			const uint8_t clear_effect[10] = { 0 };
			controller->SetTriggerEffects(0x05, clear_effect, 0x05, clear_effect);
			controller->SetRumble(0,0);
			controller->Unref();
		}
	});
#endif
#if CHIAKI_GUI_ENABLE_SETSU
	setsu_free(setsu);
#endif
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	sdeck_free(sdeck);
#endif
#if CHIAKI_LIB_ENABLE_PI_DECODER
	if(pi_decoder)
	{
		chiaki_pi_decoder_fini(pi_decoder);
		free(pi_decoder);
	}
#endif
	if(ffmpeg_decoder)
	{
		chiaki_ffmpeg_decoder_fini(ffmpeg_decoder);
		delete ffmpeg_decoder;
	}
	if (haptics_output > 0)
	{
		SDL_CloseAudioDevice(haptics_output);
		haptics_output = 0;
	}
	if (haptics_resampler_buf)
	{
		free(haptics_resampler_buf);
		haptics_resampler_buf = nullptr;
	}
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	if (sdeck_haptics_senderl)
	{
		free(sdeck_haptics_senderl);
		sdeck_haptics_senderl = nullptr;
	}
	if (sdeck_haptics_senderr)
	{
		free(sdeck_haptics_senderr);
		sdeck_haptics_senderr = nullptr;
	}
#endif
	if(mic_buf.buf)
	{
		free(mic_buf.buf);
		mic_buf.buf = nullptr;
	}
#if CHIAKI_GUI_ENABLE_SPEEX
	if(speech_processing_enabled)
	{
		{
			QMutexLocker locker(&echo_to_cancel_mutex);
			echo_to_cancel.clear();
		}
		if(mic_resampler_buf)
		{
			free(mic_resampler_buf);
			mic_resampler_buf = nullptr;
		}
		if(echo_resampler_buf)
		{
			free(echo_resampler_buf);
			echo_resampler_buf = nullptr;
		}
	}
#endif
}

void StreamSession::Start()
{
	if(!connect_timer.isValid())
		connect_timer.start();
	ChiakiErrorCode err = chiaki_session_start(&session);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		throw ChiakiException("Chiaki Session Start failed");
	}
	session_started = true;
}

void StreamSession::Stop()
{
	mic_active.storeRelaxed(false);
	chiaki_session_stop(&session);
}

void StreamSession::GoToBed()
{
	chiaki_session_goto_bed(&session);
}

void StreamSession::ToggleMute()
{
	if(!allow_unmute)
		return;
	if(!mic_connected)
	{
#ifdef Q_OS_MACOS
		if(!mic_authorization)
		{
			macMicPermission(MacMicRequestCb, this);
			return;
		}
#endif
#if CHIAKI_GUI_ENABLE_SPEEX
		if(speech_processing_enabled)
		{
			//Use 1 channel for SPEEX processing and then mix to 2 channels
			InitMic(1, opus_encoder.audio_header.rate);
		}
		else
			InitMic(2, opus_encoder.audio_header.rate);
#else
		InitMic(2, opus_encoder.audio_header.rate);
#endif
			if(!audio_in)
			{
				CHIAKI_LOGE(GetChiakiLog(), "Microphone initialization failed, leaving microphone muted");
				return;
			}
			chiaki_session_connect_microphone(&session);
			mic_connected = true;
		}
	chiaki_session_toggle_microphone(&session, muted);
	if (muted)
		muted = false;
	else
		muted = true;
	QMetaObject::invokeMethod(this, [this]() {
		for(auto controller : controllers)
			controller->SetDualsenseMic(muted);
	});
	if(audio_in)
		SDL_PauseAudioDevice(audio_in, muted);
	emit MutedChanged();
}

void StreamSession::SetLoginPIN(const QString &pin)
{
	QByteArray data = pin.toUtf8();
	chiaki_session_set_login_pin(&session, (const uint8_t *)data.constData(), data.size());
}

void StreamSession::GoHome()
{
	chiaki_session_go_home(&session);
}

void StreamSession::HandleMousePressEvent(QMouseEvent *event)
{
	if(!mouse_touch_enabled)
		return;
	// left button for touchpad gestures, others => touchpad click
	if (event->button() != Qt::MouseButton::LeftButton)
		keyboard_state.buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
	SendFeedbackState();
}

void StreamSession::HandleMouseReleaseEvent(QMouseEvent *event)
{
	if(!mouse_touch_enabled)
		return;
	// left button => end of touchpad gesture
	if (event->button() == Qt::LeftButton)
	{
		if(mouse_touch_id >= 0)
		{
			chiaki_controller_state_stop_touch(&keyboard_state, (uint8_t)mouse_touch_id);
			mouse_touch_id = -1;
		}
	}
	keyboard_state.buttons &= ~CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
	SendFeedbackState();
}

void StreamSession::HandleMouseMoveEvent(QMouseEvent *event, qreal width, qreal height)
{
	if(!mouse_touch_enabled)
		return;
	// left button with move => touchpad gesture, otherwise ignore
	if (event->buttons() == Qt::LeftButton)
	{
		float x = std::clamp(0.0, event->scenePosition().x(), width);
		float y = std::clamp(0.0, event->scenePosition().y(), height);
		float psx = x * (PS_TOUCHPAD_MAX_X / width);
		float psy = y * (PS_TOUCHPAD_MAX_Y / height);
		// if touch id is set, move, otherwise start
		if(mouse_touch_id >= 0)
			chiaki_controller_state_set_touch_pos(&keyboard_state, (uint8_t)mouse_touch_id, (uint16_t)psx, (uint16_t)psy);
		else
			mouse_touch_id=chiaki_controller_state_start_touch(&keyboard_state, (uint16_t)psx, (uint16_t)psy);	
	}
	SendFeedbackState();
}

void StreamSession::HandleKeyboardEvent(QKeyEvent *event)
{
	if(!keyboard_controller_enabled)
		return;
	if(key_map.contains(Qt::Key(event->key())) == false)
		return;

	if(event->isAutoRepeat())
		return;

	int button = key_map[Qt::Key(event->key())];
	bool press_event = event->type() == QEvent::Type::KeyPress;

	switch(button)
	{
		case CHIAKI_CONTROLLER_ANALOG_BUTTON_L2:
			keyboard_state.l2_state = press_event ? 0xff : 0;
			break;
		case CHIAKI_CONTROLLER_ANALOG_BUTTON_R2:
			keyboard_state.r2_state = press_event ? 0xff : 0;
			break;
		case static_cast<int>(ControllerButtonExt::ANALOG_STICK_RIGHT_Y_UP):
			keyboard_state.right_y = press_event ? -0x7fff : 0;
			break;
		case static_cast<int>(ControllerButtonExt::ANALOG_STICK_RIGHT_Y_DOWN):
			keyboard_state.right_y = press_event ? 0x7fff : 0;
			break;
		case static_cast<int>(ControllerButtonExt::ANALOG_STICK_RIGHT_X_UP):
			keyboard_state.right_x = press_event ? 0x7fff : 0;
			break;
		case static_cast<int>(ControllerButtonExt::ANALOG_STICK_RIGHT_X_DOWN):
			keyboard_state.right_x = press_event ? -0x7fff : 0;
			break;
		case static_cast<int>(ControllerButtonExt::ANALOG_STICK_LEFT_Y_UP):
			keyboard_state.left_y = press_event ? -0x7fff : 0;
			break;
		case static_cast<int>(ControllerButtonExt::ANALOG_STICK_LEFT_Y_DOWN):
			keyboard_state.left_y = press_event ? 0x7fff : 0;
			break;
		case static_cast<int>(ControllerButtonExt::ANALOG_STICK_LEFT_X_UP):
			keyboard_state.left_x = press_event ? 0x7fff : 0;
			break;
		case static_cast<int>(ControllerButtonExt::ANALOG_STICK_LEFT_X_DOWN):
			keyboard_state.left_x = press_event ? -0x7fff : 0;
			break;
		default:
			if(press_event)
				keyboard_state.buttons |= button;
			else
				keyboard_state.buttons &= ~button;
			break;
	}

	SendFeedbackState();
}

void StreamSession::HandleTouchEvent(QTouchEvent *event, qreal width, qreal height)
{
	//unset touchpad (we will set it if user touches edge of screen)
	touch_state.buttons &= ~CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;

	const QList<QTouchEvent::TouchPoint> touchPoints = event->points();

	for (const QTouchEvent::TouchPoint &touchPoint : touchPoints)
	{
		int id = touchPoint.id();
		switch (touchPoint.state())
		{
			//skip unchanged touchpoints
			case QEventPoint::State::Stationary:
				continue;
			case QEventPoint::State::Pressed:
			case QEventPoint::State::Updated:
			{
				float norm_x = std::clamp(0.0, touchPoint.scenePosition().x() / width, 1.0);
				float norm_y = std::clamp(0.0, touchPoint.scenePosition().y() / height, 1.0);

				// Touching edges of screen is a touchpad click
				if(norm_x <= 0.05 || norm_x >= 0.95 || norm_y <= 0.05 || norm_y >= 0.95)
					touch_state.buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
				else if(touch_tracker.empty()) // Double tap is a touchpad click
				{
					if(double_tap_timer.isValid() && double_tap_timer.elapsed() < 500)
						touch_state.buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
					double_tap_timer.restart();
				}
				// Scale to PS TouchPad since that's what PS Console expects
				float psx = norm_x * PS_TOUCHPAD_MAX_X;
				float psy = norm_y * PS_TOUCHPAD_MAX_Y;
				auto it = touch_tracker.find(id);
				if(it == touch_tracker.end())
				{
					int8_t cid = chiaki_controller_state_start_touch(&touch_state, (uint16_t)psx, (uint16_t)psy);
					// if cid < 0 => already too many multi-touches
					if(cid >= 0)
						touch_tracker[id] = (uint8_t)cid;
					else
						break;
				}
				else
					chiaki_controller_state_set_touch_pos(&touch_state, it.value(), (uint16_t)psx, (uint16_t)psy);
				break;
			}
			case QEventPoint::State::Released:
			{
				for(auto it=touch_tracker.begin(); it!=touch_tracker.end(); it++)
				{
					if(it.key() == id)
					{
						chiaki_controller_state_stop_touch(&touch_state, it.value());
						touch_tracker.erase(it);
						break;
					}
				}
				break;
			}
		}
	}
	SendFeedbackState();
}

void StreamSession::HandleDpadTouchEvent(ChiakiControllerState *state, bool placeholder)
{
	ChiakiControllerState placeholder_touch_state;
	if(!placeholder)
		dpad_touch_timer->start();
	if(state->buttons & CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT)
	{
		state->buttons &= ~CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
		if(dpad_touch_id < 0)
		{
			dpad_touch_value = QPair<uint16_t, uint16_t>((uint16_t)(0), (uint16_t)(PS_TOUCHPAD_MAXY / 2));
			dpad_touch_id = chiaki_controller_state_start_touch(&dpad_touch_state, dpad_touch_value.first, dpad_touch_value.second);
			if(dpad_touch_stop_timer->isActive() && !placeholder)
				dpad_touch_stop_timer->stop();
			return;
		}
		if(dpad_touch_stop_timer->isActive() && !placeholder)
			dpad_touch_stop_timer->stop();
		if(dpad_touch_value.first < dpad_touch_increment)
			dpad_touch_value.first = 0;
		else
			dpad_touch_value.first -= dpad_touch_increment;
		chiaki_controller_state_set_touch_pos(&dpad_touch_state, dpad_touch_id, dpad_touch_value.first, dpad_touch_value.second);
		return;
	}
	if(state->buttons & CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT)
	{
		state->buttons &= ~CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
		// starting new touch
		if(dpad_touch_id < 0)
		{
			dpad_touch_value = QPair<uint16_t, uint16_t>((uint16_t)(PS_TOUCHPAD_MAXX), (uint16_t)(PS_TOUCHPAD_MAXY / 2));
			dpad_touch_id = chiaki_controller_state_start_touch(&dpad_touch_state, dpad_touch_value.first, dpad_touch_value.second);
			if(dpad_touch_stop_timer->isActive() && !placeholder)
				dpad_touch_stop_timer->stop();
			return;
		}
		if(dpad_touch_stop_timer->isActive() && !placeholder)
			dpad_touch_stop_timer->stop();
		if(dpad_touch_value.first > (PS_TOUCHPAD_MAXX - dpad_touch_increment))
			dpad_touch_value.first = PS_TOUCHPAD_MAXX;
		else
			dpad_touch_value.first += dpad_touch_increment;
		chiaki_controller_state_set_touch_pos(&dpad_touch_state, dpad_touch_id, dpad_touch_value.first, dpad_touch_value.second);
		return;
	}
	if(state->buttons & CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN)
	{
		state->buttons &= ~CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
		// starting new touch
		if(dpad_touch_id < 0)
		{
			dpad_touch_value = QPair<uint16_t, uint16_t>((uint16_t)(PS_TOUCHPAD_MAXX / 2), (uint16_t)(PS_TOUCHPAD_MAXY));
			dpad_touch_id = chiaki_controller_state_start_touch(&dpad_touch_state, dpad_touch_value.first, dpad_touch_value.second);
			if(dpad_touch_stop_timer->isActive() && !placeholder)
				dpad_touch_stop_timer->stop();
			return;
		}
		if(dpad_touch_stop_timer->isActive() && !placeholder)
			dpad_touch_stop_timer->stop();
		if(dpad_touch_value.second > PS_TOUCHPAD_MAXY - dpad_touch_increment)
			dpad_touch_value.second = PS_TOUCHPAD_MAXY;
		else
			dpad_touch_value.second += dpad_touch_increment;
		chiaki_controller_state_set_touch_pos(&dpad_touch_state, dpad_touch_id, dpad_touch_value.first, dpad_touch_value.second);
		return;
	}
	if(state->buttons & CHIAKI_CONTROLLER_BUTTON_DPAD_UP)
	{
		state->buttons &= ~CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
		// starting new touch
		if(dpad_touch_id < 0)
		{
			dpad_touch_value = QPair<uint16_t, uint16_t>((uint16_t)(PS_TOUCHPAD_MAXX / 2), (uint16_t)(0));
			dpad_touch_id = chiaki_controller_state_start_touch(&dpad_touch_state, dpad_touch_value.first, dpad_touch_value.second);
			if(dpad_touch_stop_timer->isActive() && !placeholder)
				dpad_touch_stop_timer->stop();
			return;
		}
		if(dpad_touch_stop_timer->isActive() && !placeholder)
			dpad_touch_stop_timer->stop();
		if(dpad_touch_value.second < dpad_touch_increment)
			dpad_touch_value.second = 0;
		else
			dpad_touch_value.second -= dpad_touch_increment;
		chiaki_controller_state_set_touch_pos(&dpad_touch_state, dpad_touch_id, dpad_touch_value.first, dpad_touch_value.second);
		return;
	}
	CHIAKI_LOGW(GetChiakiLog(), "Dpad Touch event called without dpad press, ignoring...");
}

void StreamSession::UpdateGamepads()
{
#if CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER
	for(auto controller_id : controllers.keys())
	{
		auto controller = controllers[controller_id];
		if(!controller->IsConnected())
		{
			CHIAKI_LOGI(log.GetChiakiLog(), "Controller %d disconnected", controller->GetDeviceID());
			controllers.remove(controller_id);
			if (controller->IsDualSense() || controller->IsDualSenseEdge())
			{
				DisconnectHaptics();
			}
			if (!controller->IsHandheld() && !controller->IsSteamVirtualUnmasked())
			{
				haptics_handheld++;
			}
			controller->Unref();
		}
	}

	const auto available_controllers = ControllerManager::GetInstance()->GetAvailableControllers();
	for(auto controller_id : available_controllers)
	{
		if(!controllers.contains(controller_id))
		{
			auto controller = ControllerManager::GetInstance()->OpenController(controller_id);
			if(!controller)
			{
				CHIAKI_LOGE(log.GetChiakiLog(), "Failed to open controller %d", controller_id);
				continue;
			}
			CHIAKI_LOGI(log.GetChiakiLog(), "Controller %d opened: \"%s\"", controller_id, controller->GetName().toLocal8Bit().constData());
			connect(controller, &Controller::StateChanged, this, &StreamSession::SendFeedbackState);
			connect(controller, &Controller::MicButtonPush, this, &StreamSession::ToggleMute);
			controllers[controller_id] = controller;
			if(controller->IsHandheld())
			{
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
				// let sdeck handle haptics bc sdeck works even if Steam is running whereas checking via SDL only works if Steam is not running
				if(!sdeck)
					haptics_handheld++;
#else
				haptics_handheld++;
#endif
			}
			if (!controller->IsHandheld() && !controller->IsSteamVirtualUnmasked())
			{
				haptics_handheld--;
			}
			QTimer::singleShot(1000, this, [this, controller] {
				controller->ChangePlayerIndex(player_index);
				controller->ChangeLEDColor(led_color);
			});
			if (controller->IsDualSense() || controller->IsDualSenseEdge())
			{
				uint8_t trigger_intensity = (ps5_trigger_intensity < 0) ? 0xF0 : ps5_trigger_intensity;
				uint8_t rumble_intensity = (ps5_rumble_intensity < 0) ? 0x0F : ps5_rumble_intensity;
				controller->SetDualsenseMic(muted);
				if(this->haptics_output > 0)
					continue;
				// Connect haptics audio device with a delay to give the sound system time to set up
				QTimer::singleShot(1000, this, &StreamSession::WaitHaptics);
			}
		}
	}
	SendFeedbackState();
#endif
}

void StreamSession::WaitHaptics()
{
	ConnectHaptics();
	if(!(this->haptics_output > 0))
		QTimer::singleShot(14000, this, &StreamSession::ConnectHaptics);
}

void StreamSession::DpadSendFeedbackState()
{
	ChiakiControllerState state;
	chiaki_controller_state_set_idle(&state);

#if CHIAKI_GUI_ENABLE_SETSU
	// setsu is the one that potentially has gyro/accel/orient so copy that directly first
	state = setsu_state;
#endif

	for(auto controller : controllers)
	{
		auto controller_state = controller->GetState();
		chiaki_controller_state_or(&state, &state, &controller_state);
	}

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	chiaki_controller_state_or(&state, &state, &sdeck_state);
#endif
	chiaki_controller_state_or(&state, &state, &keyboard_state);
	chiaki_controller_state_or(&state, &state, &touch_state);

	if(input_block)
	{
		// Only unblock input after all buttons were released
		if(input_block == 2 && !state.buttons)
			input_block = 0;
		else
		{
			chiaki_controller_state_set_idle(&state);
			chiaki_controller_state_set_idle(&keyboard_state);
		}
	}
	if((dpad_touch_shortcut1 || dpad_touch_shortcut2 || dpad_touch_shortcut3 || dpad_touch_shortcut4) && (!dpad_touch_shortcut1 || (state.buttons & dpad_touch_shortcut1)) && (!dpad_touch_shortcut2 || (state.buttons & dpad_touch_shortcut2)) && (!dpad_touch_shortcut3 || (state.buttons & dpad_touch_shortcut3)) && (!dpad_touch_shortcut4 || (state.buttons & dpad_touch_shortcut4)))
	{
		if(!dpad_regular_touch_switched)
		{
			dpad_regular_touch_switched = true;
			dpad_regular = !dpad_regular;
		}
	}
	else
		dpad_regular_touch_switched = false;
	if(dpad_touch_increment && !dpad_regular && (state.buttons & (CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN | CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT | CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT | CHIAKI_CONTROLLER_BUTTON_DPAD_UP)))
	{
		HandleDpadTouchEvent(&state, true);
	}
	else
	{
		if(dpad_touch_id >= 0 && !dpad_touch_stop_timer->isActive())
			dpad_touch_stop_timer->start(NEW_DPAD_TOUCH_INTERVAL_MS);
	}
	chiaki_controller_state_or(&state, &state, &dpad_touch_state);
	chiaki_session_set_controller_state(&session, &state);
}

void StreamSession::SendFeedbackState()
{
	ChiakiControllerState state;
	chiaki_controller_state_set_idle(&state);

#if CHIAKI_GUI_ENABLE_SETSU
	// setsu is the one that potentially has gyro/accel/orient so copy that directly first
	state = setsu_state;
#endif

	for(auto controller : controllers)
	{
		auto controller_state = controller->GetState();
		chiaki_controller_state_or(&state, &state, &controller_state);
	}

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	chiaki_controller_state_or(&state, &state, &sdeck_state);
#endif
	chiaki_controller_state_or(&state, &state, &keyboard_state);
	chiaki_controller_state_or(&state, &state, &touch_state);

	if(input_block)
	{
		// Only unblock input after all buttons were released
		if(input_block == 2 && !state.buttons)
			input_block = 0;
		else
		{
			chiaki_controller_state_set_idle(&state);
			chiaki_controller_state_set_idle(&keyboard_state);
		}
	}
	if((dpad_touch_shortcut1 || dpad_touch_shortcut2 || dpad_touch_shortcut3 || dpad_touch_shortcut4) && (!dpad_touch_shortcut1 || (state.buttons & dpad_touch_shortcut1)) && (!dpad_touch_shortcut2 || (state.buttons & dpad_touch_shortcut2)) && (!dpad_touch_shortcut3 || (state.buttons & dpad_touch_shortcut3)) && (!dpad_touch_shortcut4 || (state.buttons & dpad_touch_shortcut4)))
	{
		if(!dpad_regular_touch_switched)
		{
			dpad_regular_touch_switched = true;
			dpad_regular = !dpad_regular;
		}
	}
	else
		dpad_regular_touch_switched = false;
	if(dpad_touch_increment && !dpad_regular && (state.buttons & (CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN | CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT | CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT | CHIAKI_CONTROLLER_BUTTON_DPAD_UP)))
	{
		HandleDpadTouchEvent(&state);
	}
	else
	{
		if(dpad_touch_id >= 0 && !dpad_touch_stop_timer->isActive())
			dpad_touch_stop_timer->start(NEW_DPAD_TOUCH_INTERVAL_MS);
	}
	chiaki_controller_state_or(&state, &state, &dpad_touch_state);
	chiaki_session_set_controller_state(&session, &state);
	// Counts controller-state updates handed to the lib, not network packets (the lib
	// sends feedback on its own schedule and may merge several updates into one packet).
	feedback_sends.fetchAndAddRelaxed(1);

#if CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER
	// Input latency, upper bound of the client-side delay: this send minus the start of the
	// poll tick preceding the one that delivered the event (the event may have happened at
	// any time since then). Covers poll interval + GUI-thread stalls + processing, not the
	// controller/kernel path before SDL sees the event.
	const quint64 now_us = chiaki_time_now_monotonic_us();
	for(auto controller : controllers)
	{
		quint64 since_us = 0;
		if(!controller->TakeLastEventPollUs(&since_us))
			continue;
		if(since_us > now_us)
			continue;
		const quint64 latency_us64 = now_us - since_us;
		if(latency_us64 > 60ull * 1000 * 1000) // stale value, not a real measurement
			continue;
		const quint32 latency_us = static_cast<quint32>(latency_us64);
		quint32 cur = input_latency_max_us.loadRelaxed();
		while(cur < latency_us && !input_latency_max_us.testAndSetRelaxed(cur, latency_us, cur)) {}
	}
#endif
}

ChiakiNax5Counters StreamSession::GetNax5Counters()
{
	ChiakiNax5Counters snap;
	// Same as chiaki_session_get_nax5_counters(); that wrapper is declared in session.h
	// but not yet defined in the lib, so snapshot the public member directly.
	chiaki_nax5_counters_snapshot(&session.nax5_counters, &snap);
	return snap;
}

Nax5AudioStats StreamSession::GetNax5AudioStats()
{
	Nax5AudioStats stats = {};
	{
		QMutexLocker locker(&audio_out_ring_mutex);
		stats.trim_dropped_bytes = audio_out_trim_dropped_bytes;
		stats.cap_dropped_bytes = audio_out_cap_dropped_bytes;
		stats.queue_clears = audio_out_queue_clears;
		stats.ring_overflows = audio_out_ring_overflows;
		stats.starved_polls = audio_out_starved_polls;
		stats.reopens = audio_out_reopens;
	}
	// Jitter-buffer depth from the counters snapshot (the lib stores it after each
	// frame); never dereference stream_connection.audio_receiver here, the session
	// thread frees it.
	ChiakiNax5Counters snap;
	chiaki_nax5_counters_snapshot(&session.nax5_counters, &snap);
	stats.jitter_frames_last = (int)snap.audio_jitter_last;
	return stats;
}

int StreamSession::GetHasBFrames()
{
	if(!ffmpeg_decoder)
		return -1;
	return __atomic_load_n(&ffmpeg_decoder->nax5_has_b_frames, __ATOMIC_RELAXED);
}

void StreamSession::InitAudio(unsigned int channels, unsigned int rate)
{
	StopAudioOutDrainThread();
	allow_unmute = true;
	if(start_mic_unmuted)
		ToggleMute();
	if(audio_out)
	{
		SDL_CloseAudioDevice(audio_out);
		audio_out = 0;
	}
	{
		QMutexLocker locker(&audio_out_ring_mutex);
		audio_out_ring_buf.clear();
		audio_out_ring_read_pos = 0;
		audio_out_ring_write_pos = 0;
		audio_out_ring_fill = 0;
		audio_out_overflow_logged = false;
	}

	SDL_AudioSpec spec = {0};
	spec.freq = rate;
	spec.channels = channels;
	spec.format = AUDIO_S16SYS;
	audio_out_sample_size = sizeof(int16_t) * channels;
	{
		// Whole sample frames only, within a sane range (SDL takes a frame count, and the
		// queue thresholds below are derived from this value).
		const unsigned int frame = (unsigned int)audio_out_sample_size;
		const unsigned int requested = audio_buffer_size;
		unsigned int effective = qBound<unsigned int>(AUDIO_BUFFER_MIN_BYTES, requested, AUDIO_BUFFER_MAX_BYTES);
		effective -= effective % frame;
		if(effective < AUDIO_BUFFER_MIN_BYTES)
			effective += frame; // rounding took it below the minimum: next whole frame up
		if(effective != requested)
			CHIAKI_LOGI(log.GetChiakiLog(), "Audio buffer size %u bytes adjusted to %u (multiple of %u, %u..%u)",
				requested, effective, frame, (unsigned int)AUDIO_BUFFER_MIN_BYTES, (unsigned int)AUDIO_BUFFER_MAX_BYTES);
		audio_buffer_size = effective;
	}
	spec.samples = audio_buffer_size / audio_out_sample_size;
	{
		QMutexLocker locker(&audio_out_ring_mutex);
		if(audio_out_opened_once)
			audio_out_reopens++;
		audio_out_opened_once = true;
	}

	SDL_AudioSpec obtained;
	audio_out = SDL_OpenAudioDevice(audio_out_device_name.isEmpty() ? nullptr : qUtf8Printable(audio_out_device_name), false, &spec, &obtained, false);
	if(!audio_out)
	{
		CHIAKI_LOGE(log.GetChiakiLog(), "Failed to open Audio Output Device '%s': %s", qPrintable(audio_out_device_name), SDL_GetError());
		if(audio_out_device_name.isEmpty())
			return;
		audio_out_device_name.clear();
		audio_out = SDL_OpenAudioDevice(nullptr, false, &spec, &obtained, false);
		if(!audio_out)
		{
			CHIAKI_LOGE(log.GetChiakiLog(), "Failed to open default Audio Output Device: %s", SDL_GetError());
			return;
		}
	}
	if(audio_out_device_name.isEmpty())
		audio_out_device_name = "Auto";

	if(obtained.format != spec.format || obtained.channels != spec.channels || obtained.freq != spec.freq)
		CHIAKI_LOGW(log.GetChiakiLog(),
			"Audio output '%s' opened with converted format %#x, %u channels @ %u Hz (requested %#x, %u channels @ %u Hz)",
			qPrintable(audio_out_device_name), obtained.format, obtained.channels, obtained.freq, spec.format, spec.channels, spec.freq);

	{
		QMutexLocker locker(&audio_out_ring_mutex);
		// The ring absorbs bursts after network stalls. It used to be 8 device buffers
		// (400 ms at the 50 ms default), which a smaller device buffer would shrink below
		// a typical burst: keep at least AUDIO_RING_MIN_MS, in whole sample frames.
		{
			size_t ring_bytes = (size_t)audio_buffer_size * 8;
			const size_t min_bytes = (size_t)nax5_audio_bytes_for_ms(AUDIO_RING_MIN_MS, rate, audio_out_sample_size);
			if(ring_bytes < min_bytes)
				ring_bytes = min_bytes;
			ring_bytes -= ring_bytes % audio_out_sample_size;
			audio_out_ring_buf.resize((int)ring_bytes);
		}
		audio_out_ring_read_pos = 0;
		audio_out_ring_write_pos = 0;
		audio_out_ring_fill = 0;
		audio_out_overflow_logged = false;
		// queued data is in the requested format (SDL converts to the obtained one)
		audio_out_rate = rate;
		audio_out_over_cap_since_ms = 0;
		audio_out_cap_last_log_ms = 0;
		audio_out_trim = Nax5AudioTrim();
		audio_out_trim_last_log_ms = 0;
		audio_out_queue_clear_last_log_ms = 0;
		// dropped-bytes and event counters are cumulative for the session (stats CSV)
	}
	if(nax5_audio_max_ms())
		CHIAKI_LOGI(log.GetChiakiLog(), "Audio latency cap %u ms (NAX5_AUDIO_MAX_MS), headroom %u ms (NAX5_AUDIO_HEADROOM_MS)",
			nax5_audio_max_ms(), nax5_audio_headroom_ms());

	SDL_PauseAudioDevice(audio_out, 0);
	StartAudioOutDrainThread();

	CHIAKI_LOGI(log.GetChiakiLog(), "Audio Device '%s' opened with %u channels @ %d Hz, buffer size %u (effective audio_buffer_size %u bytes)",
				qPrintable(audio_out_device_name), obtained.channels, obtained.freq, obtained.size, audio_buffer_size);
}

void StreamSession::InitMic(unsigned int channels, unsigned int rate)
{
	auto clear_mic_buffers = [this]() {
		if(mic_buf.buf)
		{
			free(mic_buf.buf);
			mic_buf.buf = nullptr;
		}
		{
			QMutexLocker locker(&mic_ring_mutex);
			mic_ring_buf.clear();
			mic_ring_read_pos = 0;
			mic_ring_write_pos = 0;
			mic_ring_fill = 0;
			mic_ring_drain_queued = false;
			mic_ring_overflow_logged = false;
		}
#if CHIAKI_GUI_ENABLE_SPEEX
		if(mic_resampler_buf)
		{
			free(mic_resampler_buf);
			mic_resampler_buf = nullptr;
		}
		if(echo_resampler_buf)
		{
			free(echo_resampler_buf);
			echo_resampler_buf = nullptr;
		}
		{
			QMutexLocker locker(&echo_to_cancel_mutex);
			echo_to_cancel.clear();
		}
#endif
	};

	if(audio_in)
	{
		SDL_CloseAudioDevice(audio_in);
		audio_in = 0;
	}

	clear_mic_buffers();

	mic_buf.current_byte = 0;
	int16_t mic_buf_size = channels * MICROPHONE_SAMPLES;
	mic_buf.size_bytes = mic_buf_size * sizeof(int16_t);
	mic_buf.buf = (int16_t*) calloc(mic_buf_size, sizeof(int16_t));
	if(!mic_buf.buf)
	{
		CHIAKI_LOGE(GetChiakiLog(), "Could not allocate memory for mic buf, aborting mic startup");
		return;
	}
	{
		QMutexLocker locker(&mic_ring_mutex);
		mic_ring_buf.resize((int)(mic_buf.size_bytes * 8));
		mic_ring_read_pos = 0;
		mic_ring_write_pos = 0;
		mic_ring_fill = 0;
		mic_ring_drain_queued = false;
		mic_ring_overflow_logged = false;
	}

#if CHIAKI_GUI_ENABLE_SPEEX
	if(speech_processing_enabled)
	{
		if(SDL_BuildAudioCVT(&mic_speex_cvt, AUDIO_S16SYS, 1, 48000, AUDIO_S16SYS, 2, 48000) < 0)
		{
			CHIAKI_LOGE(GetChiakiLog(), "Failed to build mic audio converter: %s", SDL_GetError());
			clear_mic_buffers();
			return;
		}
		// Size both buffers from the frame layout, never from SDL_AudioCVT::len_ratio:
		// on Windows sdl2-compat 2.32.64 packs SDL_AudioCVT inside the DLL but the
		// public header does not, so len_ratio read here is garbage (often 0) and the
		// echo buffer came out empty, corrupting the heap on every played frame.
		// Mono mic frame -> stereo for the encoder.
		mic_speex_cvt.len = mic_buf.size_bytes;
		mic_resampler_buf = (uint8_t*) calloc(mic_buf.size_bytes * 2 * qMax(1, mic_speex_cvt.len_mult), sizeof(uint8_t));
		if(!mic_resampler_buf)
		{
			CHIAKI_LOGE(GetChiakiLog(), "Mic resampler buf could not be created, aborting mic startup");
			clear_mic_buffers();
			return;
		}

		if(SDL_BuildAudioCVT(&echo_speex_cvt, AUDIO_S16SYS, 2, 48000, AUDIO_S16SYS, 1, 48000) < 0)
		{
			CHIAKI_LOGE(GetChiakiLog(), "Failed to build echo audio converter: %s", SDL_GetError());
			clear_mic_buffers();
			return;
		}
		// Stereo playback frame (what PushAudioFrame copies in) -> mono echo reference.
		echo_speex_cvt.len = mic_buf.size_bytes * 2;
		echo_resampler_buf = (uint8_t*) calloc(echo_speex_cvt.len * qMax(1, echo_speex_cvt.len_mult), sizeof(uint8_t));
		if(!echo_resampler_buf)
		{
			CHIAKI_LOGE(GetChiakiLog(), "Echo resampler buf could not be created, aborting mic startup");
			clear_mic_buffers();
			return;
		}
	}
#endif

	SDL_AudioSpec spec = {0};
	spec.freq = rate;
	spec.channels = channels;
	spec.format = AUDIO_S16SYS;
	spec.samples = audio_buffer_size / 4;
	spec.callback = [](void *userdata, Uint8 *stream, int len) {
		auto s = static_cast<StreamSession*>(userdata);
		s->QueueMicData(stream, (size_t)len);
	};
	spec.userdata = this;

	SDL_AudioSpec obtained;
	audio_in = SDL_OpenAudioDevice(audio_in_device_name.isEmpty() ? nullptr : qUtf8Printable(audio_in_device_name), true, &spec, &obtained, false);
	if(!audio_in)
	{
		CHIAKI_LOGE(log.GetChiakiLog(), "Failed to open Microphone '%s': %s", qPrintable(audio_in_device_name), SDL_GetError());
		if(audio_in_device_name.isEmpty())
		{
			clear_mic_buffers();
			return;
		}
		audio_in_device_name.clear();
		audio_in = SDL_OpenAudioDevice(nullptr, true, &spec, &obtained, false);
		if(!audio_in)
		{
			CHIAKI_LOGE(log.GetChiakiLog(), "Failed to open default Microphone: %s", SDL_GetError());
			clear_mic_buffers();
			return;
		}
	}

	if(audio_in_device_name.isEmpty())
		audio_in_device_name = "Auto";

	if(obtained.format != spec.format || obtained.channels != spec.channels || obtained.freq != spec.freq)
		CHIAKI_LOGW(log.GetChiakiLog(),
			"Microphone '%s' opened with converted format %#x, %u channels @ %u Hz (requested %#x, %u channels @ %u Hz)",
			qPrintable(audio_in_device_name), obtained.format, obtained.channels, obtained.freq, spec.format, spec.channels, spec.freq);

	CHIAKI_LOGI(log.GetChiakiLog(), "Microphone '%s' opened with %u channels @ %u Hz, buffer size %u",
			qPrintable(audio_in_device_name), obtained.channels, obtained.freq, obtained.size);
}

void StreamSession::QueueMicData(const uint8_t *micdata, size_t micdata_size)
{
	bool schedule_drain = false;
	if(!mic_active.loadRelaxed())
		return;

	{
		QMutexLocker locker(&mic_ring_mutex);
		size_t capacity = (size_t)mic_ring_buf.size();
		if(capacity == 0 || micdata_size == 0)
			return;

		if(micdata_size >= capacity)
		{
			micdata += micdata_size - capacity;
			micdata_size = capacity;
			mic_ring_read_pos = 0;
			mic_ring_write_pos = 0;
			mic_ring_fill = 0;
		}
		else if(micdata_size > capacity - mic_ring_fill)
		{
			size_t bytes_to_drop = micdata_size - (capacity - mic_ring_fill);
			mic_ring_read_pos = (mic_ring_read_pos + bytes_to_drop) % capacity;
			mic_ring_fill -= bytes_to_drop;
			if(!mic_ring_overflow_logged)
			{
				CHIAKI_LOGW(log.GetChiakiLog(), "Mic ring buffer overflow, dropping stale captured audio");
				mic_ring_overflow_logged = true;
			}
		}

		size_t first_copy = qMin(micdata_size, capacity - mic_ring_write_pos);
		memcpy(mic_ring_buf.data() + mic_ring_write_pos, micdata, first_copy);
		if(micdata_size > first_copy)
			memcpy(mic_ring_buf.data(), micdata + first_copy, micdata_size - first_copy);
		mic_ring_write_pos = (mic_ring_write_pos + micdata_size) % capacity;
		mic_ring_fill += micdata_size;

		if(!mic_ring_drain_queued)
		{
			mic_ring_drain_queued = true;
			schedule_drain = true;
		}
	}

	if(schedule_drain)
	{
		QMetaObject::invokeMethod(this, [this]() {
			DrainMicRingBuffer();
		});
	}
}

void StreamSession::DrainMicRingBuffer()
{
	while(true)
	{
		QByteArray micdata;
		{
			QMutexLocker locker(&mic_ring_mutex);
			if(mic_ring_fill == 0)
			{
				mic_ring_drain_queued = false;
				mic_ring_overflow_logged = false;
				return;
			}

			size_t capacity = (size_t)mic_ring_buf.size();
			size_t chunk_size = qMin(mic_ring_fill, capacity - mic_ring_read_pos);
			micdata = QByteArray(mic_ring_buf.constData() + mic_ring_read_pos, (int)chunk_size);
			mic_ring_read_pos = (mic_ring_read_pos + chunk_size) % capacity;
			mic_ring_fill -= chunk_size;
		}

		if(!mic_active.loadRelaxed())
		{
			QMutexLocker locker(&mic_ring_mutex);
			mic_ring_drain_queued = false;
			mic_ring_buf.clear();
			mic_ring_read_pos = 0;
			mic_ring_write_pos = 0;
			mic_ring_fill = 0;
			mic_ring_overflow_logged = false;
			return;
		}

		ReadMic(micdata);
	}
}

#if CHIAKI_GUI_ENABLE_SPEEX
bool StreamSession::ProcessMicFrame(int16_t *echo_buf)
{
	if(!speech_processing_enabled)
	{
		chiaki_opus_encoder_frame(mic_buf.buf, &opus_encoder);
		return true;
	}

	SDL_AudioCVT cvt = mic_speex_cvt;
	cvt.len = mic_buf.size_bytes;
	cvt.buf = mic_resampler_buf;

	QByteArray echo_data;
	{
		QMutexLocker locker(&echo_to_cancel_mutex);
		if(!echo_to_cancel.isEmpty())
			echo_data = echo_to_cancel.dequeue();
	}

	if(!echo_data.isEmpty())
	{
		auto echo = reinterpret_cast<const int16_t *>(echo_data.constData());
		speex_echo_cancellation(echo_state, mic_buf.buf, echo, echo_buf);
		speex_preprocess_run(preprocess_state, echo_buf);
		memcpy(mic_resampler_buf, echo_buf, mic_buf.size_bytes);
	}
	else
	{
		speex_preprocess_run(preprocess_state, mic_buf.buf);
		memcpy(mic_resampler_buf, mic_buf.buf, mic_buf.size_bytes);
	}

	if(SDL_ConvertAudio(&cvt) != 0)
	{
		CHIAKI_LOGE(log.GetChiakiLog(), "Failed to resample mic audio: %s", SDL_GetError());
		return false;
	}

	chiaki_opus_encoder_frame(reinterpret_cast<int16_t *>(mic_resampler_buf), &opus_encoder);
	return true;
}
#endif

void StreamSession::ReadMic(const QByteArray &micdata)
{
	if(!mic_active.loadRelaxed())
		return;
#if CHIAKI_GUI_ENABLE_SPEEX
	QByteArray echo_buf_storage(mic_buf.size_bytes, 0);
	auto echo_buf = reinterpret_cast<int16_t *>(echo_buf_storage.data());
#endif
	const uint8_t *micdata_ptr = reinterpret_cast<const uint8_t *>(micdata.constData());
	size_t micdata_size = (size_t)micdata.size();
	uint32_t mic_bytes_left = mic_buf.size_bytes - mic_buf.current_byte;
	// Don't send mic data if muted
	if(muted)
		return;
	size_t bytes_read = micdata_size;
	if(bytes_read == 0)
		return;
	if(bytes_read < mic_bytes_left)
	{
		memcpy((uint8_t *)mic_buf.buf + mic_buf.current_byte, micdata_ptr, bytes_read);
		mic_buf.current_byte += bytes_read;
	}
	else
	{
		memcpy((uint8_t *)mic_buf.buf + mic_buf.current_byte, micdata_ptr, mic_bytes_left);
#if CHIAKI_GUI_ENABLE_SPEEX
		if(!ProcessMicFrame(echo_buf))
			return;
#else
	    chiaki_opus_encoder_frame(mic_buf.buf, &opus_encoder);
#endif
		bytes_read -= mic_bytes_left;
		uint32_t frames = bytes_read / mic_buf.size_bytes;
		for (int i = 0; i < frames; i++)
		{
			memcpy((uint8_t *)mic_buf.buf, micdata_ptr + mic_bytes_left + i * mic_buf.size_bytes, mic_buf.size_bytes);
#if CHIAKI_GUI_ENABLE_SPEEX
			if(!ProcessMicFrame(echo_buf))
				return;
#else
	    chiaki_opus_encoder_frame(mic_buf.buf, &opus_encoder);
#endif
		}
		mic_buf.current_byte = bytes_read % mic_buf.size_bytes;
		if(mic_buf.current_byte == 0)
			return;
		memcpy((uint8_t *)mic_buf.buf, micdata_ptr + mic_bytes_left + frames * mic_buf.size_bytes, mic_buf.current_byte);
	}
}
void StreamSession::InitHaptics()
{
	haptics_output = 0;
	haptics_buffer_size = 480; //10ms * number of ms delay configured
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	sdeck_haptics_senderr = nullptr;
	sdeck_haptics_senderl = nullptr;
#endif
	haptics_resampler_buf = nullptr;
#ifdef Q_OS_LINUX
	// Haptics work most reliably with Pipewire, so try to use that if available
	SDL_SetHint("SDL_AUDIODRIVER", "pipewire");
#endif

#ifdef Q_OS_LINUX
	if (!strstr(SDL_GetCurrentAudioDriver(), "pipewire"))
	{
		CHIAKI_LOGW(
			log.GetChiakiLog(),
			"Haptics output is not using Pipewire, this may not work reliably. (was: '%s')",
			SDL_GetCurrentAudioDriver());
	}
#endif

	SDL_AudioCVT cvt;
	if(SDL_BuildAudioCVT(&haptics_cvt, AUDIO_S16SYS, 4, 3000, AUDIO_S16SYS, 4, 48000) < 0)
	{
		CHIAKI_LOGE(log.GetChiakiLog(), "Failed to build haptics audio converter: %s", SDL_GetError());
		return;
	}
	haptics_cvt.len = 240;  // 10 16bit stereo samples
	haptics_resampler_buf = (uint8_t*) calloc(haptics_cvt.len * haptics_cvt.len_mult, sizeof(uint8_t));
	if(!haptics_resampler_buf)
		CHIAKI_LOGE(log.GetChiakiLog(),"Haptics resampler buf could not be allocated");
}

void StreamSession::DisconnectHaptics()
{
	if (this->haptics_output > 0)
	{
		SDL_CloseAudioDevice(haptics_output);
		this->haptics_output = 0;
	}
}

void StreamSession::ConnectRumbleHaptics()
{
	if(rumble_haptics_connected)
		return;
	rumble_haptics = {};
	rumble_haptics.reserve(20);
	connect(this, &StreamSession::RumbleHapticPushed, this, &StreamSession::QueueRumbleHaptics);
	auto rumble_haptics_interval = RUMBLE_HAPTICS_PACKETS_PER_RUMBLE * 10;
	auto rumble_haptics_timer = new QTimer(this);
	connect(rumble_haptics_timer, &QTimer::timeout, this, [this]{
		bool changed = false;
		uint32_t strength = 0;
		for(size_t i = 0; i < RUMBLE_HAPTICS_PACKETS_PER_RUMBLE; i++)
		{
			if(!rumble_haptics.isEmpty())
				strength += rumble_haptics.dequeue();
		}
		strength /= RUMBLE_HAPTICS_PACKETS_PER_RUMBLE;
		QMetaObject::invokeMethod(this, [this, strength]() {
			for(auto controller : controllers)
			{
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
				if(haptics_handheld < 1 && (controller->IsHandheld() || (sdeck && controller->IsSteamVirtualUnmasked())))
#else
				if(haptics_handheld < 1 && controller->IsHandheld())
#endif
					continue;

				if(strength > 0 || rumble_haptics_on)
					controller->SetHapticRumble(strength, strength);
			}
		});
		rumble_haptics_on = strength > 0 ? true : false;
	});
	rumble_haptics_timer->start(rumble_haptics_interval);
	rumble_haptics_connected = true;
}

void StreamSession::QueueRumbleHaptics(uint16_t strength)
{
	if(!rumble_haptics_connected)
		return;
	rumble_haptics.enqueue(strength);
}

void StreamSession::ConnectHaptics()
{
	if (this->haptics_output > 0)
	{
		CHIAKI_LOGW(this->log.GetChiakiLog(), "Haptics already connected to an attached DualSense controller, ignoring additional controllers.");
		return;
	}
	if (!haptics_resampler_buf)
	{
		CHIAKI_LOGW(this->log.GetChiakiLog(), "Haptics resampler buf wasn't allocated, can't use haptics.");
		return;
	}
#ifdef Q_OS_MACOS
	CHIAKI_LOGW(this->log.GetChiakiLog(), "If haptics aren't working, please configure your DualSense audio device as quadrophonic in Applications/Utitilities/Audio Midi Setup on your Mac");
#endif
	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = 48000;
	want.format = AUDIO_S16SYS;
	want.channels = 4;
	want.samples = haptics_buffer_size;
	want.callback = NULL;

	const char *device_name = nullptr;
	for (int i=0; i < SDL_GetNumAudioDevices(0); i++)
	{
		device_name = SDL_GetAudioDeviceName(i, 0);
		if (!device_name || !strstr(device_name, DUALSENSE_AUDIO_DEVICE_NEEDLE))
		{
			continue;
		}
		haptics_output = SDL_OpenAudioDevice(device_name, 0, &want, &have, 0);
		if (haptics_output == 0)
		{
			CHIAKI_LOGE(log.GetChiakiLog(), "Could not open SDL Audio Device %s for haptics output: %s", device_name, SDL_GetError());
			continue;
		}
		SDL_PauseAudioDevice(haptics_output, 0);
		CHIAKI_LOGI(log.GetChiakiLog(), "Haptics Audio Device '%s' opened with %d channels @ %d Hz, buffer size %u (driver=%s)", device_name, have.channels, have.freq, have.size, SDL_GetCurrentAudioDriver());
		QMetaObject::invokeMethod(this, [this]() {
			const uint8_t clear_effect[10] = { 0 };
			for(auto controller : controllers)
			{
				if(controller->IsDualSense() || controller->IsDualSenseEdge())
					controller->SetTriggerEffects(0x05, clear_effect, 0x05, clear_effect);
			}
		});
		return;
	}
	CHIAKI_LOGW(log.GetChiakiLog(), "DualSense features were enabled and a DualSense is connected, but could not find the DualSense audio device!");
	return;
}

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
void StreamSession::ConnectSdeckHaptics()
{
	if(!enable_steamdeck_haptics)
		return;
	sdeck_last_haptic = chiaki_time_now_monotonic_ms();
	const int num_channels = 2; // Left and right haptics
	const uint32_t samples_per_packet = 120 * sizeof(uint8_t) / (2.0 * sizeof(int16_t));
	sdeck_queue_segment = samples_per_packet * STEAMDECK_HAPTIC_PACKETS_PER_ANALYSIS;
	if (sdeck_haptic_init(sdeck, sdeck_queue_segment) < 0)
	{
		CHIAKI_LOGE(log.GetChiakiLog(), "Steam Deck Haptics Audio could not be connected :(");
		return;
	}
	CHIAKI_LOGI(log.GetChiakiLog(), "Steam Deck Haptics Audio opened with %d channels @ %d Hz with %u samples per audio analysis.", num_channels, STEAMDECK_HAPTIC_SAMPLING_RATE, sdeck_queue_segment);
	sdeck_hapticl = {};
	sdeck_hapticl.reserve(20);
	sdeck_hapticr = {};
	sdeck_hapticr.reserve(20);
	sdeck_skipl = false;
	sdeck_skipr = false;
	sdeck_haptics_senderl = (int16_t *) calloc(sdeck_queue_segment, sizeof(uint16_t));
	if(!sdeck_haptics_senderl)
	{
		CHIAKI_LOGE(log.GetChiakiLog(), "Steam Deck Haptics senderl buf could not be allocated :(");
	}
	sdeck_haptics_senderr = (int16_t *) calloc(sdeck_queue_segment, sizeof(uint16_t));
	if(!sdeck_haptics_senderr)
	{
		CHIAKI_LOGE(log.GetChiakiLog(), "Steam Deck Haptics senderr buf could not be allocated :(");
	}
	qRegisterMetaType<haptic_packet_t>();
	connect(this, &StreamSession::SdeckHapticPushed, this, &StreamSession::SdeckQueueHaptics);
	auto sdeck_haptic_interval = STEAMDECK_HAPTIC_PACKETS_PER_ANALYSIS * 10;
	auto sdeck_haptic_timer = new QTimer(this);
	connect(sdeck_haptic_timer, &QTimer::timeout, this, [this]{
		haptic_packet_t haptic_packetl = {}, haptic_packetr = {};
		bool changedl = false, changedr = false;
		for(uint64_t i = 0; i < STEAMDECK_HAPTIC_PACKETS_PER_ANALYSIS; i++)
		{
			uint64_t current_tick = sdeck_last_haptic + i * 10; // 10ms packet
			if(!sdeck_hapticl.isEmpty())
			{
				uint64_t next_timestamp = sdeck_hapticl.head().timestamp;
				if(next_timestamp > (current_tick + 10))
					memset(sdeck_haptics_senderl + 30 * i, 0, 30);
				else
				{
					haptic_packetl = sdeck_hapticl.dequeue();
					memcpy(sdeck_haptics_senderl + 30 * i, haptic_packetl.haptic_packet, 30);
					changedl = true;
				}
			}
			else
				memset(sdeck_haptics_senderl + 30 * i, 0, 30);

			if(!sdeck_hapticr.isEmpty())
			{
				uint64_t next_timestamp = sdeck_hapticr.head().timestamp;
				if(next_timestamp > (current_tick + 10))
					memset(sdeck_haptics_senderr + 30 * i, 0, 30);
				else
				{
					haptic_packetr = sdeck_hapticr.dequeue();
					memcpy(sdeck_haptics_senderr + 30 * i, haptic_packetr.haptic_packet, 30);
					changedr = true;
				}
			}
			else
				memset(sdeck_haptics_senderr + 30 * i, 0, 30);
		}

		if(!changedl || sdeck_skipl)
			sdeck_skipl = false;
		else
		{
			int intervals = play_pcm_haptic(sdeck, TRACKPAD_LEFT, sdeck_haptics_senderl, sdeck_queue_segment, STEAMDECK_HAPTIC_SAMPLING_RATE);
			if (intervals < 0)
				CHIAKI_LOGE(log.GetChiakiLog(), "Failed to submit haptics audio to SteamDeck");
			else if (intervals == 2)
				sdeck_skipl = true;
		}
		if(!changedr || sdeck_skipr)
			sdeck_skipr = false;
		else
		{
			int intervals = play_pcm_haptic(sdeck, TRACKPAD_RIGHT, sdeck_haptics_senderr, sdeck_queue_segment, STEAMDECK_HAPTIC_SAMPLING_RATE);
			if (intervals < 0)
				CHIAKI_LOGE(log.GetChiakiLog(), "Failed to submit haptics audio to SteamDeck");
			else if (intervals == 2)
				sdeck_skipr = true;
		}
		sdeck_last_haptic = chiaki_time_now_monotonic_ms();
	});
	sdeck_haptic_timer->start(sdeck_haptic_interval);
	return;
}
#endif

void StreamSession::PushAudioFrame(int16_t *og_buf, size_t samples_count)
{
	if(!audio_out || !audio_volume)
		return;

	QByteArray buf((int)(samples_count * audio_out_sample_size), 0);

	// qDebug() << "Audio queue" << (SDL_GetQueuedAudioSize(audio_out) / audio_out_sample_size / samples_count) * 10 << "ms";
	// If the SDL queue runs away, drop the stale backlog and resume from the latest frame.
	if(SDL_GetQueuedAudioSize(audio_out) > 3 * audio_buffer_size)
		HandleAudioQueueRunaway();
	if(audio_volume < SDL_MIX_MAXVOLUME)
		SDL_MixAudioFormat((uint8_t *)buf.data(), (uint8_t *)og_buf, AUDIO_S16SYS, buf.size(), audio_volume);
	else
		memcpy(buf.data(), og_buf, (size_t)buf.size());
#if CHIAKI_GUI_ENABLE_SPEEX
	// change samples to mono for processing with SPEEX
	if(echo_resampler_buf && speech_processing_enabled && !muted)
	{
		if(buf.size() != (int)(mic_buf.size_bytes * 2))
		{
			CHIAKI_LOGW(log.GetChiakiLog(),
				"Skipping echo reference frame with unexpected size %d, expected %u",
				buf.size(), mic_buf.size_bytes * 2);
			goto queue_audio;
		}
		SDL_AudioCVT cvt = echo_speex_cvt;
		cvt.len = mic_buf.size_bytes * 2;
		cvt.buf = echo_resampler_buf;
		memcpy(echo_resampler_buf, buf.constData(), mic_buf.size_bytes * 2);
		if(SDL_ConvertAudio(&cvt) != 0)
		{
			CHIAKI_LOGE(log.GetChiakiLog(), "Failed to resample echo audio: %s", SDL_GetError());
			return;
		}
		QByteArray echo_frame(reinterpret_cast<const char *>(echo_resampler_buf), cvt.len_cvt);
		QMutexLocker locker(&echo_to_cancel_mutex);
		if(echo_to_cancel.size() >= ECHO_QUEUE_MAX)
			echo_to_cancel.dequeue();
		echo_to_cancel.enqueue(echo_frame);
	}
#endif
queue_audio:
	QueueAudioOutData(buf);
}

void StreamSession::QueueAudioOutData(const QByteArray &audio_data)
{
	{
		QMutexLocker locker(&audio_out_ring_mutex);
		size_t capacity = (size_t)audio_out_ring_buf.size();
		size_t data_size = (size_t)audio_data.size();
		if(capacity == 0 || data_size == 0)
			return;

		const char *data = audio_data.constData();
		if(data_size >= capacity)
		{
			data += data_size - capacity;
			data_size = capacity;
			audio_out_ring_read_pos = 0;
			audio_out_ring_write_pos = 0;
			audio_out_ring_fill = 0;
			audio_out_ring_overflows++;
		}
		else if(data_size > capacity - audio_out_ring_fill)
		{
			size_t bytes_to_drop = data_size - (capacity - audio_out_ring_fill);
			// crossfade over the cut like the trim and the cap do, a hard cut clicks
			DropOldestAudioLocked(bytes_to_drop);
			audio_out_ring_overflows++;
			if(!audio_out_overflow_logged)
			{
				CHIAKI_LOGW(log.GetChiakiLog(), "Audio output ring overflow, dropping stale queued audio");
				audio_out_overflow_logged = true;
			}
		}

		size_t first_copy = qMin(data_size, capacity - audio_out_ring_write_pos);
		memcpy(audio_out_ring_buf.data() + audio_out_ring_write_pos, data, first_copy);
		if(data_size > first_copy)
			memcpy(audio_out_ring_buf.data(), data + first_copy, data_size - first_copy);
		audio_out_ring_write_pos = (audio_out_ring_write_pos + data_size) % capacity;
		audio_out_ring_fill += data_size;
		EnforceAudioOutCapLocked();
	}

	{
		QMutexLocker lock(&audio_out_drain_mutex);
		audio_out_drain_requested = true;
	}
	audio_out_drain_wait.wakeOne();
}

void StreamSession::EnforceAudioOutCapLocked()
{
	const unsigned int max_ms = nax5_audio_max_ms();
	if(!max_ms || !audio_out || !audio_out_rate || !audio_out_sample_size)
		return;

	const uint64_t cap_bytes = nax5_audio_bytes_for_ms(max_ms, audio_out_rate, audio_out_sample_size);
	uint64_t queued = (uint64_t)SDL_GetQueuedAudioSize(audio_out) + audio_out_ring_fill;
	const uint64_t now_ms = chiaki_time_now_monotonic_ms();
	TrimStandingAudioLocked(now_ms, queued);
	queued = (uint64_t)SDL_GetQueuedAudioSize(audio_out) + audio_out_ring_fill;
	if(queued <= cap_bytes)
	{
		audio_out_over_cap_since_ms = 0;
		return;
	}

	// Only trim when the backlog persists, short jitter bursts are left alone
	if(!audio_out_over_cap_since_ms)
	{
		audio_out_over_cap_since_ms = now_ms;
		return;
	}
	if(now_ms - audio_out_over_cap_since_ms < AUDIO_CAP_SUSTAIN_MS)
		return;
	audio_out_over_cap_since_ms = 0;

	// Drop the oldest audio from the ring (the SDL queue is kept below the cap by
	// DrainAudioOutRingBuffer), whole sample frames only to keep channel alignment.
	uint64_t drop = qMin<uint64_t>(queued - cap_bytes, audio_out_ring_fill);
	drop -= drop % audio_out_sample_size;
	const size_t capacity = (size_t)audio_out_ring_buf.size();
	if(!drop || !capacity)
		return;
	DropOldestAudioLocked((size_t)drop);
	audio_out_cap_dropped_bytes += drop;

	if(!audio_out_cap_last_log_ms || now_ms - audio_out_cap_last_log_ms >= AUDIO_CAP_LOG_INTERVAL_MS)
	{
		const uint64_t bytes_per_s = nax5_audio_bytes_for_ms(1000, audio_out_rate, audio_out_sample_size);
		const uint64_t since_last = audio_out_cap_dropped_bytes - audio_out_cap_dropped_bytes_reported;
		CHIAKI_LOGI(log.GetChiakiLog(), "Audio latency cap: dropped %llu ms of stale audio since last report (queued %llu ms > %u ms)",
			(unsigned long long)(since_last * 1000 / bytes_per_s),
			(unsigned long long)(queued * 1000 / bytes_per_s), max_ms);
		audio_out_cap_dropped_bytes_reported = audio_out_cap_dropped_bytes;
		audio_out_cap_last_log_ms = now_ms;
	}
}

// SDL queue exceeded 3 device buffers (device stalled or resumed late): the SDL part
// can only be cleared, the ring part is dropped with a crossfade down to the normal
// top-up level. Called without the ring mutex held, from both PushAudioFrame (Takion
// thread) and the drain thread after an unlocked size check: the check is repeated
// under the ring mutex so one runaway is handled and counted once.
void StreamSession::HandleAudioQueueRunaway()
{
	QMutexLocker locker(&audio_out_ring_mutex);
	if(!audio_out || SDL_GetQueuedAudioSize(audio_out) <= 3 * audio_buffer_size)
		return; // the other thread already cleared it (or the device is gone)
	SDL_ClearQueuedAudio(audio_out);
	const size_t keep = (size_t)audio_buffer_size * 2;
	if(audio_out_ring_fill > keep)
	{
		size_t drop = audio_out_ring_fill - keep;
		drop -= drop % audio_out_sample_size;
		DropOldestAudioLocked(drop);
	}
	audio_out_overflow_logged = false;
	audio_out_queue_clears++;

	const uint64_t now_ms = chiaki_time_now_monotonic_ms();
	if(!audio_out_queue_clear_last_log_ms || now_ms - audio_out_queue_clear_last_log_ms >= AUDIO_CAP_LOG_INTERVAL_MS)
	{
		CHIAKI_LOGI(log.GetChiakiLog(), "Audio queue exceeded latency threshold, cleared queued audio (%u times since last report, %u total)",
			audio_out_queue_clears - audio_out_queue_clears_reported, audio_out_queue_clears);
		audio_out_queue_clears_reported = audio_out_queue_clears;
		audio_out_queue_clear_last_log_ms = now_ms;
	}
}

void StreamSession::DropOldestAudioLocked(size_t drop)
{
	const size_t capacity = (size_t)audio_out_ring_buf.size();
	// Callers must not ask for more than is queued; clamp in release builds so the
	// ring never underflows and the caller's subsequent write fits.
	Q_ASSERT(drop <= audio_out_ring_fill);
	if(drop > audio_out_ring_fill)
		drop = audio_out_ring_fill;
	if(!capacity || !drop)
		return;
	char *ring = audio_out_ring_buf.data();
	const size_t read_pos = audio_out_ring_read_pos;
	nax5_audio_splice_s16([ring, read_pos, capacity](size_t offset) { return ring + (read_pos + offset) % capacity; },
		audio_out_sample_size / sizeof(int16_t), audio_out_ring_fill, drop,
		(size_t)(audio_out_rate * AUDIO_SPLICE_FADE_MS / 1000));
	audio_out_ring_read_pos = (read_pos + drop) % capacity;
	audio_out_ring_fill -= drop;
}

void StreamSession::TrimStandingAudioLocked(uint64_t now_ms, uint64_t queued)
{
	const unsigned int headroom_ms = nax5_audio_headroom_ms();
	const size_t capacity = (size_t)audio_out_ring_buf.size();
	if(!headroom_ms || !capacity)
		return;

	const Nax5AudioTrimParams params = {
		AUDIO_TRIM_WINDOW_MS,
		nax5_audio_bytes_for_ms(headroom_ms, audio_out_rate, audio_out_sample_size),
		nax5_audio_bytes_for_ms(AUDIO_TRIM_HYSTERESIS_MS, audio_out_rate, audio_out_sample_size),
		nax5_audio_bytes_for_ms(AUDIO_TRIM_STEP_MS, audio_out_rate, audio_out_sample_size),
	};
	uint64_t drop = nax5_audio_trim_update(audio_out_trim, params, now_ms, queued);
	// Only the ring can be trimmed; whole sample frames keep channels aligned.
	drop = qMin<uint64_t>(drop, audio_out_ring_fill);
	drop -= drop % audio_out_sample_size;
	if(!drop)
		return;
	DropOldestAudioLocked((size_t)drop);
	audio_out_trim_dropped_bytes += drop;

	if(!audio_out_trim_last_log_ms || now_ms - audio_out_trim_last_log_ms >= AUDIO_CAP_LOG_INTERVAL_MS)
	{
		const uint64_t bytes_per_s = nax5_audio_bytes_for_ms(1000, audio_out_rate, audio_out_sample_size);
		const uint64_t since_last = audio_out_trim_dropped_bytes - audio_out_trim_dropped_bytes_reported;
		CHIAKI_LOGI(log.GetChiakiLog(), "Audio latency trim: dropped %llu ms of standing audio since last report (queued %llu ms, headroom %u ms)",
			(unsigned long long)(since_last * 1000 / bytes_per_s),
			(unsigned long long)(queued * 1000 / bytes_per_s), headroom_ms);
		audio_out_trim_dropped_bytes_reported = audio_out_trim_dropped_bytes;
		audio_out_trim_last_log_ms = now_ms;
	}
}

int StreamSession::GetAudioQueuedMs()
{
	QMutexLocker locker(&audio_out_ring_mutex);
	if(!audio_out || !audio_out_rate || !audio_out_sample_size)
		return 0;
	const uint64_t queued = (uint64_t)SDL_GetQueuedAudioSize(audio_out) + audio_out_ring_fill;
	return (int)(queued * 1000 / nax5_audio_bytes_for_ms(1000, audio_out_rate, audio_out_sample_size));
}

void StreamSession::DrainAudioOutRingBuffer()
{
	size_t target_queue_size = audio_buffer_size * 2;
	// With a latency cap below the default top-up (~100 ms), keep the SDL queue under
	// the cap too, so the excess stays in the ring where it can be trimmed.
	if(nax5_audio_max_ms() && audio_out_rate && audio_out_sample_size)
	{
		size_t cap_bytes = (size_t)nax5_audio_bytes_for_ms(nax5_audio_max_ms(), audio_out_rate, audio_out_sample_size);
		cap_bytes -= cap_bytes % audio_out_sample_size;
		target_queue_size = qMin(target_queue_size, qMax((size_t)audio_buffer_size, cap_bytes));
	}

	bool first_poll = true;
	while(audio_out)
	{
		if(SDL_GetQueuedAudioSize(audio_out) > 3 * audio_buffer_size)
			HandleAudioQueueRunaway();

		const Uint32 sdl_queued = SDL_GetQueuedAudioSize(audio_out);
		if(sdl_queued >= target_queue_size)
			break;

		QByteArray audio_chunk;
		{
			QMutexLocker locker(&audio_out_ring_mutex);
			if(!audio_out)
				return;
			if(audio_out_ring_fill == 0)
			{
				audio_out_overflow_logged = false;
				return;
			}
			// Device drained everything while data was still waiting in the ring
			if(first_poll && sdl_queued == 0)
				audio_out_starved_polls++;
			first_poll = false;

			size_t capacity = (size_t)audio_out_ring_buf.size();
			size_t remaining_target = target_queue_size - sdl_queued;
			size_t chunk_size = qMin(audio_out_ring_fill, qMin(remaining_target, capacity - audio_out_ring_read_pos));
			// whole sample frames only, a partial frame would shift the channels
			chunk_size -= chunk_size % audio_out_sample_size;
			if(chunk_size == 0)
				break; // queue is within a frame of the target, wait for the next pass
			audio_chunk = QByteArray(audio_out_ring_buf.constData() + audio_out_ring_read_pos, (int)chunk_size);
			audio_out_ring_read_pos = (audio_out_ring_read_pos + chunk_size) % capacity;
			audio_out_ring_fill -= chunk_size;
		}

		if(SDL_QueueAudio(audio_out, audio_chunk.constData(), (Uint32)audio_chunk.size()) < 0)
		{
			CHIAKI_LOGE(log.GetChiakiLog(), "Failed to queue audio frame: %s", SDL_GetError());
			SDL_ClearQueuedAudio(audio_out);
			break;
		}
	}
}

void StreamSession::StartAudioOutDrainThread()
{
	QMutexLocker lock(&audio_out_drain_mutex);
	if(audio_out_drain_thread_running)
		return;
	audio_out_drain_thread_running = true;
	audio_out_drain_requested = false;
	audio_out_drain_thread = QThread::create([this]() { AudioOutDrainThreadMain(); });
	audio_out_drain_thread->start();
}

void StreamSession::StopAudioOutDrainThread()
{
	QThread *thread = nullptr;
	{
		QMutexLocker lock(&audio_out_drain_mutex);
		if(!audio_out_drain_thread_running)
			return;
		audio_out_drain_thread_running = false;
		audio_out_drain_requested = true;
		thread = audio_out_drain_thread;
		audio_out_drain_thread = nullptr;
	}
	audio_out_drain_wait.wakeOne();
	if(thread)
	{
		thread->wait();
		delete thread;
	}
}

void StreamSession::AudioOutDrainThreadMain()
{
	audio_out_drain_mutex.lock();
	while(audio_out_drain_thread_running)
	{
		while(audio_out_drain_thread_running && !audio_out_drain_requested)
			audio_out_drain_wait.wait(&audio_out_drain_mutex);
		if(!audio_out_drain_thread_running)
			break;

		audio_out_drain_requested = false;
		audio_out_drain_mutex.unlock();

		while(true)
		{
			DrainAudioOutRingBuffer();

			bool has_pending_audio = false;
			{
				QMutexLocker locker(&audio_out_ring_mutex);
				has_pending_audio = audio_out && audio_out_ring_fill > 0;
			}
			if(!has_pending_audio)
				break;

			audio_out_drain_mutex.lock();
			if(!audio_out_drain_thread_running)
			{
				audio_out_drain_mutex.unlock();
				return;
			}
			audio_out_drain_wait.wait(&audio_out_drain_mutex, 4);
			if(!audio_out_drain_thread_running)
			{
				audio_out_drain_mutex.unlock();
				return;
			}
			audio_out_drain_requested = false;
			audio_out_drain_mutex.unlock();
		}

		audio_out_drain_mutex.lock();
	}
	audio_out_drain_mutex.unlock();
}

#ifdef Q_OS_MACOS
void StreamSession::SetMicAuthorization(Authorization authorization)
{
	switch(authorization)
	{
		case AUTHORIZED:
			mic_authorization = true;
			ToggleMute();
			break;
		case DENIED:
			CHIAKI_LOGE(GetChiakiLog(), "You have denied mic access. Please manually enable mic access in your System Preferences.");
			break;
		case RESTRICTED:
			CHIAKI_LOGE(GetChiakiLog(), "Access to the microphone is restricted. Please change your parental controls to allow enabling mic access if desired.");
			break;
	}
}
#endif

void StreamSession::PushHapticsFrame(uint8_t *buf, size_t buf_size)
{
	if(buf_size == 0)
	{
		CHIAKI_LOGW(log.GetChiakiLog(), "Received empty haptics frame");
		return;
	}

	if((buf_size % (2 * sizeof(int16_t))) != 0)
	{
		CHIAKI_LOGE(log.GetChiakiLog(), "Haptics audio has invalid size: %zu", buf_size);
		return;
	}

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	if(sdeck && haptics_handheld > 0 && enable_steamdeck_haptics)
	{
		if(ps5_rumble_intensity < 0 || haptic_override < 0.01)
			return;
		float intensity = haptic_override * rumble_multiplier;
		if(buf_size != 120)
		{
			CHIAKI_LOGE(log.GetChiakiLog(), "Haptic audio of incompatible size: %zu", buf_size);
			return;
		}
		int16_t amplitudel = 0, amplituder = 0;
		const size_t sample_size = 2 * sizeof(int16_t); // stereo samples
		haptic_packet_t packetl = {0}, packetr = {0};
		uint64_t timestamp = chiaki_time_now_monotonic_ms();
		packetl.timestamp = timestamp;
		packetr.timestamp = timestamp;
		size_t buf_count = buf_size / sample_size;
		for (size_t i = 0; i < buf_count; i++)
		{
			size_t cur = i * sample_size;
			memcpy(&amplitudel, buf + cur, sizeof(int16_t));
			int32_t adjustedl = (int32_t)amplitudel * intensity;
			adjustedl = (adjustedl > INT16_MAX) ? INT16_MAX : adjustedl;
			amplitudel = (adjustedl < INT16_MIN) ? INT16_MIN : adjustedl;
			packetl.haptic_packet[i] = amplitudel;
			memcpy(&amplituder, buf + cur + sizeof(int16_t), sizeof(int16_t));
			int32_t adjustedr = (int32_t)amplituder * intensity;
			adjustedr = (adjustedr > INT16_MAX) ? INT16_MAX : adjustedr;
			amplituder = (adjustedr < INT16_MIN) ? INT16_MIN : adjustedr;
			packetr.haptic_packet[i] = amplituder;
		}
		emit SdeckHapticPushed(packetl, packetr);
		return;
	}
#endif
	if((rumble_haptics_intensity != RumbleHapticsIntensity::Off) && haptics_output == 0)
	{
		int16_t amplitudel = 0, amplituder = 0;
		uint32_t suml = 0, sumr = 0;
		const size_t sample_size = 2 * sizeof(int16_t); // stereo samples

		size_t buf_count = buf_size / sample_size;
		for (size_t i = 0; i < buf_count; i++){
			size_t cur = i * sample_size;

			memcpy(&amplitudel, buf + cur, sizeof(int16_t));
			memcpy(&amplituder, buf + cur + sizeof(int16_t), sizeof(int16_t));
			suml += static_cast<uint32_t>(qFabs(amplitudel)) * 2;
			sumr += static_cast<uint32_t>(qFabs(amplituder)) * 2;
		}
		uint32_t temp_left = (suml / buf_count);
		uint32_t temp_right = (sumr / buf_count);
		uint16_t original_strength = (temp_left > temp_right) ? temp_left : temp_right;
		uint16_t left = 0;
		uint16_t right = 0;
		temp_left = (temp_left > HAPTIC_RUMBLE_MIN_STRENGTH) ? temp_left : 0;
		temp_right = (temp_right > HAPTIC_RUMBLE_MIN_STRENGTH) ? temp_right : 0;
		if(temp_left == 0 && temp_right == 0)
			return;
		switch(rumble_haptics_intensity)
		{
			case RumbleHapticsIntensity::VeryWeak:
				left = temp_left / 5;
				right = temp_right / 5;
				break;
			case RumbleHapticsIntensity::Weak:
				left = temp_left / 2;
				right = temp_right / 2;
				break;
			case RumbleHapticsIntensity::Normal:
				left = temp_left;
				right = temp_right;
				break;
			case RumbleHapticsIntensity::Strong:
				temp_left *= 2;
				temp_right *= 2;
				left = (temp_left > UINT16_MAX) ? UINT16_MAX : temp_left;
				right = (temp_right > UINT16_MAX) ? UINT16_MAX : temp_right;
				break;
			case RumbleHapticsIntensity::VeryStrong:
				temp_left *= 5;
				temp_right *= 5;
				left = (temp_left > UINT16_MAX) ? UINT16_MAX : temp_left;
				right = (temp_right > UINT16_MAX) ? UINT16_MAX : temp_right;
				break;
			default:
				left = temp_left;
				right = temp_right;
				break;
		}
		// Set minimum rumble value if above rumble min for controllers that shift up to 9 bits when rumbling
		left = ((left > 0 && left < (1 << 9)) ? (1 << 9) : left);
		right = ((right > 0 && right < (1 << 9)) ? (1 << 9) : right);
		uint16_t strength = (left > right) ? left : right;
		bool send_rumble_haptics = false;
		for(auto controller : controllers)
		{
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
			if(haptics_handheld < 1 && (controller->IsHandheld() || (sdeck && controller->IsSteamVirtualUnmasked())))
#else
			if(haptics_handheld < 1 && controller->IsHandheld())
#endif
				continue;
			send_rumble_haptics = true;
		}
		if(send_rumble_haptics)
			emit RumbleHapticPushed(strength);
		return;
	}
	if(haptics_output == 0)
		return;
	if(ps5_rumble_intensity < 0)
		return;
	if(!haptics_resampler_buf)
	{
		CHIAKI_LOGE(log.GetChiakiLog(), "Haptics output is active without a resampler buffer");
		return;
	}
	SDL_AudioCVT cvt = haptics_cvt;
	cvt.len = buf_size * 2;
	cvt.buf = haptics_resampler_buf;
	// Remix to 4 channels
	if(haptic_override > 0.99 && haptic_override < 1.01)
	{
		for (int i=0; i < buf_size; i+=4)
		{
			SDL_memset(haptics_resampler_buf + i * 2, 0, 4);
			SDL_memcpy(haptics_resampler_buf + (i * 2) + 4, buf + i, 4);
		}
	}
	else
	{
		for (int i=0; i < buf_size; i+=4)
		{
			SDL_memset(haptics_resampler_buf + i * 2, 0, 4);
			SDL_memcpy(haptics_resampler_buf + (i * 2) + 4, buf + i, 4);
			int16_t amplitudel = (*reinterpret_cast<int16_t *>(haptics_resampler_buf + (i * 2) + 4));
			int32_t adjustedl = static_cast<int32_t>(amplitudel) * haptic_override;
			adjustedl = (adjustedl > INT16_MAX) ? INT16_MAX : adjustedl;
			amplitudel = (adjustedl < INT16_MIN) ? INT16_MIN : adjustedl;
			(*reinterpret_cast<int16_t *>(haptics_resampler_buf + (i * 2) + 4)) = amplitudel;
			int16_t amplituder = (*reinterpret_cast<int16_t *>(haptics_resampler_buf + (i * 2) + 6));
			int32_t adjustedr = static_cast<int32_t>(amplituder) * haptic_override;
			adjustedr = (adjustedr > INT16_MAX) ? INT16_MAX : adjustedr;
			amplituder = (adjustedr < INT16_MIN) ? INT16_MIN : adjustedr;
			(*reinterpret_cast<int16_t *>(haptics_resampler_buf + (i * 2) + 6)) = amplituder;
		}
	}
	// Resample to 48kHZ
	if (SDL_ConvertAudio(&cvt) != 0)
	{
		CHIAKI_LOGE(log.GetChiakiLog(), "Failed to resample haptics audio: %s", SDL_GetError());
		return;
	}

	if (SDL_QueueAudio(haptics_output, cvt.buf, cvt.len_cvt) < 0)
	{
		CHIAKI_LOGE(log.GetChiakiLog(), "Failed to submit haptics audio to device: %s", SDL_GetError());
		return;
	}
}

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
void StreamSession::SdeckQueueHaptics(haptic_packet_t packetl, haptic_packet_t packetr)
{
	sdeck_hapticl.enqueue(packetl);
	sdeck_hapticr.enqueue(packetr);
}
#endif

void StreamSession::Event(ChiakiEvent *event)
{
	switch(event->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			connect_timer.invalidate();
			connected = true;
			emit ConnectedChanged();
			emit PingChanged();
			break;
		case CHIAKI_EVENT_QUIT:
			if(!connected && !holepunch_session && chiaki_quit_reason_is_error(event->quit.reason) && connect_timer.elapsed() < SESSION_RETRY_SECONDS * 1000)
			{
				// Upstream passed SESSION_RETRY_SECONDS / 3 (= 6) as milliseconds and hammered
				// the console with ~150 session requests in 2 s while it was still busy.
				QTimer::singleShot(nax5StreamRetryDelayMs(connect_timer.elapsed()), this, &StreamSession::Start);
				return;
			}
			connected = false;
			emit ConnectedChanged();
			emit PingChanged();
			emit SessionQuit(event->quit.reason, event->quit.reason_str ? QString::fromUtf8(event->quit.reason_str) : QString());
			break;
		case CHIAKI_EVENT_REGIST:
			emit AutoRegistSucceeded(event->host);
			break;
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
			emit LoginPINRequested(event->login_pin_request.pin_incorrect);
			break;
		case CHIAKI_EVENT_HOLEPUNCH:
			emit DataHolepunchProgress(event->data_holepunch.finished);
			break;
		case CHIAKI_EVENT_NICKNAME_RECEIVED:
			emit NicknameReceived(event->server_nickname);
			break;
		case CHIAKI_EVENT_RUMBLE: {
			if(ps5_rumble_intensity < 0)
				return;
			uint8_t left = event->rumble.left;
			uint8_t right = event->rumble.right;
			uint8_t left_adj = left * rumble_multiplier;
			uint8_t right_adj = right * rumble_multiplier;
			QMetaObject::invokeMethod(this, [this, left, right, left_adj, right_adj]() {
				for(auto controller : controllers)
				{
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
					if(haptics_handheld < 1 && (controller->IsHandheld() || (sdeck && controller->IsSteamVirtualUnmasked())))
#else
					if(haptics_handheld < 1 && controller->IsHandheld())
#endif
						continue;
					if(controller->IsDualSense() || controller->IsDualSenseEdge())
						controller->SetRumble(left, right);
					else
						controller->SetRumble(left_adj, right_adj);
				}
			});
			break;
		}
		case CHIAKI_EVENT_LED_COLOR: {
			memcpy(led_color, event->led_state, 3);
			uint8_t led_state[3];
			memcpy(led_state, led_color, 3);
			QMetaObject::invokeMethod(this, [this, led_state]() {
				for(auto controller : controllers)
					controller->ChangeLEDColor(led_state);
			});
			break;
		}
		case CHIAKI_EVENT_PLAYER_INDEX: {
			player_index = event->player_index;
			QMetaObject::invokeMethod(this, [this]() {
				for(auto controller : controllers)
					controller->ChangePlayerIndex(player_index);
			});
			break;
		}
		case CHIAKI_EVENT_VIDEO_FEC_FAILURE: {
			// The lib already logs the failure; only count it here and retry the IDR
			// request if the lib could not send one.
			if(!event->video_fec_failure.idr_request_sent)
				RequestIDR();
			fec_failures.fetchAndAddRelaxed(1);
			emit FecFailure();
			break;
		}
		case CHIAKI_EVENT_MOTION_RESET: {
			QMetaObject::invokeMethod(this, [this]() {
				for(auto controller : controllers)
					controller->resetMotionControls();
			});
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
			if(sdeck)
			{
				chiaki_accel_new_zero_set_active(&sdeck_accel_zero, sdeck_real_accel.accel_x, sdeck_real_accel.accel_y, sdeck_real_accel.accel_z, false);
				chiaki_orientation_tracker_init(&sdeck_orient_tracker);
				chiaki_orientation_tracker_update(
					&sdeck_orient_tracker, sdeck_state.gyro_x, sdeck_state.gyro_y, sdeck_state.gyro_z,
					sdeck_real_accel.accel_x, sdeck_real_accel.accel_y, sdeck_real_accel.accel_z, &sdeck_accel_zero, false, chiaki_time_now_monotonic_us());
				chiaki_orientation_tracker_apply_to_controller_state(&sdeck_orient_tracker, &sdeck_state);
			}
#endif
#if CHIAKI_GUI_ENABLE_SETSU
			chiaki_accel_new_zero_set_active(&setsu_accel_zero, setsu_real_accel.accel_x, setsu_real_accel.accel_y, setsu_real_accel.accel_z, false);
			chiaki_orientation_tracker_init(&orient_tracker);
			chiaki_orientation_tracker_update(
				&orient_tracker, setsu_state.gyro_x, setsu_state.gyro_y, setsu_state.gyro_z,
				setsu_real_accel.accel_x, setsu_real_accel.accel_y, setsu_real_accel.accel_z, &setsu_accel_zero, false, chiaki_time_now_monotonic_us());
			chiaki_orientation_tracker_apply_to_controller_state(&orient_tracker, &setsu_state);
#endif
			break;
		}
		case CHIAKI_EVENT_HAPTIC_INTENSITY: {
			switch(event->intensity)
			{
				case Off: {
					ps5_rumble_intensity = -1;
					rumble_multiplier = 0;
					break;
				}
				case Strong: {
					ps5_rumble_intensity = 0x00;
					rumble_multiplier = 1.0;
					break;
				}
				case Weak: {
					ps5_rumble_intensity = 0x03;
					rumble_multiplier = 0.33;
					break;
				}
				case Medium: {
					ps5_rumble_intensity = 0x02;
					rumble_multiplier = 0.5;
					break;
				}
			}
			uint8_t trigger_intensity = (ps5_trigger_intensity < 0) ? 0xF0 : ps5_trigger_intensity;
			uint8_t rumble_intensity = (ps5_rumble_intensity < 0) ? 0x0F : ps5_rumble_intensity;
			emit DualSenseIntensityChanged(trigger_intensity | rumble_intensity);
			break;
		}
		case CHIAKI_EVENT_TRIGGER_INTENSITY: {
			switch(event->intensity)
			{
				case Off: {
					ps5_trigger_intensity = -1;
					break;
				}
				case Strong: {
					ps5_trigger_intensity = 0x00;
					break;
				}
				case Weak: {
					ps5_trigger_intensity = 0x90;
					break;
				}
				case Medium: {
					ps5_trigger_intensity = 0x60;
					break;
				}
			}
			uint8_t trigger_intensity = (ps5_trigger_intensity < 0) ? 0xF0 : ps5_trigger_intensity;
			uint8_t rumble_intensity = (ps5_rumble_intensity < 0) ? 0x0F : ps5_rumble_intensity;
			emit DualSenseIntensityChanged(trigger_intensity | rumble_intensity);
			break;
		}
		case CHIAKI_EVENT_TRIGGER_EFFECTS: {
			if(ps5_trigger_intensity < 0)
				return;
			uint8_t type_left = event->trigger_effects.type_left;
			uint8_t data_left[10];
			memcpy(data_left, event->trigger_effects.left, 10);
			uint8_t data_right[10];
			memcpy(data_right, event->trigger_effects.right, 10);
			uint8_t type_right = event->trigger_effects.type_right;
			QMetaObject::invokeMethod(this, [this, type_left, data_left, type_right, data_right]() {
				for(auto controller : controllers)
					controller->SetTriggerEffects(type_left, data_left, type_right, data_right);
			});
			break;
		}
		default:
			break;
	}
}

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
void StreamSession::HandleSDeckEvent(SDeckEvent *event)
{
	if(!sdeck)
	{
		CHIAKI_LOGI(GetChiakiLog(), "Steam Deck was disconnected! Skipping stale events...\n");
		return;
	}
	// right now only one event, use switch here for more in the future
	switch(event->type)
	{
		case SDECK_EVENT_MOTION:
			if(!vertical_sdeck)
			{
				chiaki_accel_new_zero_set_active(&sdeck_real_accel, event->motion.accel_x,
				event->motion.accel_y, event->motion.accel_z, true);
				chiaki_orientation_tracker_update(&sdeck_orient_tracker,
					event->motion.gyro_x, event->motion.gyro_y, event->motion.gyro_z,
					event->motion.accel_x, event->motion.accel_y, event->motion.accel_z,
					&sdeck_accel_zero, false, chiaki_time_now_monotonic_us());
			}
			else // swap y with z axis to use roll instead of yaw
			{
				// use calculated orient
				chiaki_accel_new_zero_set_active(&sdeck_real_accel, event->motion.accel_x,
				-event->motion.accel_z, event->motion.accel_y, true);
				chiaki_orientation_tracker_update(&sdeck_orient_tracker,
					event->motion.gyro_x, -event->motion.gyro_z, event->motion.gyro_y,
					event->motion.accel_x, -event->motion.accel_z, event->motion.accel_y,
					&sdeck_accel_zero, false, chiaki_time_now_monotonic_us());
			}
			sdeck_orient_dirty = true;
			break;
		case SDECK_EVENT_GYRO_ENABLE:
			if(event->enabled)
				CHIAKI_LOGI(GetChiakiLog(), "Gyro enabled for Steam Deck");
			else
				CHIAKI_LOGE(GetChiakiLog(), "Gyro could not be enabled for Steam Deck");
			break;
	}
}
#endif

void StreamSession::CantDisplayMessage(bool cant_display)
{
	this->cant_display = cant_display;
	emit CantDisplayChanged(cant_display);
}

#if CHIAKI_GUI_ENABLE_SETSU
void StreamSession::HandleSetsuEvent(SetsuEvent *event)
{
	if(!setsu)
		return;
	switch(event->type)
	{
		case SETSU_EVENT_DEVICE_ADDED:
			switch(event->dev_type)
			{
				case SETSU_DEVICE_TYPE_TOUCHPAD:
					// connect all the touchpads!
					if(setsu_connect(setsu, event->path, event->dev_type))
						CHIAKI_LOGI(GetChiakiLog(), "Connected Setsu Touchpad Device %s", event->path);
					else
						CHIAKI_LOGE(GetChiakiLog(), "Failed to connect to Setsu Touchpad Device %s", event->path);
					break;
				case SETSU_DEVICE_TYPE_MOTION:
					// connect only one motion since multiple make no sense
					if(setsu_motion_device)
					{
						CHIAKI_LOGI(GetChiakiLog(), "Setsu Motion Device %s detected there is already one connected",
								event->path);
						break;
					}
					setsu_motion_device = setsu_connect(setsu, event->path, event->dev_type);
					if(setsu_motion_device)
						CHIAKI_LOGI(GetChiakiLog(), "Connected Setsu Motion Device %s", event->path);
					else
						CHIAKI_LOGE(GetChiakiLog(), "Failed to connect to Setsu Motion Device %s", event->path);
					break;
			}
			break;
		case SETSU_EVENT_DEVICE_REMOVED:
			switch(event->dev_type)
			{
				case SETSU_DEVICE_TYPE_TOUCHPAD:
					CHIAKI_LOGI(GetChiakiLog(), "Setsu Touchpad Device %s disconnected", event->path);
					for(auto it=setsu_ids.begin(); it!=setsu_ids.end();)
					{
						if(it.key().first == event->path)
						{
							chiaki_controller_state_stop_touch(&setsu_state, it.value());
							setsu_ids.erase(it++);
						}
						else
							it++;
					}
					SendFeedbackState();
					break;
				case SETSU_DEVICE_TYPE_MOTION:
					if(!setsu_motion_device || strcmp(setsu_device_get_path(setsu_motion_device), event->path))
						break;
					CHIAKI_LOGI(GetChiakiLog(), "Setsu Motion Device %s disconnected", event->path);
					setsu_motion_device = nullptr;
					chiaki_orientation_tracker_init(&orient_tracker);
					orient_dirty = true;
					break;
			}
			break;
		case SETSU_EVENT_TOUCH_DOWN:
			break;
		case SETSU_EVENT_TOUCH_UP:
			for(auto it=setsu_ids.begin(); it!=setsu_ids.end(); it++)
			{
				if(it.key().first == setsu_device_get_path(event->dev) && it.key().second == event->touch.tracking_id)
				{
					chiaki_controller_state_stop_touch(&setsu_state, it.value());
					setsu_ids.erase(it);
					break;
				}
			}
			SendFeedbackState();
			break;
		case SETSU_EVENT_TOUCH_POSITION: {
			QPair<QString, SetsuTrackingId> k =  { setsu_device_get_path(event->dev), event->touch.tracking_id };
			auto it = setsu_ids.find(k);
			if(it == setsu_ids.end())
			{
				int8_t cid = chiaki_controller_state_start_touch(&setsu_state, event->touch.x, event->touch.y);
				if(cid >= 0)
					setsu_ids[k] = (uint8_t)cid;
				else
					break;
			}
			else
				chiaki_controller_state_set_touch_pos(&setsu_state, it.value(), event->touch.x, event->touch.y);
			SendFeedbackState();
			break;
		}
		case SETSU_EVENT_BUTTON_DOWN:
			setsu_state.buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
			break;
		case SETSU_EVENT_BUTTON_UP:
			setsu_state.buttons &= ~CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
			break;
		case SETSU_EVENT_MOTION:
			chiaki_accel_new_zero_set_active(&sdeck_real_accel, event->motion.accel_x,
			event->motion.accel_y, event->motion.accel_z, true);
			chiaki_orientation_tracker_update(&orient_tracker,
					event->motion.gyro_x, event->motion.gyro_y, event->motion.gyro_z,
					event->motion.accel_x, event->motion.accel_y, event->motion.accel_z,
					&setsu_accel_zero, false, event->motion.timestamp);
			orient_dirty = true;
			break;
	}
}
#endif

ChiakiErrorCode StreamSession::InitiatePsnConnection(QString psn_token)
{
	ChiakiLog *log = GetChiakiLog();
	holepunch_session = chiaki_holepunch_session_init(psn_token.toUtf8().constData(), log);
	if(!holepunch_session)
	{
		CHIAKI_LOGE(log, "!! Failed to initialize session");
		return CHIAKI_ERR_MEMORY;
	}
	chiaki_holepunch_session_force_port_guessing(
			holepunch_session,
			port_guessing_enabled);
	chiaki_holepunch_session_set_port_guessing_ports(
				holepunch_session,
				port_guess_count);
	chiaki_holepunch_session_set_port_guessing_socks(
			holepunch_session,
			port_guess_socket_count);
	return CHIAKI_ERR_SUCCESS;
}

ChiakiErrorCode StreamSession::ConnectPsnConnection(QString duid, bool ps5)
{
	ChiakiLog *log = GetChiakiLog();
	if(ps5)
		CHIAKI_LOGI(log, "Duid: %s", duid.toUtf8().constData());
	size_t duid_len = duid.size();
	size_t duid_bytes_len = duid_len / 2;
	size_t duid_bytes_lenr = duid_bytes_len;
	uint8_t duid_bytes[duid_bytes_len];
	memset(duid_bytes, 0, duid_bytes_len);
	parse_hex(duid_bytes, &duid_bytes_lenr, duid.toUtf8().constData(), duid_len);
	if(duid_bytes_len != duid_bytes_lenr)
	{
		CHIAKI_LOGE(log, "Couldn't convert duid string to bytes got size mismatch");
		return CHIAKI_ERR_INVALID_DATA;
	}
	ChiakiHolepunchConsoleType console_type = ps5 ? CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS5 : CHIAKI_HOLEPUNCH_CONSOLE_TYPE_PS4;
	ChiakiErrorCode err = chiaki_holepunch_upnp_discover(holepunch_session);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(log, "!! Failed to run upnp discover");
		return err;
	}
	err = chiaki_holepunch_session_create(holepunch_session);
	if (err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(log, "!! Failed to create session");
		return err;
	}
	CHIAKI_LOGI(log, ">> Created session");
	err = holepunch_session_create_offer(holepunch_session);
	if (err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(log, "!! Failed to create offer msg for ctrl connection");
		return err;
	}
	CHIAKI_LOGI(log, ">> Created offer msg for ctrl connection");
	err = chiaki_holepunch_session_start(holepunch_session, duid_bytes, console_type);
	if (err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(log, "!! Failed to start session");
		return err;
	}
	CHIAKI_LOGI(log, ">> Started session");

	err = chiaki_holepunch_session_punch_hole(holepunch_session, CHIAKI_HOLEPUNCH_PORT_TYPE_CTRL);
	if (err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(log, "!! Failed to punch hole for control connection.");
		return err;
	}
	CHIAKI_LOGI(log, ">> Punched hole for control connection!");
	return err;
}

void StreamSession::CancelPsnConnection(bool stop_thread)
{
	chiaki_holepunch_main_thread_cancel(holepunch_session, stop_thread);
}

void StreamSession::TriggerFfmpegFrameAvailable()
{
	emit FfmpegFrameAvailable();
	if(measured_bitrate != session.stream_connection.measured_bitrate)
	{
		measured_bitrate = session.stream_connection.measured_bitrate;
		emit MeasuredBitrateChanged();
	}
	if(target_bitrate != session.stream_connection.target_bitrate)
	{
		target_bitrate = session.stream_connection.target_bitrate;
		emit TargetBitrateChanged();
	}
}

bool StreamSession::RequestIDR()
{
	ChiakiErrorCode err = chiaki_session_request_idr(&session);
	if(err == CHIAKI_ERR_SUCCESS)
	{
		if(session.stream_connection.video_receiver)
			chiaki_video_receiver_set_waiting_for_idr(session.stream_connection.video_receiver, true);
		return true;
	}
	CHIAKI_LOGW(log.GetChiakiLog(), "StreamSession failed to request IDR frame: %s", chiaki_error_string(err));
	return false;
}

class StreamSessionPrivate
{
	public:
		static void InitAudio(StreamSession *session, uint32_t channels, uint32_t rate)
		{
			QMetaObject::invokeMethod(session, "InitAudio", Qt::ConnectionType::BlockingQueuedConnection, Q_ARG(unsigned int, channels), Q_ARG(unsigned int, rate));
		}

		static void InitMic(StreamSession *session, uint32_t channels, uint32_t rate)
		{
			QMetaObject::invokeMethod(session, "InitMic", Qt::ConnectionType::BlockingQueuedConnection, Q_ARG(unsigned int, channels), Q_ARG(unsigned int, rate));
		}

		static void PushAudioFrame(StreamSession *session, int16_t *buf, size_t samples_count)	{ session->PushAudioFrame(buf, samples_count); }
		static void PushHapticsFrame(StreamSession *session, uint8_t *buf, size_t buf_size)	{ session->PushHapticsFrame(buf, buf_size); }
#ifdef Q_OS_MACOS
		static void SetMicAuthorization(StreamSession *session, Authorization authorization)                 { session->SetMicAuthorization(authorization); }
#endif
		static void CantDisplayMessage(StreamSession *session, bool cant_display)	{session->CantDisplayMessage(cant_display); }
		static void Event(StreamSession *session, ChiakiEvent *event)							{ session->Event(event); }
#if CHIAKI_GUI_ENABLE_SETSU
		static void HandleSetsuEvent(StreamSession *session, SetsuEvent *event)					{ session->HandleSetsuEvent(event); }
#endif
#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
		static void HandleSDeckEvent(StreamSession *session, SDeckEvent *event)					{ session->HandleSDeckEvent(event); }
#endif
		static void TriggerFfmpegFrameAvailable(StreamSession *session)							{ session->TriggerFfmpegFrameAvailable(); }
};

static void AudioSettingsCb(uint32_t channels, uint32_t rate, void *user)
{
	auto session = reinterpret_cast<StreamSession *>(user);
	StreamSessionPrivate::InitAudio(session, channels, rate);
}

static void AudioFrameCb(int16_t *buf, size_t samples_count, void *user)
{
	auto session = reinterpret_cast<StreamSession *>(user);
	StreamSessionPrivate::PushAudioFrame(session, buf, samples_count);
}

#ifdef Q_OS_MACOS
static void MacMicRequestCb(Authorization authorization, void *user)
{
	auto session = reinterpret_cast<StreamSession *>(user);
	StreamSessionPrivate::SetMicAuthorization(session, authorization);
}
#endif

static void HapticsFrameCb(uint8_t *buf, size_t buf_size, void *user)
{
	auto session = reinterpret_cast<StreamSession *>(user);
	StreamSessionPrivate::PushHapticsFrame(session, buf, buf_size);
}

static void CantDisplayCb(void *user, bool cant_display)
{
	auto session = reinterpret_cast<StreamSession *>(user);
	StreamSessionPrivate::CantDisplayMessage(session, cant_display);
}

static void EventCb(ChiakiEvent *event, void *user)
{
	auto session = reinterpret_cast<StreamSession *>(user);
	StreamSessionPrivate::Event(session, event);
}

#if CHIAKI_GUI_ENABLE_SETSU
static void SessionSetsuCb(SetsuEvent *event, void *user)
{
	auto session = reinterpret_cast<StreamSession *>(user);
	StreamSessionPrivate::HandleSetsuEvent(session, event);
}
#endif

#if CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
static void SessionSDeckCb(SDeckEvent *event, void *user)
{
	auto session = reinterpret_cast<StreamSession *>(user);
	StreamSessionPrivate::HandleSDeckEvent(session, event);
}
#endif

static void FfmpegFrameCb(ChiakiFfmpegDecoder *decoder, void *user)
{
	auto session = reinterpret_cast<StreamSession *>(user);
	StreamSessionPrivate::TriggerFfmpegFrameAvailable(session);
}
