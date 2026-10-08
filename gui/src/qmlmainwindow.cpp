#include "qmlmainwindow.h"
#include "qmlbackend.h"
#include "qmlsvgprovider.h"
#include "controllermanager.h"
#include "chiaki/log.h"
#include "chiaki/time.h"
#include "chiaki/session.h"
#include "streamsession.h"
#include "nax5/nax5processlog.h"

#include <qpa/qplatformnativeinterface.h>

#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include <QDebug>
#include <QThread>
#include <QShortcut>
#include <QStandardPaths>
#include <QGuiApplication>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QDateTime>
#include <QMetaEnum>
#include <QDir>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOffscreenSurface>
#include <QSurfaceFormat>
#include <QVulkanInstance>
#include <QQuickItem>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickRenderTarget>
#include <QQuickRenderControl>
#include <QQuickGraphicsDevice>
#include <QTimer>
#include <QtGlobal>
#include <algorithm>
#include <climits>
#include <mutex>
#include <cstring>
#include <numeric>
#include <unistd.h>
#include <utility>
#include <vector>
#if defined(Q_OS_MACOS)
#include <objc/message.h>
#endif

Q_LOGGING_CATEGORY(chiakiGui, "chiaki.gui", QtInfoMsg);

static void detectOpenGLGpuVendor(QmlMainWindow *window)
{
    auto *ctx = QOpenGLContext::currentContext();
    if (!ctx)
        return;

    const GLubyte *vendor_raw = ctx->extraFunctions()->glGetString(GL_VENDOR);
    if (!vendor_raw)
        return;

    QByteArray vendor = QByteArray(reinterpret_cast<const char *>(vendor_raw)).toLower();
    if (vendor.contains("amd") || vendor.contains("ati"))
    {
        window->setProperty("_chiaki_detected_amd", true);
        qCInfo(chiakiGui) << "Using amd graphics card";
    }
    else if (vendor.contains("nvidia"))
    {
        window->setProperty("_chiaki_detected_nvidia", true);
        qCInfo(chiakiGui) << "Using nvidia graphics card";
    }
}

static void placebo_log_cb(void *user, pl_log_level level, const char *msg)
{
    ChiakiLogLevel chiaki_level;
    switch (level) {
    case PL_LOG_NONE:
    case PL_LOG_TRACE:
    case PL_LOG_DEBUG:
        qCDebug(chiakiGui).noquote() << "[libplacebo]" << msg;
        break;
    case PL_LOG_INFO:
        qCInfo(chiakiGui).noquote() << "[libplacebo]" << msg;
        break;
    case PL_LOG_WARN:
        qCWarning(chiakiGui).noquote() << "[libplacebo]" << msg;
        break;
    case PL_LOG_ERR:
    case PL_LOG_FATAL:
        qCCritical(chiakiGui).noquote() << "[libplacebo]" << msg;
        break;
    }
}

static bool map_frame(pl_gpu gpu, pl_tex *tex,
                      const struct pl_source_frame *src,
                      struct pl_frame *out_frame)
{
    AVFrame *frame = reinterpret_cast<AVFrame *>(src->frame_data);
    QmlMainWindow *q = reinterpret_cast<QmlMainWindow *>(frame->opaque);
    pl_avframe_params av_params{
        .frame      = frame,
        .tex        = tex,};
    bool ok = pl_map_avframe_ex(gpu, out_frame, &av_params
    );

    if (!ok) {
        fprintf(stderr, "Failed mapping AVFrame!\n");
        qCWarning(chiakiGui) << "Failed to map AVFrame to Placebo frame!";
        if(q->getBackend() && q->getBackend()->zeroCopy())
        {
            qCInfo(chiakiGui) << "Mapping frame failed, trying without zero copy!";
            q->getBackend()->disableZeroCopy();
        }
        av_frame_free(&frame);
        return false;
    }
    if (out_frame->num_planes <= 0) {
        qCWarning(chiakiGui) << "Mapped frame contains no planes, dropping";
        pl_unmap_avframe(gpu, out_frame);
        av_frame_free(&frame);
        return false;
    }
    return true;
}

static void unmap_frame(pl_gpu gpu, struct pl_frame *frame,
                        const struct pl_source_frame *src)
{
    pl_unmap_avframe(gpu, frame);
    AVFrame *av_frame = reinterpret_cast<AVFrame *>(src->frame_data);
    av_frame_free(&av_frame);
}

static void discard_frame(const struct pl_source_frame *src)
{
    AVFrame *frame = reinterpret_cast<AVFrame *>(src->frame_data);
    QmlMainWindow *q = reinterpret_cast<QmlMainWindow *>(frame->opaque);
    q->increaseQueueCulls();
    av_frame_free(&frame);
    qCDebug(chiakiGui) << "Dropped frame with PTS" << src->pts;
}

static void discard_snapshot_frame(const struct pl_source_frame *src)
{
    AVFrame *frame = reinterpret_cast<AVFrame *>(src->frame_data);
    av_frame_free(&frame);
}

static AVFrame *clone_frame_for_replay(const AVFrame *frame)
{
    if (!frame)
        return nullptr;

    return av_frame_clone(frame);
}

// NAX5 latency/power tweaks, each can be turned off for A/B comparison:
//   NAX5_IMMEDIATE_RENDER=0  render on the refresh timer only (upstream behaviour)
//   NAX5_EAGER_SNAPSHOT=1    copy every hardware frame to RAM for replay (upstream behaviour)
//   NAX5_FRAME_MIX=1         blend neighbouring frames even with immediate render (upstream behaviour)
//   NAX5_SKIP_REPEAT=1       skip re-presenting an unchanged frame (default off: re-presenting felt smoother in play)
//   NAX5_FAST_CHROMA=0       upscale 4:2:0 chroma with ewa_lanczossharp in HighQuality presets (upstream)
//   NAX5_CACHE_SAVE_ON_STOP=0 save the shader cache only on exit, not when a stream ends (upstream)
static bool nax5_env_flag(const char *name, bool fallback)
{
    const QByteArray v = qgetenv(name);
    if (v.isEmpty())
        return fallback;
    return v != "0";
}

static bool nax5_immediate_render()
{
    static const bool enabled = nax5_env_flag("NAX5_IMMEDIATE_RENDER", true);
    return enabled;
}

static bool nax5_eager_snapshot()
{
    static const bool enabled = nax5_env_flag("NAX5_EAGER_SNAPSHOT", false);
    return enabled;
}

static bool nax5_frame_mix()
{
    static const bool enabled = nax5_env_flag("NAX5_FRAME_MIX", false);
    return enabled;
}

static bool nax5_skip_repeat()
{
    static const bool enabled = nax5_env_flag("NAX5_SKIP_REPEAT", false);
    return enabled;
}

static bool nax5_fast_chroma()
{
    static const bool enabled = nax5_env_flag("NAX5_FAST_CHROMA", true);
    return enabled;
}

static bool nax5_cache_save_on_stop()
{
    static const bool enabled = nax5_env_flag("NAX5_CACHE_SAVE_ON_STOP", true);
    return enabled;
}

// Idle re-render interval while the shown frame doesn't change (NAX5_SKIP_REPEAT):
// keeps the freeze/replay and pending-frame logic in render() running.
static constexpr int nax5_heartbeat_ms = 50;

// NAX5_DEBAND_GRAIN: grain strength for the deband pass of the HighQuality presets
// (libplacebo's default adds noise that costs bitrate-like detail on a 800p panel).
static float nax5_deband_grain()
{
    static const float grain = []() {
        bool ok = false;
        const float v = qgetenv("NAX5_DEBAND_GRAIN").trimmed().toFloat(&ok);
        return ok && v >= 0.0f ? v : 0.0f;
    }();
    return grain;
}

// NAX5_PL_LOG = warn (default) | info | debug: libplacebo log level.
static pl_log_level nax5_pl_log_level()
{
    const QByteArray v = qgetenv("NAX5_PL_LOG").trimmed().toLower();
    if (v == "debug" || v == "trace")
        return PL_LOG_DEBUG;
    if (v == "info")
        return PL_LOG_INFO;
    return PL_LOG_WARN;
}

// NAX5_FRAME_TRACE=1: per-frame CSV next to the stats log.
static bool nax5_frame_trace()
{
    static const bool enabled = nax5_env_flag("NAX5_FRAME_TRACE", false);
    return enabled;
}

static const char *nax5_present_mode_name(int mode)
{
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo_relaxed";
    case -1: return "opengl";
    default: return "unknown";
    }
}

// Monotonic arrival time (chiaki_time_now_monotonic_us) attached by the decoder to
// AVFrame.opaque_ref; 0 when absent.
static uint64_t nax5_frame_arrival_us(const AVFrame *frame)
{
    if (!frame || !frame->opaque_ref || frame->opaque_ref->size < sizeof(uint64_t))
        return 0;
    uint64_t v;
    memcpy(&v, frame->opaque_ref->data, sizeof(v));
    return v;
}

// The AVFrame behind a pl_frame mapped by map_frame (pl_map_avframe_ex); owned by the
// pl_frame, valid until pl_unmap_avframe.
static const AVFrame *nax5_avframe_of(const struct pl_frame *frame)
{
    if (!frame || !frame->user_data)
        return nullptr;
    return pl_get_mapped_avframe(frame);
}

// Lock-free "store the max": the row writer resets these with fetchAndStore(0), so a
// plain load/max/store could overwrite a reset or lose a larger concurrent value.
template <typename T>
static void nax5_atomic_max(QAtomicInteger<T> &a, T v)
{
    T cur = a.loadRelaxed();
    while (v > cur && !a.testAndSetRelaxed(cur, v, cur)) {
    }
}

// present_age_packed_current layout: count in the top 24 bits, sum of microseconds below.
static constexpr int nax5_age_count_shift = 40;
static constexpr quint64 nax5_age_sum_mask = (quint64(1) << nax5_age_count_shift) - 1;

// Power telemetry for the stats log (Steam Deck / amdgpu); empty strings when absent.
static QByteArray nax5_read_sysfs(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QByteArray();
    return f.readAll().trimmed();
}

static QString nax5_find_hwmon_dir(const char *name)
{
    const QDir hwmon(QStringLiteral("/sys/class/hwmon"));
    for (const QString &entry : hwmon.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString d = hwmon.filePath(entry);
        if (nax5_read_sysfs(d + QStringLiteral("/name")) == name)
            return d;
    }
    return QString();
}

// A hwmon directory found once is kept; a missing one (driver not loaded yet, e.g. Wi-Fi
// after resume) is looked up again every 30 s. GUI thread only (stats row writer).
struct Nax5HwmonCache {
    const char *name;
    QString dir;
    uint64_t next_try_ms = 0;
};

static QString nax5_cached_hwmon_dir(Nax5HwmonCache &cache)
{
    if (!cache.dir.isEmpty())
        return cache.dir;
    const uint64_t now_ms = chiaki_time_now_monotonic_ms();
    if (cache.next_try_ms && now_ms < cache.next_try_ms)
        return QString();
    cache.dir = nax5_find_hwmon_dir(cache.name);
    if (cache.dir.isEmpty())
        cache.next_try_ms = now_ms + 30000;
    return cache.dir;
}

static QString nax5_amdgpu_hwmon_dir()
{
    static Nax5HwmonCache cache{ "amdgpu", QString(), 0 };
    return nax5_cached_hwmon_dir(cache);
}

static QString nax5_wifi_hwmon_dir()
{
    static Nax5HwmonCache cache{ "ath11k_hwmon", QString(), 0 };
    return nax5_cached_hwmon_dir(cache);
}

// temp1_input of a hwmon directory in degrees C, empty when absent.
static QString nax5_hwmon_temp_c(const QString &dir)
{
    if (dir.isEmpty())
        return QString();
    bool ok = false;
    const double milli = nax5_read_sysfs(dir + QStringLiteral("/temp1_input")).toDouble(&ok);
    return ok ? QString::number(milli / 1000.0, 'f', 1) : QString();
}

// Mean of scaling_cur_freq over all CPUs (kHz -> MHz), empty when unavailable.
static QString nax5_cpu_mhz_avg()
{
    static const QStringList paths = []() {
        QStringList out;
        const QDir cpus(QStringLiteral("/sys/devices/system/cpu"));
        for (const QString &entry : cpus.entryList({ QStringLiteral("cpu[0-9]*") }, QDir::Dirs | QDir::System)) {
            const QString p = cpus.filePath(entry) + QStringLiteral("/cpufreq/scaling_cur_freq");
            if (QFile::exists(p))
                out << p;
        }
        return out;
    }();
    double sum = 0.0;
    int n = 0;
    for (const QString &p : paths) {
        bool ok = false;
        const double khz = nax5_read_sysfs(p).toDouble(&ok);
        if (ok) {
            sum += khz;
            n++;
        }
    }
    return n ? QString::number(sum / n / 1000.0, 'f', 0) : QString();
}

// Aggregate "cpu" line of /proc/stat: total and idle (idle + iowait) jiffies.
static bool nax5_cpu_stat(quint64 &total, quint64 &idle)
{
    QFile f(QStringLiteral("/proc/stat"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    const QList<QByteArray> fields = f.readLine().simplified().split(' ');
    if (fields.size() < 5 || fields[0] != "cpu")
        return false;
    total = 0;
    idle = 0;
    for (int i = 1; i < fields.size(); i++) {
        const quint64 v = fields[i].toULongLong();
        total += v;
        if (i == 4 || i == 5)
            idle += v;
    }
    return true;
}

// utime + stime of this process in clock ticks (fields 14 and 15 of /proc/self/stat,
// counted after the ')' that closes the command name).
static bool nax5_proc_ticks(quint64 &ticks)
{
    QFile f(QStringLiteral("/proc/self/stat"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    const QByteArray line = f.readAll();
    const int paren = line.lastIndexOf(')');
    if (paren < 0)
        return false;
    const QList<QByteArray> fields = line.mid(paren + 1).simplified().split(' ');
    // fields[0] is state (field 3), so utime (14) and stime (15) are at 11 and 12.
    if (fields.size() < 13)
        return false;
    ticks = fields[11].toULongLong() + fields[12].toULongLong();
    return true;
}

// RcvbufErrors from the "Udp:" lines of /proc/net/snmp, -1 when unavailable.
static qint64 nax5_udp_rcvbuf_errors()
{
    QFile f(QStringLiteral("/proc/net/snmp"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return -1;
    const QList<QByteArray> lines = f.readAll().split('\n');
    for (int i = 0; i + 1 < lines.size(); i++) {
        if (!lines[i].startsWith("Udp:"))
            continue;
        const QList<QByteArray> names = lines[i].simplified().split(' ');
        const QList<QByteArray> values = lines[i + 1].simplified().split(' ');
        const int col = names.indexOf("RcvbufErrors");
        if (col > 0 && col < values.size())
            return values[col].toLongLong();
        return -1;
    }
    return -1;
}

// Redact an IPv4 address to its first two octets; anything else is hidden entirely.
static QString nax5_redact_host(const QString &host)
{
    const QStringList parts = host.split(QLatin1Char('.'));
    bool ipv4 = parts.size() == 4;
    for (const QString &p : parts) {
        bool ok = false;
        const int v = p.toInt(&ok);
        if (!ok || v < 0 || v > 255)
            ipv4 = false;
    }
    if (ipv4)
        return parts[0] + QLatin1Char('.') + parts[1] + QStringLiteral(".x.x");
    return QStringLiteral("redacted");
}

// Percentile of a sample vector (nearest rank on a sorted copy); NaN when empty.
static double nax5_percentile(QVector<float> samples, double p)
{
    if (samples.isEmpty())
        return qQNaN();
    std::sort(samples.begin(), samples.end());
    const int idx = qBound(0, static_cast<int>(qRound(p * (samples.size() - 1))), samples.size() - 1);
    return samples[idx];
}

static QString nax5_fmt(double v, int decimals = 1)
{
    return qIsNaN(v) ? QStringLiteral("-") : QString::number(v, 'f', decimals);
}

// Audio bytes -> milliseconds: Remote Play audio is always 48 kHz stereo s16 (192 bytes/ms).
static constexpr double nax5_audio_bytes_per_ms = 48000.0 * 2 * 2 / 1000.0;

static QString nax5_amdgpu_device_dir()
{
    static const QString dir = []() {
        const QDir drm(QStringLiteral("/sys/class/drm"));
        for (const QString &entry : drm.entryList({ QStringLiteral("card?") }, QDir::Dirs | QDir::System)) {
            const QString d = drm.filePath(entry) + QStringLiteral("/device");
            if (QFile::exists(d + QStringLiteral("/gpu_busy_percent")))
                return d;
        }
        return QString();
    }();
    return dir;
}

struct Nax5PowerSample
{
    QString apu_w, apu_now_w, bat_w, gpu_busy, gpu_mhz;
};

static Nax5PowerSample nax5_power_sample()
{
    Nax5PowerSample out;
    const QString hwmon = nax5_amdgpu_hwmon_dir();
    if (!hwmon.isEmpty()) {
        // Whole APU (CPU + GPU + video engines), SMU average ("slowPPT"), in microwatts.
        bool ok = false;
        const double uw = nax5_read_sysfs(hwmon + QStringLiteral("/power1_average")).toDouble(&ok);
        if (ok)
            out.apu_w = QString::number(uw / 1e6, 'f', 2);
        // Instantaneous socket power; reads higher than the SMU average, logged to compare.
        const double now_uw = nax5_read_sysfs(hwmon + QStringLiteral("/power1_input")).toDouble(&ok);
        if (ok)
            out.apu_now_w = QString::number(now_uw / 1e6, 'f', 2);
    }
    const QString dev = nax5_amdgpu_device_dir();
    if (!dev.isEmpty()) {
        out.gpu_busy = QString::fromLatin1(nax5_read_sysfs(dev + QStringLiteral("/gpu_busy_percent")));
        // The current level is the one marked with '*'.
        for (const QByteArray &line : nax5_read_sysfs(dev + QStringLiteral("/pp_dpm_sclk")).split('\n')) {
            if (line.endsWith('*')) {
                const QList<QByteArray> parts = line.simplified().split(' ');
                if (parts.size() >= 2)
                    out.gpu_mhz = QString::fromLatin1(parts[1]).remove(QStringLiteral("Mhz"), Qt::CaseInsensitive);
                break;
            }
        }
    }
    // Whole device draw, only meaningful on battery.
    const QString bat = QStringLiteral("/sys/class/power_supply/BAT1");
    if (nax5_read_sysfs(bat + QStringLiteral("/status")) == "Discharging") {
        bool ok_i = false, ok_v = false;
        const double ua = nax5_read_sysfs(bat + QStringLiteral("/current_now")).toDouble(&ok_i);
        const double uv = nax5_read_sysfs(bat + QStringLiteral("/voltage_now")).toDouble(&ok_v);
        if (ok_i && ok_v)
            out.bat_w = QString::number(ua * uv / 1e12, 'f', 2);
    }
    return out;
}

static AVFrame *make_fallback_snapshot_frame(const AVFrame *frame)
{
    if (!frame)
        return nullptr;

    if (!frame->hw_frames_ctx || frame->format == AV_PIX_FMT_VULKAN)
        return nullptr;

    AVFrame *copy = av_frame_alloc();
    if (!copy)
        return nullptr;

    if (av_hwframe_transfer_data(copy, frame, 0) < 0) {
        av_frame_free(&copy);
        return av_frame_clone(frame);
    }

    int sw_format = copy->format;
    int sw_width = copy->width;
    int sw_height = copy->height;
    AVRational sw_sar = copy->sample_aspect_ratio;
    AVColorSpace sw_colorspace = copy->colorspace;
    AVColorRange sw_color_range = copy->color_range;
    AVColorPrimaries sw_color_primaries = copy->color_primaries;
    AVColorTransferCharacteristic sw_color_trc = copy->color_trc;
    av_frame_copy_props(copy, frame);
    copy->format = sw_format >= 0 ? sw_format : frame->format;
    copy->width = sw_width;
    copy->height = sw_height;
    copy->sample_aspect_ratio = sw_sar;
    copy->colorspace = sw_colorspace;
    copy->color_range = sw_color_range;
    copy->color_primaries = sw_color_primaries;
    copy->color_trc = sw_color_trc;
    if (copy->hw_frames_ctx) {
        av_buffer_unref(&copy->hw_frames_ctx);
        copy->hw_frames_ctx = nullptr;
    }
    return copy;
}

static QString shader_cache_path()
{
    static QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/pl_shader.cache";
    return path;
}


static const char *render_params_path()
{
    static QString path = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/Chiaki/pl_render_params.conf";
    return qPrintable(path);
}

static const struct pl_hook *load_mpv_hook(pl_gpu gpu, const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCWarning(chiakiGui) << "Failed to open shader resource" << path;
        return nullptr;
    }

    QByteArray shader = file.readAll();
    const struct pl_hook *hook = pl_mpv_user_shader_parse(gpu, shader.constData(), static_cast<size_t>(shader.size()));
    if (!hook)
        qCWarning(chiakiGui) << "Failed to parse shader resource" << path;
    return hook;
}

static bool uses_fsrcnnx(PlaceboUpscaler upscaler)
{
    return upscaler == PlaceboUpscaler::FSRCNNX8 || upscaler == PlaceboUpscaler::FSRCNNX16;
}

static constexpr float spatial_upscale_threshold = 1.2f;

static bool uses_custom_upscale_hook(PlaceboUpscaler upscaler)
{
    return upscaler == PlaceboUpscaler::FSR || uses_fsrcnnx(upscaler);
}

static const struct pl_hook *select_spatial_hook(QmlMainWindow::VideoPreset preset,
                                                 PlaceboUpscaler upscaler,
                                                 float upscale_factor,
                                                 const struct pl_hook *fsr_hook,
                                                 const struct pl_hook *hook8,
                                                 const struct pl_hook *hook16)
{
    if (upscale_factor <= 1.0f)
        return nullptr;

    switch (preset) {
    case QmlMainWindow::VideoPreset::HighQualitySpatial:
        return upscale_factor >= spatial_upscale_threshold ? hook8 : fsr_hook;
    case QmlMainWindow::VideoPreset::HighQualityAdvancedSpatial:
        return upscale_factor >= spatial_upscale_threshold ? hook16 : fsr_hook;
    case QmlMainWindow::VideoPreset::Custom:
        switch (upscaler) {
        case PlaceboUpscaler::FSR:
            return fsr_hook;
        case PlaceboUpscaler::FSRCNNX8:
            return upscale_factor >= spatial_upscale_threshold ? hook8 : fsr_hook;
        case PlaceboUpscaler::FSRCNNX16:
            return upscale_factor >= spatial_upscale_threshold ? hook16 : fsr_hook;
        default:
            return nullptr;
        }
    default:
        return nullptr;
    }
}

static void clearQuickOpenGLTarget(QOpenGLFramebufferObject *fbo)
{
    if (!fbo)
        return;

    auto *ctx = QOpenGLContext::currentContext();
    if (!ctx)
        return;

    QOpenGLExtraFunctions *gl = ctx->extraFunctions();
    if (!gl)
        return;

    gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(fbo->handle()));
    gl->glViewport(0, 0, fbo->width(), fbo->height());
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

pl_gpu QmlMainWindow::placeboGpu() const
{
    if (placebo_vulkan)
        return placebo_vulkan->gpu;
    if (placebo_opengl)
        return placebo_opengl->gpu;
    return nullptr;
}

class RenderControl : public QQuickRenderControl
{
public:
    explicit RenderControl(QWindow *window)
        : window(window)
    {
    }

    QWindow *renderWindow(QPoint *offset) override
    {
        Q_UNUSED(offset);
        return window;
    }

private:
    QWindow *window = {};
};

QmlMainWindow::QmlMainWindow(Settings *settings, bool exit_app_on_stream_exit)
    : QWindow()
    , settings(settings)
{
    init(settings, exit_app_on_stream_exit);
}

QmlMainWindow::QmlMainWindow(const StreamSessionConnectInfo &connect_info)
    : QWindow()
    , settings(connect_info.settings)
{
    direct_stream = true;
    emit directStreamChanged();
    init(connect_info.settings);
    backend->createSession(connect_info);

    if (connect_info.fullscreen || connect_info.zoom || connect_info.stretch)
        fullscreenTime();
    if (connect_info.zoom)
        setVideoMode(VideoMode::Zoom);
    else if (connect_info.stretch)
        setVideoMode(VideoMode::Stretch);

    connect(session, &StreamSession::SessionQuit, qGuiApp, &QGuiApplication::quit);
}

QmlMainWindow::~QmlMainWindow()
{
    Q_ASSERT(!placebo_swapchain);
    closeStreamStatsLog();

    if (render_backend == RenderBackend::Vulkan)
        av_buffer_unref(&vulkan_hw_dev_ctx);

    const bool isOpenGlBackend = render_backend == RenderBackend::OpenGL;
    bool hasOpenGlContextCurrent = isOpenGlBackend && makeOpenGLContextCurrent();

    if (isOpenGlBackend && quick_window)
        quick_window->setRenderTarget(QQuickRenderTarget());

#ifndef Q_OS_MACOS
    if (owns_render_thread) {
        QMetaObject::invokeMethod(quick_render, &QQuickRenderControl::invalidate);
        render_thread->quit();
        render_thread->wait();
    } else {
        quick_render->invalidate();
    }
#else
    // On macOS, we still need to invalidate to prevent leaks
    if (owns_render_thread) {
        QMetaObject::invokeMethod(quick_render, &QQuickRenderControl::invalidate);
        render_thread->quit();
        render_thread->wait();
    } else {
        quick_render->invalidate();
    }
#endif

    if (pl_gpu gpu = placeboGpu()) {
        pl_tex_destroy(gpu, &quick_tex);
        for (auto &tex : placebo_tex)
            pl_tex_destroy(gpu, &tex);
        if (render_backend == RenderBackend::Vulkan)
            pl_vulkan_sem_destroy(gpu, &quick_sem);
    }

    {
        QMutexLocker locker(&pending_frame_mutex);
        if (pending_frame)
            av_frame_free(&pending_frame);
        pending_frame_stored_us = 0;
        pending_pts = 0.0;
        pending_duration = 0.0f;
        pending_frame_queue_origin = 0.0;
        pending_frame_present.storeRelease(0);
    }
    {
        QMutexLocker locker(&reset_seed_mutex);
        if (reset_seed_frame)
            av_frame_free(&reset_seed_frame);
        reset_seed_pts = 0.0;
        reset_seed_duration = 0.0f;
        reset_seed_generation = 0;
    }
    clearSnapshotFrame();

    // Only destroy the FBO while its OpenGL context is current.
    if (hasOpenGlContextCurrent) {
        delete quick_fbo;
    } else if (quick_fbo) {
        // Try one more time to make context current for proper cleanup
        if (isOpenGlBackend && makeOpenGLContextCurrent()) {
            delete quick_fbo;
            hasOpenGlContextCurrent = true;
        }
    }
    quick_fbo = nullptr;

    if (hasOpenGlContextCurrent)
        doneOpenGLContextCurrent();

    delete quick_item;
    delete quick_window;
    // calls invalidate here if not already called
    delete quick_render;
    if (owns_render_thread)
        delete render_thread->parent();
    delete qml_engine;

    saveShaderCache();
    pl_cache_destroy(&placebo_cache);
    pl_queue_destroy(&placebo_queue);
    pl_renderer_destroy(&placebo_renderer);
    if (render_backend == RenderBackend::Vulkan) {
        pl_vulkan_destroy(&placebo_vulkan);
        pl_vk_inst_destroy(&placebo_vk_inst);
    } else {
        pl_opengl_destroy(&placebo_opengl);
    }
    delete qt_vk_inst;
    delete qt_gl_offscreen_surface;
    delete qt_gl_context;
    pl_options_free(&renderparams_opts);
    pl_mpv_user_shader_destroy(&fsr_hook);
    pl_mpv_user_shader_destroy(&fsrcnnx_hook_8);
    pl_mpv_user_shader_destroy(&fsrcnnx_hook_16);
    pl_log_destroy(&placebo_log);
}

void QmlMainWindow::updateWindowType(WindowType type)
{
    switch (type) {
    case WindowType::SelectedResolution:
        setVideoMode(VideoMode::Normal);
        break;
    case WindowType::CustomResolution:
        setVideoMode(VideoMode::Normal);
        break;
    case WindowType::Fullscreen:
        fullscreenTime();
        setVideoMode(VideoMode::Normal);
        break;
    case WindowType::Zoom:
        fullscreenTime();
        setVideoMode(VideoMode::Zoom);
        break;
    case WindowType::Stretch:
        fullscreenTime();
        setVideoMode(VideoMode::Stretch);
        break;
    default:
        break;
    }
}

bool QmlMainWindow::hasVideo() const
{
    return has_video;
}

void QmlMainWindow::writeStreamStatsRow(int stream_fps, int display_fps, int dropped, const QString &resolution)
{
    if (!session || !session->GetConnected()) {
        closeStreamStatsLog();
        return;
    }
    if (!stats_log && stats_log_failed)
        return;
    if (!stats_log) {
        // ~250 KB per hour of streaming; keep the newest 60 sessions.
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + QStringLiteral("/stream-stats");
        QDir().mkpath(dir);
        const QStringList old_logs = QDir(dir).entryList({ QStringLiteral("*.csv") }, QDir::Files, QDir::Name | QDir::Reversed);
        for (int i = 59; i < old_logs.size(); ++i)
            QFile::remove(dir + QLatin1Char('/') + old_logs[i]);
        // UTC keeps names in chronological order across DST changes; milliseconds keep
        // a quick reconnect from reusing the previous name.
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const QString base = dir + now.toString(QStringLiteral("/yyyy-MM-dd_HH-mm-ss-zzz"));
        stats_log = new QFile(base + QStringLiteral(".csv"));
        if (!stats_log->open(QIODevice::NewOnly | QIODevice::Text)) {
            qCWarning(chiakiGui) << "Cannot write stream stats log" << stats_log->fileName();
            delete stats_log;
            stats_log = nullptr;
            stats_log_failed = true;
            return;
        }
        stats_log_clock.start();
        // Counters accumulated before the log opened belong to no row: drop them.
        catch_up_count.storeRelaxed(0);
        repeat_skips.storeRelaxed(0);
        queue_culls_current.storeRelaxed(0);
        late_presents_current.storeRelaxed(0);
        swap_gap_us_max_current.storeRelaxed(0);
        render_us_max_current.storeRelaxed(0);
        present_age_us_max_current.storeRelaxed(0);
        present_age_packed_current.storeRelaxed(0);
        skip_miss_overlay_current.storeRelaxed(0);
        skip_miss_key_current.storeRelaxed(0);
        skip_miss_invalid_current.storeRelaxed(0);
        skip_miss_gate_current.storeRelaxed(0);
        qml_updates_current.storeRelaxed(0);
        swap_stats_restart.storeRelease(1);
        // The first gui_stall_max_ms would otherwise include the app startup.
        ControllerManager::GetInstance()->TakeGuiStallMaxMs();
        // For the render-thread statistics: frame duration fallback and pts time base.
        session_max_fps.storeRelaxed(session->GetMaxFps());
        {
            AVRational tb = { 0, 1 };
            if (ChiakiFfmpegDecoder *dec = session->GetFfmpegDecoder()) {
                if (dec->codec_context)
                    tb = dec->codec_context->pkt_timebase;
                if (tb.num <= 0 || tb.den <= 0)
                    tb = dec->synthetic_time_base;
            }
            const bool tb_known = tb.num > 0 && tb.den > 0;
            frame_tb_num.storeRelaxed(tb_known ? tb.num : 0);
            frame_tb_den.storeRelease(tb_known ? tb.den : 0);
        }
        stats_log_frames_lost = session->GetFramesLost();
        // Baselines for the per-second deltas of the appended columns.
        stats_base = StatsBaseline();
        stats_base.counters = session->GetNax5Counters();
        if (ChiakiFfmpegDecoder *dec = session->GetFfmpegDecoder())
            stats_base.decoder_drops = __atomic_load_n(&dec->nax5_eagain_drops, __ATOMIC_RELAXED);
        stats_base.fb_sends = session->GetFeedbackSends();
        {
            const Nax5AudioStats a = session->GetNax5AudioStats();
            stats_base.audio_dropped_bytes = a.trim_dropped_bytes + a.cap_dropped_bytes;
            stats_base.audio_starved = a.starved_polls;
        }
        stats_base.queue_generation = placebo_queue_generation.loadAcquire();
        nax5_cpu_stat(stats_base.cpu_total, stats_base.cpu_idle);
        nax5_proc_ticks(stats_base.proc_ticks);
        stats_base.udp_rcvbuf_err = nax5_udp_rcvbuf_errors();
        stats_base.elapsed_ms = 0;
        stats_acc = StatsAccum();
        {
            bool ok = false;
            const int bat = nax5_read_sysfs(QStringLiteral("/sys/class/power_supply/BAT1/capacity")).toInt(&ok);
            stats_acc.bat_pct_start = ok ? bat : -1;
        }
        stats_quit_reason.clear();
        // Everything that defines the configuration, so sessions can be compared later.
        // key=value without spaces; info (free text) stays last.
        auto env_or = [](const char *name, const char *fallback) {
            const QByteArray v = qgetenv(name);
            return QString::fromLatin1(v.isEmpty() ? QByteArray(fallback) : v);
        };
        const double refresh = screen() ? screen()->refreshRate() : 0.0;
        stats_base.screen_hz = refresh;
        const int present_mode = present_mode_actual.loadAcquire();
        stats_present_mode_header = render_backend == RenderBackend::Vulkan && present_mode < 0
            ? QStringLiteral("pending") : QString::fromLatin1(nax5_present_mode_name(present_mode));
        // Decoder details: codec_id / hw context are set at decoder open and constant afterwards.
        QString codec = QStringLiteral("-"), decoder_name = QStringLiteral("-");
        int hdr = -1;
        if (ChiakiFfmpegDecoder *dec = session->GetFfmpegDecoder()) {
            if (dec->codec_context)
                codec = QString::fromLatin1(avcodec_get_name(dec->codec_context->codec_id));
            hdr = dec->hdr_enabled ? 1 : 0;
            if (dec->hw_device_ctx)
                decoder_name = QString::fromLatin1(av_hwdevice_get_type_name(
                    reinterpret_cast<AVHWDeviceContext *>(dec->hw_device_ctx->data)->type));
            else
                decoder_name = QStringLiteral("software");
        }
        const QDateTime local_now = QDateTime::currentDateTime();
        const int tz_s = local_now.offsetFromUtc();
        const QString tz = QStringLiteral("%1%2:%3").arg(tz_s < 0 ? QLatin1Char('-') : QLatin1Char('+'))
            .arg(qAbs(tz_s) / 3600, 2, 10, QLatin1Char('0')).arg(qAbs(tz_s) % 3600 / 60, 2, 10, QLatin1Char('0'));
        QStringList fields;
        fields << QStringLiteral("host=%1").arg(nax5_redact_host(session->GetHost()))
               << QStringLiteral("session_id=%1").arg(session->GetSessionIdSuffix())
               << QStringLiteral("requested_kbps=%1").arg(session->GetRequestedBitrateKbps())
               << QStringLiteral("requested_res=%1").arg(session->GetRequestedResolution())
               << QStringLiteral("max_fps=%1").arg(session->GetMaxFps())
               << QStringLiteral("ping_ms=%1").arg(session->GetPingMs())
               << QStringLiteral("screen_hz=%1").arg(refresh, 0, 'f', 1)
               << QStringLiteral("window=%1").arg(QString::fromLatin1(QMetaEnum::fromType<VideoMode>().valueToKey(static_cast<int>(video_mode))))
               << QStringLiteral("preset=%1").arg(QString::fromLatin1(QMetaEnum::fromType<VideoPreset>().valueToKey(static_cast<int>(video_preset))))
               << QStringLiteral("vsync=%1").arg(present_vsync_enabled.loadRelaxed() ? 1 : 0)
               << QStringLiteral("present_mode=%1").arg(stats_present_mode_header)
               << QStringLiteral("backend=%1").arg(render_backend == RenderBackend::Vulkan ? QStringLiteral("vulkan") : QStringLiteral("opengl"))
               << QStringLiteral("hdr_trc=%1").arg(settings->GetDisplayTargetTrc())
               << QStringLiteral("hdr_peak=%1").arg(settings->GetDisplayTargetPeak())
               << QStringLiteral("codec=%1").arg(codec)
               << QStringLiteral("hdr=%1").arg(hdr)
               << QStringLiteral("decoder=%1").arg(decoder_name)
               << QStringLiteral("has_b_frames=%1").arg(session->GetHasBFrames())
               << QStringLiteral("low_delay=%1").arg(nax5_env_flag("NAX5_LOW_DELAY", true) ? 1 : 0)
               << QStringLiteral("audio_buffer_size=%1").arg(settings->GetAudioBufferSize())
               << QStringLiteral("audio_headroom_ms=%1").arg(env_or("NAX5_AUDIO_HEADROOM_MS", "70"))
               << QStringLiteral("reorder_us=%1").arg(env_or("NAX5_REORDER_TIMEOUT_US", "4000"))
               << QStringLiteral("immediate_render=%1").arg(immediateRenderActive() ? 1 : 0)
               << QStringLiteral("eager_snapshot=%1").arg(nax5_eager_snapshot() ? 1 : 0)
               << QStringLiteral("frame_mix=%1").arg(frameMixActive(video_preset) ? 1 : 0)
               << QStringLiteral("skip_repeat=%1").arg(skipRepeatActive() ? 1 : 0)
               << QStringLiteral("fast_chroma=%1").arg(nax5_fast_chroma() ? 1 : 0)
               << QStringLiteral("deband_grain=%1").arg(nax5_deband_grain(), 0, 'f', 1)
               << QStringLiteral("power_tune=%1").arg(env_or("NAX5_POWER_TUNE", "1"))
               << QStringLiteral("cpu_governor=%1").arg(QString::fromLatin1(nax5_read_sysfs(QStringLiteral("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"))))
               << QStringLiteral("cpu_epp=%1").arg(QString::fromLatin1(nax5_read_sysfs(QStringLiteral("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference"))))
               << QStringLiteral("gpu_level=%1").arg(QString::fromLatin1(nax5_read_sysfs(nax5_amdgpu_device_dir() + QStringLiteral("/power_dpm_force_performance_level"))))
               << QStringLiteral("start_utc=%1").arg(now.toString(Qt::ISODate))
               << QStringLiteral("tz=%1").arg(tz)
               << QStringLiteral("build=%1").arg(QString::fromLatin1(nax5_build_id()))
               // Column semantics: input_lat_ms_max is an upper bound since the previous
               // controller poll tick; console_rtt_ms is the console's raw report, not an RTT.
               << QStringLiteral("input_lat=poll_bound")
               << QStringLiteral("console_rtt=raw_not_rtt")
               // "No data this second" is an empty cell, not -1.
               << QStringLiteral("missing=empty")
               << QStringLiteral("info=%1").arg(session->GetStreamInfo());
        const QByteArray header = (QStringLiteral("# ") + fields.join(QLatin1Char(' ')) + QStringLiteral("\n")
            + QStringLiteral("time,t_s,stream_fps,display_fps,resolution,mbps_measured,mbps_target,loss_pct,frames_lost,dropped,wifi_dbm,queue_depth,catch_ups,window,preset,vsync,apu_w,bat_w,gpu_busy,gpu_mhz,apu_now_w,repeat_skips,audio_ms,")
            + QStringLiteral("fec_ok,fec_failed,units_lost,pkts_late,reorder_skips,retransmits,decoder_drops,queue_culls,queue_resets,late_presents,swap_gap_max_ms,render_ms_max,present_age_ms_avg,present_age_ms_max,console_rtt_ms,console_loss,upstream_loss_pct,kbytes_rx,idr_frames,audio_plc,audio_trim_ms,audio_starved,input_lat_ms_max,fb_sends,gui_stall_max_ms,cpu_mhz_avg,cpu_load_pct,proc_cpu_pct,apu_temp_c,wifi_temp_c,bat_pct,udp_rcvbuf_err,ping_ms,ping_last_ms,")
            + QStringLiteral("audio_skipped,skip_miss_overlay,skip_miss_key,skip_miss_invalid,skip_miss_gate,audio_starved_total,qml_updates\n")).toUtf8();
        stats_log->write(header);
        qCInfo(chiakiGui) << "Stream stats log:" << stats_log->fileName();
        if (nax5_frame_trace()) {
            frame_trace_log = new QFile(base + QStringLiteral("_frames.csv"));
            if (frame_trace_log->open(QIODevice::NewOnly | QIODevice::Text)) {
                // pts in seconds when the decoder's time base is known, else as stored.
                const bool pts_seconds = frame_tb_den.loadAcquire() > 0;
                frame_trace_pts_seconds.storeRelaxed(pts_seconds ? 1 : 0);
                frame_trace_log->write(pts_seconds
                    ? "pts,arrival_us,present_us,age_ms,render_ms,queue_depth,repeated\n"
                    : "pts_raw,arrival_us,present_us,age_ms,render_ms,queue_depth,repeated\n");
                frame_trace_log_active.storeRelease(1);
            } else {
                qCWarning(chiakiGui) << "Cannot write frame trace" << frame_trace_log->fileName();
                delete frame_trace_log;
                frame_trace_log = nullptr;
            }
            QMutexLocker locker(&frame_trace_mutex);
            frame_trace_ring.clear();
            frame_trace_ring.reserve(1024);
            frame_trace_dropped = 0;
        }
    }

    // gamescope switches the panel to the stream's refresh rate after the header was
    // written: record the change instead of rewriting the header.
    const double screen_hz = screen() ? screen()->refreshRate() : 0.0;
    if (qAbs(screen_hz - stats_base.screen_hz) > 0.05) {
        stats_base.screen_hz = screen_hz;
        stats_log->write(QStringLiteral("# note screen_hz=%1\n").arg(screen_hz, 0, 'f', 1).toUtf8());
    }
    // Panel period measured from FIFO swap intervals (render thread), once per log and
    // only while the swapchain is FIFO (the value is only measured and used then).
    if (!stats_base.panel_period_noted) {
        const int mode = present_mode_actual.loadAcquire();
        const int period_us = mode == VK_PRESENT_MODE_FIFO_KHR || mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR
            ? panel_period_us.loadRelaxed() : 0;
        if (period_us > 0) {
            stats_base.panel_period_noted = true;
            stats_log->write(QStringLiteral("# note panel_period_ms=%1\n").arg(period_us / 1000.0, 0, 'f', 2).toUtf8());
        }
    }

    // Signal level of the first wireless interface, empty on wired links.
    QString wifi_dbm;
    QFile wireless(QStringLiteral("/proc/net/wireless"));
    if (wireless.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QList<QByteArray> lines = wireless.readAll().split('\n');
        if (lines.size() > 2) {
            const QList<QByteArray> fields = lines[2].simplified().split(' ');
            if (fields.size() > 3)
                wifi_dbm = QString::fromLatin1(fields[3]).remove(QLatin1Char('.'));
        }
    }

    // Per-row counters are taken (and reset) only here, i.e. only when a row is actually
    // written; seconds skipped by the early returns above accumulate into the next row.
    const int catch_ups = catch_up_count.fetchAndStoreRelaxed(0);
    const int repeat_skips_row = repeat_skips.fetchAndStoreRelaxed(0);
    const int queue_culls = queue_culls_current.fetchAndStoreRelaxed(0);
    const int late_presents = late_presents_current.fetchAndStoreRelaxed(0);
    const double swap_gap_max_ms = swap_gap_us_max_current.fetchAndStoreRelaxed(0) / 1000.0;
    const double render_ms_max = render_us_max_current.fetchAndStoreRelaxed(0) / 1000.0;
    // Sum and count come from one atomic, so they always describe the same samples.
    const quint64 present_age_packed = present_age_packed_current.fetchAndStoreRelaxed(0);
    const int present_age_us_max = present_age_us_max_current.fetchAndStoreRelaxed(0);
    const quint64 present_age_count = present_age_packed >> nax5_age_count_shift;
    const quint64 present_age_sum_us = present_age_packed & nax5_age_sum_mask;
    const double present_age_ms_max = present_age_count ? present_age_us_max / 1000.0 : -1.0;
    const double present_age_ms_avg = present_age_count ? static_cast<double>(present_age_sum_us) / present_age_count / 1000.0 : -1.0;
    const int skip_miss_overlay = skip_miss_overlay_current.fetchAndStoreRelaxed(0);
    const int skip_miss_key = skip_miss_key_current.fetchAndStoreRelaxed(0);
    const int skip_miss_invalid = skip_miss_invalid_current.fetchAndStoreRelaxed(0);
    const int skip_miss_gate = skip_miss_gate_current.fetchAndStoreRelaxed(0);

    const int frames_lost = session->GetFramesLost();
    const int frames_lost_row = frames_lost - stats_log_frames_lost;
    const Nax5PowerSample power = nax5_power_sample();
    const qint64 elapsed_ms = stats_log_clock.elapsed();
    const double interval_s = qMax(0.001, (elapsed_ms - stats_base.elapsed_ms) / 1000.0);
    stats_base.elapsed_ms = elapsed_ms;

    // Lib counters: per-second deltas of the snapshot, "_last" values as reported.
    const ChiakiNax5Counters ctr = session->GetNax5Counters();
    const ChiakiNax5Counters &prev = stats_base.counters;
    auto d32 = [](quint32 now, quint32 before) { return static_cast<qint64>(now - before); };
    const qint64 fec_ok = d32(ctr.frames_fec_ok, prev.frames_fec_ok);
    const qint64 fec_failed = d32(ctr.frames_fec_failed, prev.frames_fec_failed);
    const qint64 units_lost = d32(ctr.units_lost, prev.units_lost);
    const qint64 pkts_late = d32(ctr.pkts_late, prev.pkts_late);
    const qint64 reorder_skips = d32(ctr.reorder_skips, prev.reorder_skips);
    const qint64 retransmits = d32(ctr.retransmits, prev.retransmits);
    const qint64 idr_frames = d32(ctr.idr_frames, prev.idr_frames);
    const qint64 audio_plc = d32(ctr.audio_plc, prev.audio_plc);
    const qint64 audio_skipped = d32(ctr.audio_skipped, prev.audio_skipped);
    const double kbytes_rx = static_cast<double>(ctr.bytes_rx - prev.bytes_rx) / 1024.0;
    // 0 = nothing reported yet: written as an empty cell.
    const double console_rtt_ms = ctr.console_rtt_us_last ? ctr.console_rtt_us_last / 1000.0 : -1.0;
    const double upstream_loss_pct = ctr.console_upstream_loss_x10000_last / 100.0;
    stats_base.counters = ctr;

    qint64 decoder_drops = 0;
    if (ChiakiFfmpegDecoder *dec = session->GetFfmpegDecoder()) {
        const quint32 drops = __atomic_load_n(&dec->nax5_eagain_drops, __ATOMIC_RELAXED);
        decoder_drops = d32(drops, stats_base.decoder_drops);
        stats_base.decoder_drops = drops;
    }
    const quint64 queue_generation = placebo_queue_generation.loadAcquire();
    const qint64 queue_resets = static_cast<qint64>(queue_generation - stats_base.queue_generation);
    stats_base.queue_generation = queue_generation;

    const Nax5AudioStats audio = session->GetNax5AudioStats();
    const quint64 audio_dropped_bytes = audio.trim_dropped_bytes + audio.cap_dropped_bytes;
    const double audio_trim_ms = static_cast<double>(audio_dropped_bytes - stats_base.audio_dropped_bytes) / nax5_audio_bytes_per_ms;
    stats_base.audio_dropped_bytes = audio_dropped_bytes;
    const qint64 audio_starved = d32(audio.starved_polls, stats_base.audio_starved);
    stats_base.audio_starved = audio.starved_polls;
    const double input_lat_ms_max = session->TakeInputLatencyMaxUs() / 1000.0;
    const quint32 fb_sends_now = session->GetFeedbackSends();
    const qint64 fb_sends = d32(fb_sends_now, stats_base.fb_sends);
    stats_base.fb_sends = fb_sends_now;
    const quint32 gui_stall_max_ms = ControllerManager::GetInstance()->TakeGuiStallMaxMs();

    // System: cheap sysfs/procfs reads, paths cached.
    QString cpu_load_pct;
    {
        quint64 total = 0, idle = 0;
        if (nax5_cpu_stat(total, idle) && total > stats_base.cpu_total) {
            const quint64 d_total = total - stats_base.cpu_total;
            const quint64 d_idle = idle - stats_base.cpu_idle;
            cpu_load_pct = QString::number(100.0 * (d_total - qMin(d_idle, d_total)) / d_total, 'f', 1);
            stats_base.cpu_total = total;
            stats_base.cpu_idle = idle;
        }
    }
    QString proc_cpu_pct;
    {
        quint64 ticks = 0;
        if (nax5_proc_ticks(ticks) && ticks >= stats_base.proc_ticks) {
            static const long clk_tck = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
            proc_cpu_pct = QString::number(100.0 * (ticks - stats_base.proc_ticks) / (clk_tck * interval_s), 'f', 1);
            stats_base.proc_ticks = ticks;
        }
    }
    QString udp_rcvbuf_err;
    {
        const qint64 errs = nax5_udp_rcvbuf_errors();
        if (errs >= 0 && stats_base.udp_rcvbuf_err >= 0)
            udp_rcvbuf_err = QString::number(errs - stats_base.udp_rcvbuf_err);
        stats_base.udp_rcvbuf_err = errs;
    }
    const QString apu_temp_c = nax5_hwmon_temp_c(nax5_amdgpu_hwmon_dir());
    const QString wifi_temp_c = nax5_hwmon_temp_c(nax5_wifi_hwmon_dir());
    const QString bat_pct = QString::fromLatin1(nax5_read_sysfs(QStringLiteral("/sys/class/power_supply/BAT1/capacity")));
    const double loss_pct = session->GetInstantPacketLoss() * 100.0;
    const int audio_ms = session->GetAudioQueuedMs();

    QStringList row;
    row << QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))
        << QString::number(elapsed_ms / 1000)
        << QString::number(stream_fps)
        << QString::number(display_fps)
        << resolution
        << QString::number(session->GetMeasuredBitrate(), 'f', 2)
        << QString::number(session->GetTargetBitrate(), 'f', 2)
        << QString::number(loss_pct, 'f', 2)
        << QString::number(frames_lost_row)
        << QString::number(dropped)
        << wifi_dbm
        << QString::number(queueDepthAverage(), 'f', 2)
        << QString::number(catch_ups)
        // Window mode and preset can be switched from the stream menu mid-session.
        << QString::fromLatin1(QMetaEnum::fromType<VideoMode>().valueToKey(static_cast<int>(video_mode)))
        << QString::fromLatin1(QMetaEnum::fromType<VideoPreset>().valueToKey(static_cast<int>(video_preset)))
        // Actual swapchain mode (FIFO = 1), not the setting.
        << QString::number(present_vsync_enabled.loadRelaxed() ? 1 : 0)
        << power.apu_w << power.bat_w << power.gpu_busy << power.gpu_mhz << power.apu_now_w
        // Presents skipped because they would repeat the last one (NAX5_SKIP_REPEAT).
        << QString::number(repeat_skips_row)
        // Audio waiting for playback (ring + SDL queue), i.e. how far sound lags.
        << QString::number(audio_ms)
        // --- appended columns (IMPL-2409) ---
        << QString::number(fec_ok)
        << QString::number(fec_failed)
        << QString::number(units_lost)
        << QString::number(pkts_late)
        << QString::number(reorder_skips)
        << QString::number(retransmits)
        << QString::number(decoder_drops)
        << QString::number(queue_culls)
        << QString::number(queue_resets)
        << QString::number(late_presents)
        << QString::number(swap_gap_max_ms, 'f', 1)
        << QString::number(render_ms_max, 'f', 1)
        // Empty cell = no data this second (no presents / nothing reported yet).
        << (present_age_count ? QString::number(present_age_ms_avg, 'f', 1) : QString())
        << (present_age_count ? QString::number(present_age_ms_max, 'f', 1) : QString())
        << (ctr.console_rtt_us_last ? QString::number(console_rtt_ms, 'f', 2) : QString())
        << QString::number(ctr.console_loss_last)
        << QString::number(upstream_loss_pct, 'f', 2)
        << QString::number(kbytes_rx, 'f', 0)
        << QString::number(idr_frames)
        << QString::number(audio_plc)
        << QString::number(audio_trim_ms, 'f', 1)
        << QString::number(audio_starved)
        << QString::number(input_lat_ms_max, 'f', 1)
        << QString::number(fb_sends)
        << QString::number(gui_stall_max_ms)
        << nax5_cpu_mhz_avg()
        << cpu_load_pct
        << proc_cpu_pct
        << apu_temp_c
        << wifi_temp_c
        << bat_pct
        << udp_rcvbuf_err
        // NAX5: live RTT from data-channel ACKs (smoothed, last sample); empty before the first sample
        << (ctr.data_rtt_samples ? QString::number(ctr.data_rtt_us_smoothed / 1000.0, 'f', 1) : QString())
        << (ctr.data_rtt_samples ? QString::number(ctr.data_rtt_us_last / 1000.0, 'f', 1) : QString())
        // --- appended columns (IMPL-2409b) ---
        << QString::number(audio_skipped)
        // Renders that repeated the last present but were not skipped, by reason.
        << QString::number(skip_miss_overlay)
        << QString::number(skip_miss_key)
        << QString::number(skip_miss_invalid)
        << QString::number(skip_miss_gate)
        << QString::number(audio.starved_polls)
        // Qt Quick scene updates (sceneChanged + renderRequested) this second.
        << QString::number(qml_updates_current.fetchAndStoreRelaxed(0));
    stats_log_frames_lost = frames_lost;
    stats_log->write((row.join(QLatin1Char(',')) + QLatin1Char('\n')).toUtf8());
    stats_log->flush();

    // Samples for the end-of-session summary.
    stats_acc.rows++;
    stats_acc.stream_fps << stream_fps;
    if (stats_acc.first_frame_ms < 0 && stream_fps > 0)
        stats_acc.first_frame_ms = elapsed_ms;
    if (stats_acc.first_frame_ms >= 0 && elapsed_ms >= stats_acc.first_frame_ms + 5000)
        stats_acc.stream_fps_steady << stream_fps;
    stats_acc.display_fps << display_fps;
    stats_acc.loss_pct << static_cast<float>(loss_pct);
    stats_acc.audio_ms << audio_ms;
    if (present_age_count)
        stats_acc.present_age_ms << static_cast<float>(present_age_ms_avg);
    if (ctr.console_rtt_us_last)
        stats_acc.console_rtt_ms << static_cast<float>(console_rtt_ms);
    if (ctr.data_rtt_samples)
        stats_acc.ping_ms << static_cast<float>(ctr.data_rtt_us_last / 1000.0);
    if (!wifi_dbm.isEmpty())
        stats_acc.wifi_dbm << wifi_dbm.toFloat();
    if (!power.apu_w.isEmpty())
        stats_acc.apu_w << power.apu_w.toFloat();
    stats_acc.frames_lost += qMax(0, frames_lost_row);
    stats_acc.fec_ok += fec_ok;
    stats_acc.fec_failed += fec_failed;
    stats_acc.decoder_drops += decoder_drops;
    stats_acc.queue_culls += queue_culls;
    stats_acc.queue_resets += queue_resets;
    stats_acc.late_presents += late_presents;
    stats_acc.catch_ups += catch_ups;
    stats_acc.repeat_skips += repeat_skips_row;
    stats_acc.audio_plc += audio_plc;
    stats_acc.audio_trim_ms += audio_trim_ms;
    stats_acc.render_ms_max = qMax(stats_acc.render_ms_max, render_ms_max);
    stats_acc.swap_gap_max_ms = qMax(stats_acc.swap_gap_max_ms, swap_gap_max_ms);
    if (!apu_temp_c.isEmpty())
        stats_acc.apu_temp_c_max = qMax(stats_acc.apu_temp_c_max, apu_temp_c.toDouble());
    if (!bat_pct.isEmpty())
        stats_acc.bat_pct_end = bat_pct.toInt();
    stats_acc.has_b_frames = session->GetHasBFrames();

    flushFrameTrace();
}

void QmlMainWindow::writeStreamStatsSummary(QFile *file, const StatsAccum &acc, qint64 duration_ms)
{
    if (!file || !acc.rows)
        return;
    QStringList kv;
    kv << QStringLiteral("duration_s=%1").arg(duration_ms / 1000)
       << QStringLiteral("rows=%1").arg(acc.rows)
       // From 5 s after the first decoded frame on: the startup ramp is not a stall.
       << QStringLiteral("stream_fps_min=%1").arg(nax5_fmt(nax5_percentile(acc.stream_fps_steady, 0.0), 0))
       << QStringLiteral("stream_fps_median=%1").arg(nax5_fmt(nax5_percentile(acc.stream_fps, 0.5), 0))
       << QStringLiteral("display_fps_median=%1").arg(nax5_fmt(nax5_percentile(acc.display_fps, 0.5), 0))
       << QStringLiteral("loss_pct_median=%1").arg(nax5_fmt(nax5_percentile(acc.loss_pct, 0.5), 2))
       << QStringLiteral("loss_pct_p95=%1").arg(nax5_fmt(nax5_percentile(acc.loss_pct, 0.95), 2))
       << QStringLiteral("loss_pct_max=%1").arg(nax5_fmt(nax5_percentile(acc.loss_pct, 1.0), 2))
       << QStringLiteral("frames_lost_total=%1").arg(acc.frames_lost)
       << QStringLiteral("fec_ok_total=%1").arg(acc.fec_ok)
       << QStringLiteral("fec_failed_total=%1").arg(acc.fec_failed)
       << QStringLiteral("decoder_drops_total=%1").arg(acc.decoder_drops)
       << QStringLiteral("queue_culls_total=%1").arg(acc.queue_culls)
       << QStringLiteral("queue_resets_total=%1").arg(acc.queue_resets)
       << QStringLiteral("late_presents_total=%1").arg(acc.late_presents)
       << QStringLiteral("catch_ups_total=%1").arg(acc.catch_ups)
       << QStringLiteral("repeat_skips_total=%1").arg(acc.repeat_skips)
       << QStringLiteral("audio_plc_total=%1").arg(acc.audio_plc)
       << QStringLiteral("audio_trim_ms_total=%1").arg(acc.audio_trim_ms, 0, 'f', 0)
       << QStringLiteral("audio_ms_median=%1").arg(nax5_fmt(nax5_percentile(acc.audio_ms, 0.5), 0))
       << QStringLiteral("audio_ms_p95=%1").arg(nax5_fmt(nax5_percentile(acc.audio_ms, 0.95), 0))
       << QStringLiteral("render_ms_max=%1").arg(acc.render_ms_max, 0, 'f', 1)
       // p95 over the per-second averages of the present age.
       << QStringLiteral("present_age_avg_ms_p95=%1").arg(nax5_fmt(nax5_percentile(acc.present_age_ms, 0.95), 1))
       << QStringLiteral("swap_gap_max_ms=%1").arg(acc.swap_gap_max_ms, 0, 'f', 1)
       << QStringLiteral("console_rtt_ms_median=%1").arg(nax5_fmt(nax5_percentile(acc.console_rtt_ms, 0.5), 2))
       << QStringLiteral("ping_ms_median=%1").arg(nax5_fmt(nax5_percentile(acc.ping_ms, 0.5), 1))
       << QStringLiteral("ping_ms_p95=%1").arg(nax5_fmt(nax5_percentile(acc.ping_ms, 0.95), 1))
       << QStringLiteral("wifi_dbm_min=%1").arg(nax5_fmt(nax5_percentile(acc.wifi_dbm, 0.0), 0))
       << QStringLiteral("wifi_dbm_median=%1").arg(nax5_fmt(nax5_percentile(acc.wifi_dbm, 0.5), 0))
       << QStringLiteral("apu_w_mean=%1").arg(acc.apu_w.isEmpty() ? QStringLiteral("-")
              : QString::number(std::accumulate(acc.apu_w.begin(), acc.apu_w.end(), 0.0) / acc.apu_w.size(), 'f', 2))
       << QStringLiteral("bat_pct_start=%1").arg(acc.bat_pct_start)
       << QStringLiteral("bat_pct_end=%1").arg(acc.bat_pct_end)
       << QStringLiteral("apu_temp_c_max=%1").arg(acc.apu_temp_c_max < 0 ? QStringLiteral("-") : QString::number(acc.apu_temp_c_max, 'f', 1))
       << QStringLiteral("has_b_frames=%1").arg(acc.has_b_frames)
       << QStringLiteral("present_mode=%1").arg(render_backend == RenderBackend::Vulkan && present_mode_actual.loadAcquire() < 0
              ? QStringLiteral("none") : QString::fromLatin1(nax5_present_mode_name(present_mode_actual.loadAcquire())))
       << QStringLiteral("quit_reason=%1").arg(stats_quit_reason.isEmpty() ? QStringLiteral("-") : stats_quit_reason);
    const QString line = kv.join(QLatin1Char(' '));
    file->write((QStringLiteral("# summary ") + line + QLatin1Char('\n')).toUtf8());
    file->flush();
    qCInfo(chiakiGui).noquote() << "Stream summary:" << line;
}

void QmlMainWindow::flushFrameTrace()
{
    if (!frame_trace_log)
        return;
    QVector<FrameTraceEntry> entries;
    int dropped = 0;
    {
        QMutexLocker locker(&frame_trace_mutex);
        entries.swap(frame_trace_ring);
        frame_trace_ring.reserve(1024);
        dropped = frame_trace_dropped;
        frame_trace_dropped = 0;
    }
    QString out;
    const int pts_decimals = frame_trace_pts_seconds.loadRelaxed() ? 6 : 0;
    for (const FrameTraceEntry &e : entries) {
        const double age_ms = e.arrival_us && e.present_us > e.arrival_us
            ? static_cast<double>(e.present_us - e.arrival_us) / 1000.0 : -1.0;
        out += QStringLiteral("%1,%2,%3,%4,%5,%6,%7\n")
            .arg(e.pts, 0, 'f', pts_decimals).arg(e.arrival_us).arg(e.present_us)
            .arg(age_ms, 0, 'f', 2).arg(e.render_ms, 0, 'f', 2).arg(e.queue_depth).arg(e.repeated);
    }
    if (dropped)
        out += QStringLiteral("# note trace_entries_dropped=%1\n").arg(dropped);
    if (!out.isEmpty()) {
        frame_trace_log->write(out.toUtf8());
        frame_trace_log->flush();
    }
}

void QmlMainWindow::closeStreamStatsLog()
{
    if (frame_trace_log) {
        frame_trace_log_active.storeRelease(0);
        flushFrameTrace();
        frame_trace_log->close();
        delete frame_trace_log;
        frame_trace_log = nullptr;
    }
    if (!stats_log)
        return;
    stats_log->close();
    delete stats_log;
    stats_log = nullptr;
}

void QmlMainWindow::saveShaderCache()
{
    if (!placebo_cache)
        return;
    // Unchanged since the last save (or the load at startup): nothing to write.
    const uint64_t signature = pl_cache_signature(placebo_cache);
    if (signature == shader_cache_saved_signature)
        return;
    // pl_cache locks internally, so saving while the render thread adds shaders is safe.
    // QSaveFile writes a temporary file and renames it: a crash mid-write keeps the old cache.
    const QString path = shader_cache_path();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qCWarning(chiakiGui) << "Cannot write shader cache" << path;
        return;
    }
    const int objects = pl_cache_save_ex(placebo_cache, [](void *priv, size_t size, const void *ptr) {
        static_cast<QSaveFile *>(priv)->write(static_cast<const char *>(ptr), static_cast<qint64>(size));
    }, &file);
    if (!file.commit()) {
        qCWarning(chiakiGui) << "Failed to save shader cache" << path;
        return;
    }
    shader_cache_saved_signature = signature;
    qCInfo(chiakiGui) << "Saved shader cache:" << objects << "objects";
}

int QmlMainWindow::droppedFrames() const
{
    return dropped_frames;
}

void QmlMainWindow::increaseDroppedFrames()
{
    dropped_frames_current.fetchAndAddRelaxed(1);
}

void QmlMainWindow::increaseQueueCulls()
{
    dropped_frames_current.fetchAndAddRelaxed(1);
    queue_culls_current.fetchAndAddRelaxed(1);
}

bool QmlMainWindow::keepVideo() const
{
    return keep_video;
}

QmlBackend * QmlMainWindow::getBackend()
{
    return backend;
}

void QmlMainWindow::setKeepVideo(bool keep)
{
    keep_video = keep;
    emit keepVideoChanged();
}

void QmlMainWindow::grabInput()
{
    setCursor(Qt::ArrowCursor);
    grab_input++;
    if (session)
        session->BlockInput(grab_input);
}

void QmlMainWindow::releaseInput()
{
    if (!grab_input)
        return;
    grab_input--;
    if (!grab_input && has_video && settings->GetHideCursor())
        setCursor(Qt::BlankCursor);
    if (session)
        session->BlockInput(grab_input);
}

bool QmlMainWindow::directStream() const
{
    return direct_stream;
}

QmlMainWindow::VideoMode QmlMainWindow::videoMode() const
{
    return video_mode;
}

void QmlMainWindow::setVideoMode(VideoMode mode)
{
    video_mode = mode;
    emit videoModeChanged();
    // With NAX5_SKIP_REPEAT only the slow heartbeat may be armed; show the change now.
    if (update_timer)
        scheduleUpdate();
}

float QmlMainWindow::zoomFactor() const
{
    return zoom_factor;
}

void QmlMainWindow::setZoomFactor(float factor)
{
    zoom_factor = factor;
    emit zoomFactorChanged();
    if (update_timer)
        scheduleUpdate();
}

QmlMainWindow::VideoPreset QmlMainWindow::videoPreset() const
{
    return video_preset;
}

void QmlMainWindow::setVideoPreset(VideoPreset preset)
{
    video_preset = preset;
    emit videoPresetChanged();
    if (update_timer)
        scheduleUpdate();
}

void QmlMainWindow::setSettings(Settings *new_settings)
{
    settings = new_settings;
    QString profile = settings->GetCurrentProfile();
    qCCritical(chiakiGui) << "Current Profile: " << profile;
    if(profile.isEmpty())
        QGuiApplication::setApplicationDisplayName("NAX5");
    else
        QGuiApplication::setApplicationDisplayName(QString("NAX5:%1").arg(profile));
    this->setTitle(QGuiApplication::applicationDisplayName());
}

void QmlMainWindow::show()
{
    QQmlComponent component(qml_engine, QUrl(QStringLiteral("qrc:/Main.qml")));
    if (!component.isReady()) {
        qCCritical(chiakiGui) << "Component not ready\n" << component.errors();
        QMetaObject::invokeMethod(QGuiApplication::instance(), &QGuiApplication::quit, Qt::QueuedConnection);
        return;
    }

    QVariantMap props;
    props[QStringLiteral("parent")] = QVariant::fromValue(quick_window->contentItem());
    quick_item = qobject_cast<QQuickItem*>(component.createWithInitialProperties(props));
    if (!quick_item) {
        qCCritical(chiakiGui) << "Failed to create root item\n" << component.errors();
        QMetaObject::invokeMethod(QGuiApplication::instance(), &QGuiApplication::quit, Qt::QueuedConnection);
        return;
    }
    if (!pending_renderer_fallback_reason.isEmpty()) {
        const QString reason = pending_renderer_fallback_reason;
        pending_renderer_fallback_reason.clear();
        QMetaObject::invokeMethod(quick_item, "showRendererFallbackDialog",
                                  Q_ARG(QVariant, QVariant::fromValue(reason)));
    }
    auto screen_size = QGuiApplication::primaryScreen()->availableSize();
    resize(screen_size);

    if (qEnvironmentVariable("XDG_CURRENT_DESKTOP") == "gamescope")
        fullscreenTime();
    else
    {
        if(!settings->GetGeometry().isEmpty())
        {
            setGeometry(settings->GetGeometry());
            showNormal();
        }
        else
            showMaximized();
        setWindowAdjustable(true);
    }
}

namespace {
constexpr int kDefaultQueueDepthLimit = 2;
constexpr int kAdaptiveQueueDepthBoost = 1;
}

void QmlMainWindow::presentFrame(ChiakiFfmpegFrame frame, int32_t frames_lost)
{
    // Decoder losses are network loss (frames_lost column) and EAGAIN drops
    // (decoder_drops column): "dropped" counts only pl_queue culls and pending-frame
    // replacements.
    Q_UNUSED(frames_lost);

    if (!frame.frame)
        return;
    // Queued from the frame thread: the session may have ended (or a new one started)
    // before this ran, then the frame belongs to nobody.
    if (!backend || !backend->streamActive()) {
        av_frame_free(&frame.frame);
        return;
    }

    stream_frames_current.fetchAndAddRelaxed(1);
    stream_frame_width.storeRelaxed(frame.frame->width);
    stream_frame_height.storeRelaxed(frame.frame->height);

    if (frame.recovered)
    {
        const bool stored_pending = storePendingFrame(frame, true);
        if (stored_pending)
            snapshotPendingFrame();
        if (frame.frame)
            av_frame_free(&frame.frame);
        bool need_reset = false;
        {
            QMutexLocker locker(&placebo_state_mutex);
            double current_pts = queue_pts_origin;
            if (queue_pts_origin >= 0.0 && playback_started && ts_start) {
                const uint64_t now_us = chiaki_time_now_monotonic_us();
                current_pts += static_cast<double>(now_us - ts_start) / 1000000.0;
            }
            need_reset = queue_pts_origin < 0.0 || frame.pts + 1e-6 < current_pts;
        }
        if (need_reset && placebo_reset_pending.loadAcquire() == 0) {
            qCInfo(chiakiGui)
                << "Queue reset requested for recovered frame"
                << "frame_pts=" << frame.pts
                << "preserve_timeline=true";
            queuePlaceboReset(true);
        }
        if (need_reset)
            storeResetSeedFromPendingFrame(reset_seed_capture_generation.loadAcquire());
        return;
    }

    frame.frame->opaque = this;

    // A deferred reset is waiting for the render thread: nothing may enter the queue
    // before it (it would be wiped, or be drawn against the old origin). Park the frame;
    // the render thread resets and then pushes it (see performHardReset).
    if (hard_reset_pending.loadAcquire() != HardResetNone) {
        parkFrameForHardReset(frame);
        requestFrameRender();
        return;
    }

    bool deinterlace_enabled = false;
    bool need_rollback_reset = false;
    {
        QMutexLocker locker(&placebo_state_mutex);
        need_rollback_reset = queue_pts_origin < 0.0 || frame.pts + 1e-6 < queue_pts_origin;
        deinterlace_enabled = this->renderparams_opts->params.deinterlace_params;
    }
    if (need_rollback_reset) {
        // pl_queue_reset frees the frames the render thread may be drawing from
        // (frame_mix): wait for it -- but not through a whole render, which holds
        // frame_mix_mutex across start_frame/swap_buffers (GPU waits, shader compiles
        // of several hundred ms). If it isn't free within 50 ms, hand the reset to the
        // render thread: park this frame as the pending frame and set the flag that
        // render() handles before its next pl_queue_update. Lock order frame_mix_mutex
        // -> placebo_state_mutex, hence the second look at the condition.
        if (!frame_mix_mutex.tryLock(50)) {
            qCInfo(chiakiGui)
                << "Queue reset in presentFrame due to pts rollback deferred to the render thread"
                << "frame_pts=" << frame.pts;
            hard_reset_pending.storeRelease(HardResetRollback);
            parkFrameForHardReset(frame);
            requestFrameRender();
            return;
        }
        std::unique_lock<QRecursiveMutex> mix_locker(frame_mix_mutex, std::adopt_lock);
        QMutexLocker locker(&placebo_state_mutex);
        if (queue_pts_origin < 0.0 || frame.pts + 1e-6 < queue_pts_origin) {
            qCInfo(chiakiGui)
                << "Queue reset in presentFrame due to pts rollback"
                << "frame_pts=" << frame.pts
                << "queue_pts_origin=" << queue_pts_origin;
            pl_queue_reset(placebo_queue);
            placebo_queue_generation.fetchAndAddRelease(1);
            renderer_cache_flush_pending.storeRelaxed(1);
            ts_start = 0;
            queue_pts_origin = frame.pts;
            newest_queued_frame_pts = -1.0;
            playback_started = false;
        }
    }

    struct pl_source_frame src_frame{
        .pts = frame.pts - queue_pts_origin,
        .duration = static_cast<float>(frame.duration),
        // allow soft-disabling deinterlacing at the source frame level
        .first_field = deinterlace_enabled
            ? pl_field_from_avframe(frame.frame)
                : PL_FIELD_NONE,
        .frame_data = frame.frame,
        .map = map_frame,
        .unmap = unmap_frame,
        .discard = discard_frame,
    };
    const int depth_limit = effectiveQueueDepthLimit();
    bool queued = false;
    bool depth_exceeded = false;
    {
        QMutexLocker locker(&placebo_state_mutex);
        const int depth = pl_queue_num_frames(placebo_queue);
        depth_exceeded = depth >= depth_limit;
        updateQueueDepthAverage(depth);
        if (!depth_exceeded)
        {
            pl_queue_push(placebo_queue, &src_frame);
            newest_queued_frame_pts = qMax(newest_queued_frame_pts, frame.pts);
            // Same critical section as the push: render() never sees the new frame
            // against the old clock.
            anchorPlaybackLocked(src_frame.pts, frame.duration);
            queued = true;
        }
    }
    if (!queued)
    {
        const quint64 reset_seed_generation = reset_seed_capture_generation.loadAcquire();
        if (reset_seed_capture_active.loadAcquire() != 0 && reset_seed_generation != 0)
            storeResetSeedFrame(frame.frame, frame.pts, frame.duration, reset_seed_generation);
        if (storePendingFrame(frame, true))
            snapshotPendingFrame();
        if (frame.frame)
            av_frame_free(&frame.frame);
        requestFrameRender();
        return;
    }

    bool dropped_stale_pending = false;
    {
        QMutexLocker locker(&pending_frame_mutex);
        if (pending_frame && pending_pts <= frame.pts + 1e-6) {
            qCDebug(chiakiGui)
                << "Dropping stale pending frame after newer direct queue submission"
                << "pending_pts=" << pending_pts
                << "queued_pts=" << frame.pts;
            av_frame_free(&pending_frame);
            dropped_frames_current.fetchAndAddRelaxed(1);
            pending_pts = 0.0;
            pending_duration = 0.0f;
            pending_frame_queue_origin = 0.0;
            pending_frame_stored_us = 0;
            pending_frame_present.storeRelease(0);
            dropped_stale_pending = true;
        }
    }
    if (dropped_stale_pending)
        updatePendingFrameAge(0.0);

    {
        const quint64 reset_seed_generation = reset_seed_capture_generation.loadAcquire();
        if (reset_seed_capture_active.loadAcquire() != 0 && reset_seed_generation != 0)
            storeResetSeedFrame(frame.frame, frame.pts, frame.duration, reset_seed_generation);
    }

    snapshotLastFrame(frame.frame, frame.pts, frame.duration);

    if (!has_video) {
        has_video = true;
        if (!grab_input && settings->GetHideCursor())
            setCursor(Qt::BlankCursor);
        emit hasVideoChanged();
    }
    requestFrameRender();
}

void QmlMainWindow::requestTerminate()
{
    // Steam and the launcher script may both send TERM; a second one must not call
    // quit() in the middle of the close (that path spins at 100% CPU until killed).
    if (terminating)
        return;
    terminating = true;
    // Safety net in case the session never reports that it stopped. Settings are
    // written as they change, so a hard exit loses nothing.
    QTimer::singleShot(6000, this, []() {
        qCWarning(chiakiGui) << "Graceful shutdown timed out, exiting";
        ::_exit(0);
    });
    if (backend && backend->qmlSession()) {
        connect(backend, &QmlBackend::sessionChanged, this, [this](StreamSession *s) {
            if (!s)
                QMetaObject::invokeMethod(this, &QWindow::close, Qt::QueuedConnection);
        });
        backend->stopSession(false);
        return;
    }
    close();
}

bool QmlMainWindow::immediateRenderActive() const
{
    // With OpenGL the render thread is the GUI thread and update() would render
    // synchronously inside presentFrame, so keep the timer there.
    return nax5_immediate_render() && render_backend == RenderBackend::Vulkan;
}

bool QmlMainWindow::frameMixActive(VideoPreset preset) const
{
    // Custom presets keep whatever frame mixer the user configured.
    return nax5_frame_mix() || !immediateRenderActive() || preset == VideoPreset::Custom;
}

bool QmlMainWindow::skipRepeatActive() const
{
    return nax5_skip_repeat() && immediateRenderActive();
}

void QmlMainWindow::requestFrameRender()
{
    // Render right away instead of waiting up to a refresh interval for the timer;
    // update() merges with a render already in flight.
    if (immediateRenderActive())
        update();
    else
        scheduleUpdate();
}

// placebo_state_mutex must be held.
void QmlMainWindow::anchorPlaybackLocked(double src_pts, float duration)
{
    const uint64_t now_us = chiaki_time_now_monotonic_us();
    const uint64_t frame_pts_us = src_pts > 0.0 ? static_cast<uint64_t>(src_pts * 1000000.0) : 0;
    auto anchor = [&]() { ts_start = frame_pts_us < now_us ? now_us - frame_pts_us : 0; };
    if (!playback_started) {
        anchor();
        playback_started = true;
        return;
    }
    if (!ts_start)
        return;
    const double current_pts = static_cast<double>(now_us - ts_start) / 1000000.0;
    if (immediateRenderActive()) {
        // The timeline is anchored to the first frame. If that one arrived late, every
        // later frame waits in the queue until its pts comes up: latency that never goes
        // away. render() shows frames up to clock + one refresh interval (0 without
        // vsync), so anything beyond that would wait -- move the clock to it. The clock
        // only moves forward, and late frames are shown right away anyway.
        if (src_pts > current_pts + present_window_s) {
            // Re-anchor always; count it only when the jump is more than half a frame
            // (smaller ones are timer jitter, not a late first frame).
            if (src_pts - current_pts > static_cast<double>(duration) / 2.0)
                catch_up_count.fetchAndAddRelaxed(1);
            anchor();
        }
    } else {
        const double catch_up_threshold = qMax(static_cast<double>(duration) * 2.0, 0.05);
        if (src_pts > current_pts + catch_up_threshold)
            anchor();
    }
}

void QmlMainWindow::resetPlaceboQueue()
{
    uint64_t now_ms = chiaki_time_now_monotonic_ms();
    const bool preserve_timeline = placebo_reset_preserve_timeline.loadAcquire() != 0;
    if (!preserve_timeline && last_placebo_reset_ts && now_ms - last_placebo_reset_ts < 100)
    {
        const int wait_ms = static_cast<int>(qMax<uint64_t>(100 - (now_ms - last_placebo_reset_ts), 1));
        const quint64 generation = placebo_reset_throttle_generation.fetchAndAddRelaxed(1) + 1;
        qCInfo(chiakiGui)
            << "Queue reset throttled"
            << "wait_ms=" << wait_ms
            << "preserve_timeline=" << preserve_timeline;
        placebo_reset_pending.storeRelease(0);
        QTimer::singleShot(wait_ms, this, [this, generation, preserve_timeline]() {
            if (placebo_reset_throttle_generation.loadAcquire() != generation)
                return;
            queuePlaceboReset(preserve_timeline);
        });
        return;
    }
    last_placebo_reset_ts = now_ms;
    placebo_reset_throttle_generation.fetchAndAddRelaxed(1);
    placebo_reset_preserve_timeline.storeRelease(0);

    // Lock order: frame_mix_mutex -> placebo_state_mutex (see the header).
    QMutexLocker mix_locker(&frame_mix_mutex);
    QMutexLocker locker(&placebo_state_mutex);
    qCInfo(chiakiGui)
        << "Queue reset executing"
        << "preserve_timeline=" << preserve_timeline
        << "queue_pts_origin=" << queue_pts_origin
        << "playback_started=" << playback_started;
    pl_queue_reset(placebo_queue);
    placebo_queue_generation.fetchAndAddRelease(1);
    newest_queued_frame_pts = -1.0;
    renderer_cache_flush_pending.storeRelaxed(1);
    const bool keep_timeline = preserve_timeline && queue_pts_origin >= 0.0 && playback_started;
    preserve_playback_timeline = keep_timeline;
    if (!keep_timeline) {
        queue_pts_origin = -1.0;
        playback_started = false;
        ts_start = 0;
        preserve_playback_timeline = false;
    }
    locker.unlock();
    mix_locker.unlock();
    const quint64 generation = pending_reset_snapshot_generation.loadRelaxed();
    last_reset_snapshot_generation.storeRelaxed(generation);
    const bool had_pending = hasPendingFrame();
    const quint64 reset_generation = reset_seed_capture_generation.loadAcquire();
    reset_seed_capture_active.storeRelease(0);
    reset_seed_capture_generation.storeRelease(0);
    bool seeded = applyResetSeedFrame(reset_generation);
    if (!seeded && had_pending)
        applyPendingFrame();
    if (!seeded && !had_pending)
        applyKeptFrameSnapshot();
}

void QmlMainWindow::schedulePlaceboReset()
{
    queuePlaceboReset(false);
}

void QmlMainWindow::queuePlaceboReset(bool preserve_timeline)
{
    qCInfo(chiakiGui)
        << "Queue reset scheduled"
        << "preserve_timeline=" << preserve_timeline;
    if (preserve_timeline) {
        placebo_reset_preserve_timeline.storeRelease(1);
    } else if (placebo_reset_preserve_timeline.loadRelaxed() == 0) {
        placebo_reset_preserve_timeline.storeRelease(0);
    }
    quint64 generation = reset_seed_capture_generation.loadAcquire();
    if (!generation)
        generation = placebo_reset_throttle_generation.loadRelaxed() + 1;
    reset_seed_capture_generation.storeRelease(generation);
    reset_seed_capture_active.storeRelease(1);
    placebo_reset_pending.storeRelease(1);
    QMetaObject::invokeMethod(this, [this]() { scheduleUpdate(); }, Qt::QueuedConnection);
}

double QmlMainWindow::queueDepthAverage() const
{
    return queue_depth_average.load(std::memory_order_relaxed);
}

double QmlMainWindow::pendingFrameAge() const
{
    QMutexLocker locker(&pending_frame_age_mutex);
    return pending_frame_age;
}

void QmlMainWindow::updateQueueDepthAverage(int depth)
{
    const double alpha = 0.15;
    // Only presentFrame (GUI thread, under placebo_state_mutex) writes this.
    double next = queue_depth_average.load(std::memory_order_relaxed) * (1.0 - alpha) + static_cast<double>(depth) * alpha;
    queue_depth_average.store(next, std::memory_order_relaxed);
    // QML is notified by overlay_values_timer (<= 4 Hz, only when the shown value changes).
}

void QmlMainWindow::updatePendingFrameAge(double age)
{
    double clamped = qMax(age, 0.0);
    {
        QMutexLocker locker(&pending_frame_age_mutex);
        pending_frame_age = clamped;
    }
    // Called on every render (render thread): QML is notified by overlay_values_timer instead.
}

bool QmlMainWindow::storePendingFrame(ChiakiFfmpegFrame &frame, bool take_ownership)
{
    if (!frame.frame)
        return false;

    {
        QMutexLocker locker(&pending_frame_mutex);
        // Keep the freshest queued-backlog frame and ignore stale arrivals.
        if (pending_frame && frame.pts <= pending_pts + 1e-6)
            return false;
    }

    AVFrame *stored_frame = take_ownership ? frame.frame : av_frame_clone(frame.frame);
    if (!stored_frame)
        return false;

    double queue_origin = 0.0;
    {
        QMutexLocker state_locker(&placebo_state_mutex);
        queue_origin = queue_pts_origin;
    }

    {
        QMutexLocker locker(&pending_frame_mutex);
        if (pending_frame && frame.pts <= pending_pts + 1e-6)
        {
            if (!take_ownership)
                av_frame_free(&stored_frame);
            return false;
        }
        if (pending_frame) {
            av_frame_free(&pending_frame);
            dropped_frames_current.fetchAndAddRelaxed(1);
        }
        pending_frame = stored_frame;
        pending_pts = frame.pts;
        pending_duration = frame.duration;
        pending_frame_queue_origin = queue_origin;
        pending_frame_stored_us = chiaki_time_now_monotonic_us();
        pending_frame_present.storeRelease(1);
    }

    if (take_ownership)
        frame.frame = nullptr;

    refreshPendingFrameAge();
    return true;
}

void QmlMainWindow::parkFrameForHardReset(ChiakiFfmpegFrame &frame)
{
    // GUI thread, while hard_reset_pending is set. A pending frame from before a pts
    // rollback (or from the previous session) has the higher pts and would make
    // storePendingFrame ignore the incoming one as stale: it belongs to the old timeline.
    bool dropped_old = false;
    {
        QMutexLocker locker(&pending_frame_mutex);
        if (pending_frame && pending_pts > frame.pts + 1e-6) {
            av_frame_free(&pending_frame);
            dropped_frames_current.fetchAndAddRelaxed(1);
            pending_pts = 0.0;
            pending_duration = 0.0f;
            pending_frame_queue_origin = 0.0;
            pending_frame_stored_us = 0;
            pending_frame_present.storeRelease(0);
            dropped_old = true;
        }
    }
    // Same bookkeeping as a frame that found the queue full (presentFrame).
    const quint64 reset_seed_generation = reset_seed_capture_generation.loadAcquire();
    if (reset_seed_capture_active.loadAcquire() != 0 && reset_seed_generation != 0)
        storeResetSeedFrame(frame.frame, frame.pts, frame.duration, reset_seed_generation);
    if (storePendingFrame(frame, true))
        snapshotPendingFrame();
    else if (dropped_old)
        refreshPendingFrameAge();
    if (frame.frame)
        av_frame_free(&frame.frame);
}

void QmlMainWindow::performHardReset()
{
    Q_ASSERT(QThread::currentThread() == render_thread);
    // Deferred pl_queue_reset from the GUI thread (presentFrame pts rollback, session
    // change), which could not get frame_mix_mutex within 50 ms because a render held it.
    //
    // Ordering: the GUI thread sets hard_reset_pending without frame_mix_mutex (after a
    // failed tryLock) and clears it only while holding frame_mix_mutex, having done an
    // equivalent reset itself (session change). Otherwise this function is the only
    // place that clears it, under both locks, after the reset. While it is
    // set, presentFrame never pushes into the queue (it parks frames as the pending
    // frame) and applyPendingFrame does not drain the pending frame, so nothing queued
    // after the rollback/teardown can be wiped by this late reset. render() calls this
    // before its first pl_queue_update, and the render that was running when the flag
    // was set kept its frame_mix entries valid because no reset happened under it. So
    // the sequence is the same as the synchronous path: reset, then the new frame is
    // pushed with queue_pts_origin = its pts and a fresh playback anchor.
    int done = HardResetNone;
    for (;;) {
        const int kind = hard_reset_pending.loadAcquire();
        if (kind == HardResetNone)
            break;
        // Rollback: the parked frame's pts becomes the origin, as presentFrame sets it.
        double origin = -1.0;
        if (kind == HardResetRollback) {
            QMutexLocker locker(&pending_frame_mutex);
            if (pending_frame)
                origin = pending_pts;
        }
        QMutexLocker mix_locker(&frame_mix_mutex);
        QMutexLocker locker(&placebo_state_mutex);
        // Re-read under the locks: the GUI thread may have done the reset itself (it
        // clears the flag only while holding frame_mix_mutex) or upgraded a rollback
        // to a session change (without the lock, after a failed tryLock).
        if (hard_reset_pending.loadAcquire() != kind)
            continue;
        qCInfo(chiakiGui)
            << "Deferred queue reset executing"
            << "reason=" << (kind == HardResetRollback ? "pts_rollback" : "session_change")
            << "queue_pts_origin=" << origin;
        pl_queue_reset(placebo_queue);
        placebo_queue_generation.fetchAndAddRelease(1);
        renderer_cache_flush_pending.storeRelaxed(1);
        newest_queued_frame_pts = -1.0;
        queue_pts_origin = origin;
        playback_started = false;
        ts_start = 0;
        if (hard_reset_pending.testAndSetOrdered(kind, HardResetNone)) {
            done = kind;
            break;
        }
    }
    // Push the parked frame (after the reset, with the flag clear), as presentFrame
    // would have right after its own reset.
    if (done == HardResetRollback)
        applyPendingFrame();
}

bool QmlMainWindow::storeResetSeedFrame(const AVFrame *frame, double pts, float duration, quint64 generation)
{
    if (!frame || generation == 0)
        return false;

    AVFrame *clone = av_frame_clone(frame);
    if (!clone)
        return false;

    {
        QMutexLocker locker(&reset_seed_mutex);
        if (reset_seed_frame && reset_seed_generation != generation) {
            av_frame_free(&reset_seed_frame);
            reset_seed_frame = nullptr;
            reset_seed_pts = 0.0;
            reset_seed_duration = 0.0f;
            reset_seed_generation = 0;
        }
        if (reset_seed_frame && pts <= reset_seed_pts + 1e-6) {
            av_frame_free(&clone);
            return false;
        }
        if (reset_seed_frame)
            av_frame_free(&reset_seed_frame);
        reset_seed_frame = clone;
        reset_seed_pts = pts;
        reset_seed_duration = duration;
        reset_seed_generation = generation;
    }

    return true;
}

bool QmlMainWindow::storeResetSeedFromPendingFrame(quint64 generation)
{
    if (generation == 0)
        return false;

    AVFrame *clone = nullptr;
    double pts = 0.0;
    float duration = 0.0f;
    {
        QMutexLocker locker(&pending_frame_mutex);
        if (!pending_frame)
            return false;
        clone = av_frame_clone(pending_frame);
        if (!clone)
            return false;
        pts = pending_pts;
        duration = pending_duration;
    }

    {
        QMutexLocker locker(&reset_seed_mutex);
        if (reset_seed_frame && reset_seed_generation != generation) {
            av_frame_free(&reset_seed_frame);
            reset_seed_frame = nullptr;
            reset_seed_pts = 0.0;
            reset_seed_duration = 0.0f;
            reset_seed_generation = 0;
        }
        if (reset_seed_frame && pts <= reset_seed_pts + 1e-6) {
            av_frame_free(&clone);
            return false;
        }
        if (reset_seed_frame)
            av_frame_free(&reset_seed_frame);
        reset_seed_frame = clone;
        reset_seed_pts = pts;
        reset_seed_duration = duration;
        reset_seed_generation = generation;
    }

    return true;
}

void QmlMainWindow::snapshotPendingFrame()
{
    AVFrame *snapshot_frame = nullptr;
    double pts = 0.0;
    float duration = 0.0f;
    {
        QMutexLocker locker(&pending_frame_mutex);
        if (!pending_frame)
            return;
        snapshot_frame = av_frame_clone(pending_frame);
        if (!snapshot_frame)
            return;
        pts = pending_pts;
        duration = pending_duration;
    }
    snapshotLastFrame(snapshot_frame, pts, duration, true);
}

void QmlMainWindow::refreshPendingFrameAge()
{
    uint64_t stored_us = 0;
    {
        QMutexLocker locker(&pending_frame_mutex);
        if (!pending_frame) {
            updatePendingFrameAge(0.0);
            return;
        }
        stored_us = pending_frame_stored_us;
    }

    double age = 0.0;
    if (stored_us) {
        const uint64_t now_us = chiaki_time_now_monotonic_us();
        if (now_us > stored_us)
            age = static_cast<double>(now_us - stored_us) / 1000000.0;
    }

    updatePendingFrameAge(age);
}

void QmlMainWindow::snapshotLastFrame(AVFrame *frame, double pts, float duration, bool take_ownership)
{
    // qCInfo(chiakiGui) << "snapshotLastFrame invoked pts=" << pts << " duration=" << duration;
    bool stored = false;
    const char *reason = "unknown";
    auto log_result = [&]() {
        // qCInfo(chiakiGui) << "snapshotLastFrame result stored=" << stored << " reason=" << reason;
    };
    auto release_owned_frame = [&]() {
        if (take_ownership && frame)
            av_frame_free(&frame);
    };

    if (!frame) {
        reason = "null frame";
        log_result();
        return;
    }
    const bool is_hw = frame->hw_frames_ctx != nullptr;
    if (!is_hw && !frame->data[0]) {
        reason = "missing data";
        release_owned_frame();
        log_result();
        return;
    }
    if (!is_hw && av_pix_fmt_count_planes(static_cast<AVPixelFormat>(frame->format)) <= 0) {
        reason = "no planes";
        release_owned_frame();
        log_result();
        return;
    }

    AVFrame *clone = take_ownership ? frame : av_frame_clone(frame);
    if (!clone) {
        reason = "clone failure";
        release_owned_frame();
        log_result();
        return;
    }

    if (!take_ownership) {
        clone->format = frame->format;
        clone->width = frame->width;
        clone->height = frame->height;
        clone->sample_aspect_ratio = frame->sample_aspect_ratio;
        clone->colorspace = frame->colorspace;
        clone->color_range = frame->color_range;
        clone->color_primaries = frame->color_primaries;
        clone->color_trc = frame->color_trc;
        av_frame_copy_props(clone, frame);
        if (!is_hw && clone->hw_frames_ctx) {
            av_buffer_unref(&clone->hw_frames_ctx);
            clone->hw_frames_ctx = nullptr;
        }
    }
    clone->opaque = this;
    // The RAM copy is only used to replay the last frame when the stream stalls, so by
    // default it is made lazily in keptReplaySourceLocked() instead of for every frame
    // (a 720p P010 frame is ~2.8 MB, i.e. ~165 MB/s read back from VRAM at 60 fps).
    AVFrame *fallback_clone = nax5_eager_snapshot() ? make_fallback_snapshot_frame(frame) : nullptr;
    {
        QMutexLocker locker(&kept_frame_mutex);
        if (kept_frame)
            av_frame_free(&kept_frame);
        if (fallback_frame)
            av_frame_free(&fallback_frame);
        kept_frame = clone;
        kept_frame_pts = pts;
        kept_frame_duration = duration;
        stored = true;
        reason = "stored";
        fallback_frame = fallback_clone;
        // Bumped under the lock so prepareReplayFallback() can tell a copy is stale.
        snapshot_generation.fetchAndAddRelaxed(1);
        if (nax5_eager_snapshot() && !fallback_clone && frame->hw_frames_ctx && frame->format != AV_PIX_FMT_VULKAN)
            qCWarning(chiakiGui) << "snapshotLastFrame: failed to create fallback frame";
    }
    // qCInfo(chiakiGui) << "snapshotLastFrame: stored kept_frame pts=" << pts << " duration=" << duration;
    log_result();
}

AVFrame *QmlMainWindow::keptReplaySourceLocked()
{
    return fallback_frame ? fallback_frame : kept_frame;
}

void QmlMainWindow::prepareReplayFallback()
{
    // Copies the kept hardware frame to RAM without holding any lock during the copy;
    // the result is dropped if a newer frame was kept meanwhile. Only the queue-reset
    // path (applyKeptFrameSnapshot) prepares it: pl_queue keeps showing its last frame
    // during a normal stall, and the empty-queue replay in render() uses the hardware
    // frame itself, as upstream does for Vulkan frames.
    AVFrame *source = nullptr;
    quint64 generation = 0;
    {
        QMutexLocker locker(&kept_frame_mutex);
        if (fallback_frame || !kept_frame || fallback_attempt_generation == snapshot_generation.loadRelaxed())
            return;
        generation = snapshot_generation.loadRelaxed();
        fallback_attempt_generation = generation;
        source = av_frame_clone(kept_frame);
    }
    if (!source)
        return;
    AVFrame *copy = make_fallback_snapshot_frame(source);
    av_frame_free(&source);
    if (!copy)
        return;
    QMutexLocker locker(&kept_frame_mutex);
    if (!fallback_frame && kept_frame && snapshot_generation.loadRelaxed() == generation)
        fallback_frame = copy;
    else
        av_frame_free(&copy);
}

void QmlMainWindow::clearSnapshotFrame()
{
    QMutexLocker locker(&kept_frame_mutex);
    if (kept_frame)
        av_frame_free(&kept_frame);
    if (fallback_frame)
        av_frame_free(&fallback_frame);
    kept_frame = nullptr;
    fallback_frame = nullptr;
    fallback_attempt_generation = ~0ULL;
}

bool QmlMainWindow::enqueueKeptFrame(double queue_pts_origin_hint, bool deinterlace_enabled, double &used_origin)
{
    AVFrame *clone = nullptr;
    double pts = 0.0;
    float duration = 0.0f;
    {
        QMutexLocker locker(&kept_frame_mutex);
        AVFrame *source = keptReplaySourceLocked();
        if (!source)
            return false;
        clone = clone_frame_for_replay(source);
        if (!clone)
            return false;
        pts = kept_frame_pts;
        duration = kept_frame_duration;
    }

    clone->opaque = this;
    double origin = queue_pts_origin_hint;
    if (origin < 0.0)
        origin = pts;
    used_origin = origin;

    struct pl_source_frame snapshot_src = {};
    snapshot_src.pts = pts - origin;
    snapshot_src.duration = static_cast<float>(duration);
    snapshot_src.first_field = deinterlace_enabled
        ? pl_field_from_avframe(clone)
        : PL_FIELD_NONE;
    snapshot_src.frame_data = clone;
    snapshot_src.map = map_frame;
    snapshot_src.unmap = unmap_frame;
    snapshot_src.discard = discard_snapshot_frame;

    pl_queue_push(placebo_queue, &snapshot_src);
    newest_queued_frame_pts = qMax(newest_queued_frame_pts, pts);
    return true;
}

bool QmlMainWindow::queueStoredFrame(AVFrame *frame, double pts, float duration,
                                     void (*discard_cb)(const struct pl_source_frame *))
{
    if (!frame)
        return false;

    frame->opaque = this;

    bool deinterlace_enabled = false;
    struct pl_source_frame src_frame;
    {
        // May pl_queue_reset below: lock order frame_mix_mutex -> placebo_state_mutex.
        // Re-entrant from render() (applyPendingFrameIfQueueHasCapacity), which already
        // holds frame_mix_mutex and refreshes frame_mix right after.
        QMutexLocker mix_locker(&frame_mix_mutex);
        QMutexLocker locker(&placebo_state_mutex);
        if (newest_queued_frame_pts >= 0.0 && pts <= newest_queued_frame_pts + 1e-6) {
            qCDebug(chiakiGui)
                << "Dropping stale stored frame before queue submission"
                << "frame_pts=" << pts
                << "newest_queued_pts=" << newest_queued_frame_pts;
            av_frame_free(&frame);
            return false;
        }
        const bool preserve_timeline = preserve_playback_timeline;
        preserve_playback_timeline = false;
        if (queue_pts_origin < 0.0 || pts + 1e-6 < queue_pts_origin) {
            qCInfo(chiakiGui)
                << "Queue reset in applyPendingFrame due to pts rollback"
                << "frame_pts=" << pts
                << "queue_pts_origin=" << queue_pts_origin
                << "preserve_timeline=" << preserve_timeline;
            pl_queue_reset(placebo_queue);
            placebo_queue_generation.fetchAndAddRelease(1);
            renderer_cache_flush_pending.storeRelaxed(1);
            const double old_origin = queue_pts_origin;
            queue_pts_origin = pts;
            newest_queued_frame_pts = -1.0;
            if (preserve_timeline && old_origin >= 0.0 && playback_started) {
                const double delta_pts = queue_pts_origin - old_origin;
                const int64_t delta_us = static_cast<int64_t>(delta_pts * 1000000.0);
                int64_t ts_start_signed = static_cast<int64_t>(ts_start) + delta_us;
                if (ts_start_signed < 0)
                    ts_start_signed = 0;
                ts_start = static_cast<uint64_t>(ts_start_signed);
            } else {
                ts_start = 0;
                playback_started = false;
            }
        }
        deinterlace_enabled = this->renderparams_opts->params.deinterlace_params;
        src_frame = {
            .pts = pts - queue_pts_origin,
            .duration = static_cast<float>(duration),
            .first_field = deinterlace_enabled
                ? pl_field_from_avframe(frame)
                : PL_FIELD_NONE,
            .frame_data = frame,
            .map = map_frame,
            .unmap = unmap_frame,
            .discard = discard_cb,
        };
        pl_queue_push(placebo_queue, &src_frame);
        newest_queued_frame_pts = qMax(newest_queued_frame_pts, pts);
        anchorPlaybackLocked(src_frame.pts, duration);
    }

    // Usually runs on the render thread (render() -> applyPendingFrame / resetPlaceboQueue):
    // has_video, the cursor and the QML notification belong to the GUI thread.
    // Queued, it may run after the session ended: then there is no video to show.
    auto mark_has_video = [this](bool require_session) {
        if (has_video || (require_session && !stream_session_active.loadAcquire()))
            return;
        has_video = true;
        if (!grab_input && settings->GetHideCursor())
            setCursor(Qt::BlankCursor);
        emit hasVideoChanged();
    };
    if (QThread::currentThread() == thread())
        mark_has_video(false);
    else
        QMetaObject::invokeMethod(this, [mark_has_video]() { mark_has_video(true); }, Qt::QueuedConnection);
    scheduleUpdate();
    return true;
}

bool QmlMainWindow::applyResetSeedFrame(quint64 generation)
{
    AVFrame *frame = nullptr;
    double pts = 0.0;
    float duration = 0.0f;
    {
        QMutexLocker locker(&reset_seed_mutex);
        if (!reset_seed_frame)
            return false;
        if (reset_seed_generation != generation) {
            av_frame_free(&reset_seed_frame);
            reset_seed_pts = 0.0;
            reset_seed_duration = 0.0f;
            reset_seed_generation = 0;
            return false;
        }
        frame = reset_seed_frame;
        pts = reset_seed_pts;
        duration = reset_seed_duration;
        reset_seed_frame = nullptr;
        reset_seed_pts = 0.0;
        reset_seed_duration = 0.0f;
        reset_seed_generation = 0;
    }

    return queueStoredFrame(frame, pts, duration, discard_frame);
}

bool QmlMainWindow::applyKeptFrameSnapshot()
{
    prepareReplayFallback();
    AVFrame *clone = nullptr;
    double pts = 0.0;
    float duration = 0.0f;
    {
        QMutexLocker locker(&kept_frame_mutex);
        AVFrame *source = keptReplaySourceLocked();
        if (!source)
            return false;
        clone = clone_frame_for_replay(source);
        if (!clone)
            return false;
        pts = kept_frame_pts;
        duration = kept_frame_duration;
    }

    return queueStoredFrame(clone, pts, duration, discard_snapshot_frame);
}

void QmlMainWindow::applyPendingFrame()
{
    // The pending frame may be the one parked for a deferred reset: only
    // performHardReset() may push it, after the reset.
    if (hard_reset_pending.loadAcquire() != HardResetNone)
        return;
    AVFrame *frame = nullptr;
    double pts = 0.0;
    float duration = 0.0f;
    {
        QMutexLocker locker(&pending_frame_mutex);
        if (!pending_frame)
        {
            updatePendingFrameAge(0.0);
            return;
        }
        frame = pending_frame;
        pts = pending_pts;
        duration = pending_duration;
        pending_frame = nullptr;
        pending_pts = 0.0;
        pending_duration = 0.0f;
        pending_frame_stored_us = 0;
        pending_frame_queue_origin = 0.0;
        pending_frame_present.storeRelease(0);
    }

    updatePendingFrameAge(0.0);
    queueStoredFrame(frame, pts, duration, discard_frame);
}

bool QmlMainWindow::applyPendingFrameIfQueueHasCapacity()
{
    if (!hasPendingFrame() || hard_reset_pending.loadAcquire() != HardResetNone)
        return false;

    const int depth_limit = effectiveQueueDepthLimit();
    bool has_capacity = false;
    {
        QMutexLocker locker(&placebo_state_mutex);
        has_capacity = pl_queue_num_frames(placebo_queue) < depth_limit;
    }
    if (has_capacity) {
        applyPendingFrame();
        return true;
    }
    return false;
}

bool QmlMainWindow::hasPendingFrame() const
{
    return pending_frame_present.loadAcquire() != 0;
}

int QmlMainWindow::effectiveQueueDepthLimit() const
{
    const int configured_limit = settings ? settings->GetQueueDepthLimit() : kDefaultQueueDepthLimit;
    int depth_limit = qMax(configured_limit, 2);

    const bool backlog_pending = hasPendingFrame();
    const double avg_depth = queue_depth_average.load(std::memory_order_relaxed);
    if (backlog_pending || avg_depth >= static_cast<double>(depth_limit) - 0.25)
        depth_limit += kAdaptiveQueueDepthBoost;

    return depth_limit;
}

AVBufferRef *QmlMainWindow::vulkanHwDeviceCtx()
{
    if (render_backend != RenderBackend::Vulkan)
        return nullptr;

    if (vulkan_hw_dev_ctx || vk_decode_queue_index < 0)
        return vulkan_hw_dev_ctx;

    vulkan_hw_dev_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    AVHWDeviceContext *hwctx = reinterpret_cast<AVHWDeviceContext*>(vulkan_hw_dev_ctx->data);
    hwctx->user_opaque = const_cast<void*>(reinterpret_cast<const void*>(placebo_vulkan));
    AVVulkanDeviceContext *vkctx = reinterpret_cast<AVVulkanDeviceContext*>(hwctx->hwctx);
    vkctx->get_proc_addr = placebo_vulkan->get_proc_addr;
    vkctx->inst = placebo_vulkan->instance;
    vkctx->phys_dev = placebo_vulkan->phys_device;
    vkctx->act_dev = placebo_vulkan->device;
    vkctx->device_features = *placebo_vulkan->features;
    vkctx->enabled_inst_extensions = placebo_vk_inst->extensions;
    vkctx->nb_enabled_inst_extensions = placebo_vk_inst->num_extensions;
    vkctx->enabled_dev_extensions = placebo_vulkan->extensions;
    vkctx->nb_enabled_dev_extensions = placebo_vulkan->num_extensions;
    // FFmpeg 7.1 deprecates these in favour of the qf[] array, which additionally needs
    // the decode queue's video_caps (a vkGetPhysicalDeviceQueueFamilyProperties2 query).
    // With nb_qf == 0 FFmpeg converts the legacy fields itself, including video_caps.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    vkctx->queue_family_index = placebo_vulkan->queue_graphics.index;
    vkctx->nb_graphics_queues = placebo_vulkan->queue_graphics.count;
    vkctx->queue_family_tx_index = placebo_vulkan->queue_transfer.index;
    vkctx->nb_tx_queues = placebo_vulkan->queue_transfer.count;
    vkctx->queue_family_comp_index = placebo_vulkan->queue_compute.index;
    vkctx->nb_comp_queues = placebo_vulkan->queue_compute.count;
    vkctx->queue_family_decode_index = vk_decode_queue_index;
    vkctx->nb_decode_queues = 1;
#pragma GCC diagnostic pop
    vkctx->lock_queue = [](struct AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index) {
        auto vk = reinterpret_cast<pl_vulkan>(dev_ctx->user_opaque);
        vk->lock_queue(vk, queue_family, index);
    };
    vkctx->unlock_queue = [](struct AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index) {
        auto vk = reinterpret_cast<pl_vulkan>(dev_ctx->user_opaque);
        vk->unlock_queue(vk, queue_family, index);
    };
    if (av_hwdevice_ctx_init(vulkan_hw_dev_ctx) < 0) {
        qCWarning(chiakiGui) << "Failed to create Vulkan decode context";
        av_buffer_unref(&vulkan_hw_dev_ctx);
    }

    return vulkan_hw_dev_ctx;
}

bool QmlMainWindow::makeOpenGLContextCurrent()
{
    if (!qt_gl_context)
        return false;

    QSurface *target_surface = nullptr;
    if (qt_gl_offscreen_surface && !isExposed())
        target_surface = qt_gl_offscreen_surface;
    else if (handle())
        target_surface = this;
    else if (qt_gl_offscreen_surface)
        target_surface = qt_gl_offscreen_surface;

    if (!target_surface)
        return false;

    if (QThread::currentThread() == qt_gl_context->thread())
        return qt_gl_context->makeCurrent(target_surface);

    bool ok = false;
    QObject *context_obj = qt_gl_context;
    QMetaObject::invokeMethod(context_obj, [this, target_surface, &ok]() {
        ok = qt_gl_context && qt_gl_context->makeCurrent(target_surface);
    }, Qt::BlockingQueuedConnection);
    return ok;
}

void QmlMainWindow::doneOpenGLContextCurrent()
{
    if (!qt_gl_context)
        return;

    if (QThread::currentThread() == qt_gl_context->thread()) {
        qt_gl_context->doneCurrent();
        return;
    }

    QObject *context_obj = qt_gl_context;
    QMetaObject::invokeMethod(context_obj, [this]() {
        if (qt_gl_context)
            qt_gl_context->doneCurrent();
    }, Qt::BlockingQueuedConnection);
}

void QmlMainWindow::init(Settings *settings, bool exit_app_on_stream_exit)
{
    render_backend = settings->GetRenderBackend();
    setSurfaceType(render_backend == RenderBackend::Vulkan ? QWindow::VulkanSurface : QWindow::OpenGLSurface);
    qparams = {};
    qparams.drift_compensation = 1e-3;
    qparams.interpolation_threshold = 0.01;
    qparams.timeout = 0;

    auto initOpenGLBackend = [&]() -> bool {
        QSurfaceFormat format = QSurfaceFormat::defaultFormat();
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setAlphaBufferSize(8);
        format.setDepthBufferSize(24);
        format.setStencilBufferSize(8);
        format.setSwapInterval(settings->GetVSyncEnabled() ? 1 : 0);
        present_vsync_enabled.storeRelaxed(settings->GetVSyncEnabled() ? 1 : 0);
#if defined(Q_OS_MACOS)
        // macOS only exposes modern OpenGL as a core profile.
        if (format.majorVersion() < 4) {
            format.setVersion(4, 1);
            format.setProfile(QSurfaceFormat::CoreProfile);
        }
#endif

        qt_gl_context = new QOpenGLContext();
        qt_gl_context->setFormat(format);
        if (QOpenGLContext *share = QOpenGLContext::globalShareContext())
            qt_gl_context->setShareContext(share);
        if (!qt_gl_context->create()) {
            qCWarning(chiakiGui) << "Failed to create QOpenGLContext";
            return false;
        }

        setFormat(qt_gl_context->format());
        qt_gl_offscreen_surface = new QOffscreenSurface();
        qt_gl_offscreen_surface->setFormat(qt_gl_context->format());
        qt_gl_offscreen_surface->create();
        if (!qt_gl_offscreen_surface->isValid()) {
            qCWarning(chiakiGui) << "Failed to create OpenGL offscreen surface";
            return false;
        }

        if (!this->handle())
            this->create();

        if (!makeOpenGLContextCurrent()) {
            qCWarning(chiakiGui) << "Failed to make QOpenGLContext current";
            return false;
        }

        detectOpenGLGpuVendor(this);
        amd_card = property("_chiaki_detected_amd").toBool();
        nvidia_card = property("_chiaki_detected_nvidia").toBool();

        struct pl_opengl_params gl_params = {
            .get_proc_addr_ex = [](void *proc_ctx, const char *procname) -> pl_voidfunc_t {
                auto ctx = static_cast<QOpenGLContext *>(proc_ctx);
                // Use volatile to prevent compiler optimizations from caching the address on macOS.
                volatile pl_voidfunc_t addr = reinterpret_cast<pl_voidfunc_t>(ctx->getProcAddress(procname));
                return addr;
            },
            .proc_ctx = qt_gl_context,
            .debug = false,
            .allow_software = true,
            .make_current = [](void *priv) -> bool {
                auto q = static_cast<QmlMainWindow *>(priv);
                return q->makeOpenGLContextCurrent();
            },
            .release_current = [](void *priv) {
                auto q = static_cast<QmlMainWindow *>(priv);
                q->doneOpenGLContextCurrent();
            },
            .priv = this,
        };
        placebo_opengl = pl_opengl_create(placebo_log, &gl_params);
        if (!placebo_opengl) {
            qCWarning(chiakiGui) << "Failed to create libplacebo OpenGL backend";
            return false;
        }
        doneOpenGLContextCurrent();
        return true;
    };

    auto fallbackToOpenGL = [&](const QString &reason) {
        qCWarning(chiakiGui) << "Vulkan renderer initialization failed, falling back to OpenGL:" << reason;
        pending_renderer_fallback_reason = reason;

        if (placebo_vulkan)
            pl_vulkan_destroy(&placebo_vulkan);
        if (placebo_vk_inst)
            pl_vk_inst_destroy(&placebo_vk_inst);

        render_backend = RenderBackend::OpenGL;
        setSurfaceType(QWindow::OpenGLSurface);
        if (!initOpenGLBackend())
            qFatal("Failed initializing OpenGL backend");
    };

    struct pl_log_params log_params = {
        .log_cb = placebo_log_cb,
        .log_level = nax5_pl_log_level(),
    };
    placebo_log = pl_log_create(PL_API_VER, &log_params);

    if (render_backend == RenderBackend::Vulkan) {
        if (qEnvironmentVariableIsSet("CHIAKI_FORCE_VULKAN_FALLBACK")) {
            fallbackToOpenGL(QStringLiteral("Forced Vulkan fallback for testing"));
            goto renderer_backend_ready;
        }

        const char *vk_exts[] = {
            nullptr,
            VK_KHR_SURFACE_EXTENSION_NAME,
        };

#if defined(Q_OS_LINUX)
        if (QGuiApplication::platformName().startsWith("wayland"))
            vk_exts[0] = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
        else if (QGuiApplication::platformName().startsWith("xcb"))
            vk_exts[0] = VK_KHR_XCB_SURFACE_EXTENSION_NAME;
        else
            Q_UNREACHABLE();
#elif defined(Q_OS_MACOS)
        vk_exts[0] = VK_EXT_METAL_SURFACE_EXTENSION_NAME;
#elif defined(Q_OS_WIN)
        vk_exts[0] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#endif

        const char *opt_extensions[] = {
            VK_EXT_HDR_METADATA_EXTENSION_NAME,
        };

        struct pl_vk_inst_params vk_inst_params = {
            .extensions = vk_exts,
            .num_extensions = 2,
            .opt_extensions = opt_extensions,
            .num_opt_extensions = 1,
        };
        placebo_vk_inst = pl_vk_inst_create(placebo_log, &vk_inst_params);
        if (!placebo_vk_inst) {
            fallbackToOpenGL(QStringLiteral("Failed to create Vulkan instance"));
        } else {

#define GET_PROC(name_) { \
    vk_funcs.name_ = reinterpret_cast<decltype(vk_funcs.name_)>( \
        placebo_vk_inst->get_proc_addr(placebo_vk_inst->instance, #name_)); \
    if (!vk_funcs.name_) { \
        fallbackToOpenGL(QStringLiteral("Failed to resolve Vulkan symbol %1").arg(#name_)); \
        goto vulkan_setup_done; \
    } }
        GET_PROC(vkGetDeviceProcAddr)
#if defined(Q_OS_LINUX)
        if (QGuiApplication::platformName().startsWith("wayland"))
            GET_PROC(vkCreateWaylandSurfaceKHR)
        else if (QGuiApplication::platformName().startsWith("xcb"))
            GET_PROC(vkCreateXcbSurfaceKHR)
#elif defined(Q_OS_MACOS)
        GET_PROC(vkCreateMetalSurfaceEXT)
#elif defined(Q_OS_WIN)
        GET_PROC(vkCreateWin32SurfaceKHR)
#endif
        GET_PROC(vkDestroySurfaceKHR)
        GET_PROC(vkGetPhysicalDeviceQueueFamilyProperties)
        GET_PROC(vkGetPhysicalDeviceProperties)
        GET_PROC(vkGetPhysicalDeviceSurfacePresentModesKHR)
#undef GET_PROC

        const char *opt_dev_extensions[] = {
            VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
            VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
            VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
            VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
        };

        struct pl_vulkan_params vulkan_params = {
            .instance = placebo_vk_inst->instance,
            .get_proc_addr = placebo_vk_inst->get_proc_addr,
            .allow_software = true,
            PL_VULKAN_DEFAULTS
            .extra_queues = VK_QUEUE_VIDEO_DECODE_BIT_KHR,
            .opt_extensions = opt_dev_extensions,
            .num_opt_extensions = 4,
        };
        placebo_vulkan = pl_vulkan_create(placebo_log, &vulkan_params);
        if (!placebo_vulkan) {
            fallbackToOpenGL(QStringLiteral("Failed to create Vulkan device"));
        } else {

#define GET_PROC(name_) { \
    vk_funcs.name_ = reinterpret_cast<decltype(vk_funcs.name_)>( \
        vk_funcs.vkGetDeviceProcAddr(placebo_vulkan->device, #name_)); \
    if (!vk_funcs.name_) { \
        fallbackToOpenGL(QStringLiteral("Failed to resolve Vulkan symbol %1").arg(#name_)); \
        goto vulkan_setup_done; \
    } }
        GET_PROC(vkWaitSemaphores)
#undef GET_PROC

        uint32_t queueFamilyCount = 0;
        vk_funcs.vkGetPhysicalDeviceQueueFamilyProperties(placebo_vulkan->phys_device, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilyProperties(queueFamilyCount);
        vk_funcs.vkGetPhysicalDeviceQueueFamilyProperties(placebo_vulkan->phys_device, &queueFamilyCount, queueFamilyProperties.data());
        auto queue_it = std::find_if(queueFamilyProperties.begin(), queueFamilyProperties.end(), [](VkQueueFamilyProperties prop) {
            return prop.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR;
        });
        if (queue_it != queueFamilyProperties.end())
            vk_decode_queue_index = std::distance(queueFamilyProperties.begin(), queue_it);
        VkPhysicalDeviceProperties device_props;
        vk_funcs.vkGetPhysicalDeviceProperties(placebo_vulkan->phys_device, &device_props);
        if(device_props.vendorID == 0x1002)
        {
            amd_card = true;
            qCInfo(chiakiGui) << "Using amd graphics card";
        }
        else if(device_props.vendorID == 0x10de)
        {
            nvidia_card = true;
            qCInfo(chiakiGui) << "Using nvidia graphics card";
        }
        }
        }
vulkan_setup_done:
        ;
    } else {
        if (!initOpenGLBackend())
            qFatal("Failed initializing OpenGL backend");
    }

    if (render_backend == RenderBackend::Vulkan) {
        qt_vk_inst = new QVulkanInstance;
        qt_vk_inst->setVkInstance(placebo_vk_inst->instance);
        if (!qt_vk_inst->create()) {
            delete qt_vk_inst;
            qt_vk_inst = nullptr;
            fallbackToOpenGL(QStringLiteral("Failed to create QVulkanInstance"));
        }
    }

renderer_backend_ready:
    struct pl_cache_params cache_params = {
        .log = placebo_log,
        .max_total_size = 10 << 20, // 10 MB
    };
    placebo_cache = pl_cache_create(&cache_params);
    pl_gpu_set_cache(placeboGpu(), placebo_cache);
    FILE *file = fopen(qPrintable(shader_cache_path()), "rb");
    if (file) {
        pl_cache_load_file(placebo_cache, file);
        fclose(file);
    }
    shader_cache_saved_signature = pl_cache_signature(placebo_cache);

    placebo_renderer = pl_renderer_create(
        placebo_log,
        placeboGpu()
    );

    placebo_queue = pl_queue_create(placeboGpu());

    if (render_backend == RenderBackend::Vulkan) {
        struct pl_vulkan_sem_params sem_params = {
            .type = VK_SEMAPHORE_TYPE_TIMELINE,
        };
        quick_sem = pl_vulkan_sem_create(placebo_vulkan->gpu, &sem_params);
    }

    quick_render = new RenderControl(this);

    QQuickWindow::setDefaultAlphaBuffer(true);
    QQuickWindow::setGraphicsApi(render_backend == RenderBackend::Vulkan ? QSGRendererInterface::Vulkan : QSGRendererInterface::OpenGL);
    quick_window = new QQuickWindow(quick_render);
    if (render_backend == RenderBackend::Vulkan) {
        quick_window->setVulkanInstance(qt_vk_inst);
        quick_window->setGraphicsDevice(QQuickGraphicsDevice::fromDeviceObjects(placebo_vulkan->phys_device, placebo_vulkan->device, placebo_vulkan->queue_graphics.index));
    } else {
        quick_window->setGraphicsDevice(QQuickGraphicsDevice::fromOpenGLContext(qt_gl_context));
    }
    quick_window->setColor(QColor(0, 0, 0, 0));
    connect(quick_window, &QQuickWindow::focusObjectChanged, this, &QmlMainWindow::focusObjectChanged);

    qml_engine = new QQmlEngine(this);
    qml_engine->addImageProvider(QStringLiteral("svg"), new QmlSvgProvider);
    if (!qml_engine->incubationController())
        qml_engine->setIncubationController(quick_window->incubationController());
    connect(qml_engine, &QQmlEngine::quit, this, &QWindow::close);

    backend = new QmlBackend(settings, this);
    connect(backend, &QmlBackend::sessionChanged, this, [this, exit_app_on_stream_exit](StreamSession *s) {
        session = s;
        stream_session_active.storeRelease(s ? 1 : 0);
        grab_input = 0;
        stream_frame_width.storeRelaxed(0);
        stream_frame_height.storeRelaxed(0);
        if (stats_log) {
            // The summary needs the quit reason, whose SessionQuit slot (connected below,
            // after the backend's) runs after this handler in the same emit chain: hand
            // the file over and finish it on the next event loop pass.
            flushFrameTrace();
            QFile *finished = stats_log;
            stats_log = nullptr;
            const StatsAccum acc = stats_acc;
            const qint64 duration_ms = stats_log_clock.elapsed();
            QTimer::singleShot(0, this, [this, finished, acc, duration_ms]() {
                writeStreamStatsSummary(finished, acc, duration_ms);
                finished->close();
                delete finished;
            });
        }
        closeStreamStatsLog();
        stats_log_failed = false;
        catch_up_count.storeRelaxed(0);
        repeat_skips.storeRelaxed(0);
        if (session) {
            session->BlockInput(0);
            stats_quit_reason.clear();
            connect(session, &StreamSession::SessionQuit, this, [this](ChiakiQuitReason reason, const QString &) {
                stats_quit_reason = QString::fromLatin1(chiaki_quit_reason_string(reason)).simplified().replace(QLatin1Char(' '), QLatin1Char('_'));
            });
        }
        update_timer->stop();
        // Lock order: frame_mix_mutex -> placebo_state_mutex (see the header). A render
        // holds frame_mix_mutex through start_frame/swap (GPU waits, shader compiles):
        // don't stall the GUI thread on it. If it isn't free within 50 ms, the render
        // thread does this reset before it next touches the queue (performHardReset);
        // the rest of the teardown doesn't depend on it.
        if (frame_mix_mutex.tryLock(50)) {
            std::unique_lock<QRecursiveMutex> mix_locker(frame_mix_mutex, std::adopt_lock);
            QMutexLocker locker(&placebo_state_mutex);
            pl_queue_reset(placebo_queue);
            placebo_queue_generation.fetchAndAddRelease(1);
            newest_queued_frame_pts = -1.0;
            queue_pts_origin = -1.0;
            playback_started = false;
            ts_start = 0;
            // This reset supersedes a deferred one still waiting (the render thread
            // is not in performHardReset: we hold frame_mix_mutex).
            hard_reset_pending.storeRelease(HardResetNone);
        } else {
            qCInfo(chiakiGui) << "Queue reset on session change deferred to the render thread";
            // No extra render is requested: whichever render() runs next does the
            // reset first, and until then nothing touches the queue (presentFrame
            // parks frames while the flag is set).
            hard_reset_pending.storeRelease(HardResetSessionEnd);
        }
        {
            QMutexLocker locker(&reset_seed_mutex);
            if (reset_seed_frame)
                av_frame_free(&reset_seed_frame);
            reset_seed_pts = 0.0;
            reset_seed_duration = 0.0f;
            reset_seed_generation = 0;
        }
        clearSnapshotFrame();
        snapshot_generation.storeRelaxed(0);
        pending_reset_snapshot_generation.storeRelaxed(0);
        last_reset_snapshot_generation.storeRelaxed(0);
        renderer_cache_flush_pending.storeRelaxed(1);
        reset_seed_capture_active.storeRelease(0);
        reset_seed_capture_generation.storeRelease(0);
        if (has_video) {
            has_video = false;
            setCursor(Qt::ArrowCursor);
            emit hasVideoChanged();
        }
        if(session && exit_app_on_stream_exit)
        {
            connect(session, &StreamSession::SessionQuit, qGuiApp, &QGuiApplication::quit);
        }
        if(!session)
        {
            // Shaders compiled during the stream survive a later hard exit (the
            // destructor save never runs then): ~240 ms of SPIR-V work at next start.
            if (nax5_cache_save_on_stop())
                saveShaderCache();
            setStreamWindowAdjustable(false);
            if(qEnvironmentVariable("XDG_CURRENT_DESKTOP") != "gamescope")
            {
                if(!this->settings->GetGeometry().isEmpty())
                {
                    setGeometry(this->settings->GetGeometry());
                    normalTime();
                }
                else
                    showMaximized();
            }
        }
    });
    connect(backend, &QmlBackend::windowTypeUpdated, this, &QmlMainWindow::updateWindowType);

    if (render_backend == RenderBackend::OpenGL) {
        render_thread = QGuiApplication::instance()->thread();
        owns_render_thread = false;
    } else {
        render_thread = new QThread;
        render_thread->setObjectName("render");
        render_thread->start();
        owns_render_thread = true;

        quick_render->prepareThread(render_thread);
        quick_render->moveToThread(render_thread);
    }

    connect(quick_render, &QQuickRenderControl::sceneChanged, this, [this]() {
        quick_need_sync.storeRelaxed(1);
        qml_updates_current.fetchAndAddRelaxed(1);
        scheduleUpdate();
    });
    connect(quick_render, &QQuickRenderControl::renderRequested, this, [this]() {
        quick_need_render.storeRelaxed(1);
        qml_updates_current.fetchAndAddRelaxed(1);
        scheduleUpdate();
    });

    update_timer = new QTimer(this);
    update_timer->setSingleShot(true);
    connect(update_timer, &QTimer::timeout, this, &QmlMainWindow::update);

    if (render_backend == RenderBackend::OpenGL) {
        if (!makeOpenGLContextCurrent())
            qFatal("Failed to make QOpenGLContext current for render control initialization");
        quick_render->initialize();
        doneOpenGLContextCurrent();
    } else {
        QMetaObject::invokeMethod(quick_render, &QQuickRenderControl::initialize);
    }

    // Stats-panel values change every frame / every render; announce them to QML at most
    // 4 times a second and only when the shown value changes (a notification per render
    // came from the render thread and re-evaluated the bindings each time). The values
    // themselves, the CSV and the queue logic are unaffected.
    QTimer *overlay_values_timer = new QTimer(this);
    overlay_values_timer->setInterval(250);
    overlay_values_timer->start();
    connect(overlay_values_timer, &QTimer::timeout, this, [this]() {
        const int depth_x10 = qRound(queueDepthAverage() * 10.0);
        if (depth_x10 != overlay_notified_depth_x10) {
            overlay_notified_depth_x10 = depth_x10;
            emit queueDepthAverageChanged();
        }
        const int age_ms = qRound(pendingFrameAge() * 1000.0);
        if (age_ms != overlay_notified_age_ms) {
            overlay_notified_age_ms = age_ms;
            emit pendingFrameAgeChanged();
        }
    });

    QTimer *dropped_frames_timer = new QTimer(this);
    dropped_frames_timer->setInterval(1000);
    dropped_frames_timer->start();
    connect(dropped_frames_timer, &QTimer::timeout, this, [this]() {
        int dropped_frames_next = dropped_frames_current.fetchAndStoreRelaxed(0);
        if (dropped_frames != dropped_frames_next) {
            dropped_frames = dropped_frames_next;
            emit droppedFramesChanged();
        }
        const int stream_fps_next = stream_frames_current.fetchAndStoreRelaxed(0);
        const int display_fps_next = display_frames_current.fetchAndStoreRelaxed(0);
        const int width = stream_frame_width.loadRelaxed();
        const int height = stream_frame_height.loadRelaxed();
        const QString resolution_next = width > 0 && height > 0
            ? QStringLiteral("%1x%2").arg(width).arg(height) : QString();
        if (stream_fps != stream_fps_next || display_fps != display_fps_next || stream_resolution != resolution_next) {
            stream_fps = stream_fps_next;
            display_fps = display_fps_next;
            stream_resolution = resolution_next;
            emit fpsChanged();
        }
        writeStreamStatsRow(stream_fps_next, display_fps_next, dropped_frames_next, resolution_next);
    });

    this->renderparams_opts = pl_options_alloc(this->placebo_log);
    pl_options_reset(this->renderparams_opts, &pl_render_high_quality_params);
    this->renderparams_changed = true;
    this->fsr_hook = load_mpv_hook(placeboGpu(), QStringLiteral(":/shaders/FSR.glsl"));
    this->fsrcnnx_hook_8 = load_mpv_hook(placeboGpu(), QStringLiteral(":/shaders/FSRCNNX_x2_8-0-4-1.glsl"));
    this->fsrcnnx_hook_16 = load_mpv_hook(placeboGpu(), QStringLiteral(":/shaders/FSRCNNX_x2_16-0-4-1.glsl"));


    switch (settings->GetPlaceboPreset()) {
    case PlaceboPreset::Fast:
        setVideoPreset(VideoPreset::Fast);
        break;
    case PlaceboPreset::Default:
        setVideoPreset(VideoPreset::Default);
        break;
    case PlaceboPreset::HighQualitySpatial:
        setVideoPreset(VideoPreset::HighQualitySpatial);
        break;
    case PlaceboPreset::HighQualityAdvancedSpatial:
        setVideoPreset(VideoPreset::HighQualityAdvancedSpatial);
        break;
    case PlaceboPreset::Custom:
        setVideoPreset(VideoPreset::Custom);
        break;
    case PlaceboPreset::HighQuality:
    default:
        setVideoPreset(VideoPreset::HighQuality);
        break;
    }
    setZoomFactor(settings->GetZoomFactor());
}

void QmlMainWindow::normalTime()
{
    if(windowState() == Qt::WindowFullScreen)
    {
        if(was_maximized)
        {
            showMaximized();
            was_maximized = false;
        }
        else
            showNormal();
        setMinimumSize(QSize(0, 0));
    }
    QTimer::singleShot(1000, this, [this]{
        if(session)
            setStreamWindowAdjustable(true);
        else
        {
            setWindowAdjustable(true);
        }
    });
}

void QmlMainWindow::fullscreenTime()
{
    if(windowState() == Qt::WindowFullScreen)
        return;
    if(session)
        setStreamWindowAdjustable(false);
    else
        setWindowAdjustable(false);
    setMinimumSize(size());
    if(windowState() == Qt::WindowMaximized)
        was_maximized = true;
    else
        was_maximized = false;
    showFullScreen();
}
void QmlMainWindow::update()
{
    Q_ASSERT(QThread::currentThread() == QGuiApplication::instance()->thread());

    {
        QMutexLocker locker(&render_schedule_mutex);
        if (render_scheduled) {
            render_pending = true;
            return;
        }
        render_scheduled = true;
    }

    if (quick_need_sync.fetchAndStoreRelaxed(0) != 0) {
        quick_render->polishItems();
        if (quick_render->thread() == QThread::currentThread())
            sync();
        else
            QMetaObject::invokeMethod(quick_render, std::bind(&QmlMainWindow::sync, this), Qt::BlockingQueuedConnection);
    }

    update_timer->stop();
    QMetaObject::invokeMethod(quick_render, std::bind(&QmlMainWindow::render, this));
}

void QmlMainWindow::scheduleUpdate()
{
    if (QThread::currentThread() != QGuiApplication::instance()->thread()) {
        QMetaObject::invokeMethod(this, &QmlMainWindow::scheduleUpdate, Qt::QueuedConnection);
        return;
    }

    // A slow heartbeat gets replaced by the regular interval.
    if (!update_timer->isActive() || update_timer_heartbeat) {
        double refresh_rate = screen() ? screen()->refreshRate() : 60.0;
        if (refresh_rate <= 1.0)
            refresh_rate = 60.0;
        int interval_ms = has_video ? qMax(1, (int)(1000.0 / refresh_rate)) : 10;
        if (hasPendingFrame())
            interval_ms = qMax(1, interval_ms / 2);
        update_timer_heartbeat = false;
        update_timer->start(interval_ms);
    }
}

void QmlMainWindow::scheduleHeartbeatUpdate()
{
    if (QThread::currentThread() != QGuiApplication::instance()->thread()) {
        QMetaObject::invokeMethod(this, &QmlMainWindow::scheduleHeartbeatUpdate, Qt::QueuedConnection);
        return;
    }

    // Never delays an update that is already due sooner.
    if (!update_timer->isActive()) {
        update_timer_heartbeat = true;
        update_timer->start(nax5_heartbeat_ms);
    }
}

void QmlMainWindow::updatePlacebo()
{
    renderparams_changed = true;
}

void QmlMainWindow::updateVSync()
{
    if (render_backend != RenderBackend::Vulkan)
        return;
    swapchain_recreate_pending.storeRelaxed(1);
    scheduleUpdate();
}

void QmlMainWindow::createSwapchain()
{
    Q_ASSERT(QThread::currentThread() == render_thread);

    if (placebo_swapchain)
        return;

    if (render_backend == RenderBackend::OpenGL) {
        if (!makeOpenGLContextCurrent()) {
            qCCritical(chiakiGui) << "Failed to make OpenGL context current";
            return;
        }
        struct pl_opengl_swapchain_params sw_params = {
            .swap_buffers = [](void *priv) {
                auto q = static_cast<QmlMainWindow *>(priv);
                if (q->qt_gl_context)
                    q->qt_gl_context->swapBuffers(q);
            },
            .priv = this,
        };
        placebo_swapchain = pl_opengl_create_swapchain(placebo_opengl, &sw_params);
        doneOpenGLContextCurrent();
        if (!placebo_swapchain)
            qCCritical(chiakiGui) << "Failed creating OpenGL swapchain";
        return;
    }

    VkResult err = VK_ERROR_UNKNOWN;
#if defined(Q_OS_LINUX)
    if (QGuiApplication::platformName().startsWith("wayland")) {
        VkWaylandSurfaceCreateInfoKHR surfaceInfo = {};
        surfaceInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        surfaceInfo.display = reinterpret_cast<struct wl_display*>(QGuiApplication::platformNativeInterface()->nativeResourceForWindow("display", this));
        surfaceInfo.surface = reinterpret_cast<struct wl_surface*>(QGuiApplication::platformNativeInterface()->nativeResourceForWindow("surface", this));
        err = vk_funcs.vkCreateWaylandSurfaceKHR(placebo_vk_inst->instance, &surfaceInfo, nullptr, &surface);
    } else if (QGuiApplication::platformName().startsWith("xcb")) {
        VkXcbSurfaceCreateInfoKHR surfaceInfo = {};
        surfaceInfo.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
        surfaceInfo.connection = reinterpret_cast<xcb_connection_t*>(QGuiApplication::platformNativeInterface()->nativeResourceForWindow("connection", this));
        surfaceInfo.window = static_cast<xcb_window_t>(winId());
        err = vk_funcs.vkCreateXcbSurfaceKHR(placebo_vk_inst->instance, &surfaceInfo, nullptr, &surface);
    } else {
        Q_UNREACHABLE();
    }
#elif defined(Q_OS_MACOS)
    VkMetalSurfaceCreateInfoEXT surfaceInfo = {};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    surfaceInfo.pLayer = static_cast<const CAMetalLayer*>(reinterpret_cast<void*(*)(id, SEL)>(objc_msgSend)(reinterpret_cast<id>(winId()), sel_registerName("layer")));
    err = vk_funcs.vkCreateMetalSurfaceEXT(placebo_vk_inst->instance, &surfaceInfo, nullptr, &surface);
#elif defined(Q_OS_WIN)
    VkWin32SurfaceCreateInfoKHR surfaceInfo = {};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.hinstance = GetModuleHandle(nullptr);
    surfaceInfo.hwnd = reinterpret_cast<HWND>(winId());
    err = vk_funcs.vkCreateWin32SurfaceKHR(placebo_vk_inst->instance, &surfaceInfo, nullptr, &surface);
#endif

    if (err != VK_SUCCESS)
        qFatal("Failed to create VkSurfaceKHR");

    // Present mode: the VSync setting means FIFO; otherwise NAX5_PRESENT_MODE = mailbox
    // (default) | immediate | fifo, falling back MAILBOX -> IMMEDIATE -> FIFO among the
    // modes the surface supports. libplacebo silently uses FIFO for an unsupported mode,
    // so the choice has to be made here, before pl_vulkan_create_swapchain.
    const bool vsync_setting = settings->GetVSyncEnabled();
    VkPresentModeKHR requested = VK_PRESENT_MODE_MAILBOX_KHR;
    {
        const QByteArray env = qgetenv("NAX5_PRESENT_MODE").trimmed().toLower();
        if (env == "immediate")
            requested = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (env == "fifo")
            requested = VK_PRESENT_MODE_FIFO_KHR;
        else if (!env.isEmpty() && env != "mailbox")
            qCWarning(chiakiGui) << "Unknown NAX5_PRESENT_MODE" << env << "- using mailbox";
    }
    if (vsync_setting)
        requested = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkPresentModeKHR> supported;
    {
        uint32_t count = 0;
        if (vk_funcs.vkGetPhysicalDeviceSurfacePresentModesKHR(placebo_vulkan->phys_device, surface, &count, nullptr) == VK_SUCCESS && count) {
            supported.resize(count);
            // VK_INCOMPLETE / errors: treat as FIFO-only (empty list). Never keep
            // zero-filled entries: 0 is VK_PRESENT_MODE_IMMEDIATE_KHR.
            if (vk_funcs.vkGetPhysicalDeviceSurfacePresentModesKHR(placebo_vulkan->phys_device, surface, &count, supported.data()) == VK_SUCCESS)
                supported.resize(count);
            else
                supported.clear();
        }
    }
    auto is_supported = [&](VkPresentModeKHR m) {
        // FIFO is mandatory by the spec; an empty answer means the query failed.
        return supported.empty() ? m == VK_PRESENT_MODE_FIFO_KHR
                                 : std::find(supported.begin(), supported.end(), m) != supported.end();
    };
    VkPresentModeKHR chosen = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR candidate : { requested, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR }) {
        if (is_supported(candidate)) {
            chosen = candidate;
            break;
        }
    }
    QStringList supported_names;
    for (VkPresentModeKHR m : supported)
        supported_names << QString::fromLatin1(nax5_present_mode_name(m));
    qCInfo(chiakiGui).noquote() << QStringLiteral("Present mode: requested %1, supported [%2], using %3")
        .arg(QString::fromLatin1(nax5_present_mode_name(requested)))
        .arg(supported_names.join(QStringLiteral(", ")))
        .arg(QString::fromLatin1(nax5_present_mode_name(chosen)));

    struct pl_vulkan_swapchain_params swapchain_params = {
        .surface = surface,
        .present_mode = chosen,
        .swapchain_depth = 1,
    };
    placebo_swapchain = pl_vulkan_create_swapchain(placebo_vulkan, &swapchain_params);
    if (!placebo_swapchain && chosen != VK_PRESENT_MODE_FIFO_KHR) {
        qCWarning(chiakiGui) << "Swapchain creation failed with" << nax5_present_mode_name(chosen) << "- falling back to FIFO VSync";
        swapchain_params.present_mode = VK_PRESENT_MODE_FIFO_KHR;
        placebo_swapchain = pl_vulkan_create_swapchain(placebo_vulkan, &swapchain_params);
    }
    if (!placebo_swapchain) {
        vk_funcs.vkDestroySurfaceKHR(placebo_vk_inst->instance, surface, nullptr);
        surface = VK_NULL_HANDLE;
        present_mode_actual.storeRelease(-1);
        return;
    }
    // From the ACTUAL mode: FIFO/FIFO_RELAXED wait for vblank, mailbox/immediate do not.
    present_mode_actual.storeRelease(swapchain_params.present_mode);
    present_vsync_enabled.storeRelease(swapchain_params.present_mode == VK_PRESENT_MODE_FIFO_KHR
                                       || swapchain_params.present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? 1 : 0);
    // Swap statistics restart with the new swapchain; a measured panel period is kept
    // and keeps being refreshed while the mode is FIFO.
    last_swap_us = 0;
    last_new_swap_us = 0;
    panel_window_start_us = 0;
    panel_intervals_us.clear();
}

void QmlMainWindow::destroySwapchain()
{
    Q_ASSERT(QThread::currentThread() == render_thread);

    if (!placebo_swapchain)
        return;

    pl_swapchain_destroy(&placebo_swapchain);
    last_present_valid = false;
    if (render_backend == RenderBackend::Vulkan)
        vk_funcs.vkDestroySurfaceKHR(placebo_vk_inst->instance, surface, nullptr);
    if (render_backend == RenderBackend::OpenGL && quick_fbo) {
        if (!makeOpenGLContextCurrent()) {
            qCWarning(chiakiGui) << "Failed to make OpenGL context current while destroying swapchain";
            swapchain_size = QSize();
            return;
        }
        delete quick_fbo;
        quick_fbo = nullptr;
        doneOpenGLContextCurrent();
    } else {
        delete quick_fbo;
        quick_fbo = nullptr;
    }
    swapchain_size = QSize();
}

void QmlMainWindow::resizeSwapchain()
{
    Q_ASSERT(QThread::currentThread() == render_thread);

    if (!placebo_swapchain)
        createSwapchain();
    if (!placebo_swapchain)
        return;

    const QSize window_size(width() * devicePixelRatio(), height() * devicePixelRatio());
    const bool swapchain_ready = quick_tex && (render_backend != RenderBackend::OpenGL || quick_fbo);
    if (window_size == swapchain_size && swapchain_ready)
        return;

    QSize new_swapchain_size = window_size;
    pl_swapchain_resize(placebo_swapchain, &new_swapchain_size.rwidth(), &new_swapchain_size.rheight());
    last_present_valid = false;

    if (render_backend == RenderBackend::OpenGL) {
        if (!makeOpenGLContextCurrent()) {
            qCCritical(chiakiGui) << "Failed to make OpenGL context current";
            return;
        }

        QOpenGLFramebufferObject *new_quick_fbo =
            new QOpenGLFramebufferObject(new_swapchain_size, QOpenGLFramebufferObject::CombinedDepthStencil);
        if (!new_quick_fbo || !new_quick_fbo->isValid()) {
            qCCritical(chiakiGui) << "Failed to create Qt Quick OpenGL framebuffer object";
            delete new_quick_fbo;
            doneOpenGLContextCurrent();
            return;
        }

        int quick_texture_internal_format = static_cast<int>(new_quick_fbo->format().internalTextureFormat());
        if (!quick_texture_internal_format)
            quick_texture_internal_format = 0x8058; // GL_RGBA8
        struct pl_opengl_wrap_params wrap_params = {
            .texture = new_quick_fbo->texture(),
            .framebuffer = static_cast<unsigned int>(new_quick_fbo->handle()),
            .width = new_swapchain_size.width(),
            .height = new_swapchain_size.height(),
            .target = 0x0DE1,
            .iformat = quick_texture_internal_format,
        };
        pl_tex new_quick_tex = pl_opengl_wrap(placeboGpu(), &wrap_params);
        if (!new_quick_tex) {
            qCCritical(chiakiGui) << "Failed to wrap Qt Quick OpenGL framebuffer texture";
            delete new_quick_fbo;
            doneOpenGLContextCurrent();
            return;
        }

        quick_window->setRenderTarget(QQuickRenderTarget());
        if (quick_tex)
            pl_tex_destroy(placeboGpu(), &quick_tex);
        delete quick_fbo;
        quick_fbo = new_quick_fbo;
        quick_tex = new_quick_tex;
        swapchain_size = new_swapchain_size;
        quick_window->setRenderTarget(QQuickRenderTarget::fromOpenGLTexture(quick_fbo->texture(), quick_fbo->size()));
        doneOpenGLContextCurrent();
        return;
    }

    struct pl_tex_params tex_params = {
        .w = new_swapchain_size.width(),
        .h = new_swapchain_size.height(),
        .format = pl_find_fmt(placebo_vulkan->gpu, PL_FMT_UNORM, 4, 8, 8, PL_FMT_CAP_RENDERABLE),
        .sampleable = true,
        .renderable = true,
    };
    if (!pl_tex_recreate(placebo_vulkan->gpu, &quick_tex, &tex_params))
    {
        qCCritical(chiakiGui) << "Failed to create placebo texture";
        return;
    }

    VkFormat vk_format;
    VkImage vk_image = pl_vulkan_unwrap(placebo_vulkan->gpu, quick_tex, &vk_format, nullptr);
    swapchain_size = new_swapchain_size;
    quick_window->setRenderTarget(QQuickRenderTarget::fromVulkanImage(vk_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, vk_format, swapchain_size));
}

void QmlMainWindow::updateSwapchain()
{
    Q_ASSERT(QThread::currentThread() == QGuiApplication::instance()->thread());

    quick_item->setSize(size());
    quick_window->resize(size());

    if (quick_render->thread() == QThread::currentThread())
        resizeSwapchain();
    else
        QMetaObject::invokeMethod(quick_render, std::bind(&QmlMainWindow::resizeSwapchain, this), Qt::BlockingQueuedConnection);
    quick_render->polishItems();
    if (quick_render->thread() == QThread::currentThread())
        sync();
    else
        QMetaObject::invokeMethod(quick_render, std::bind(&QmlMainWindow::sync, this), Qt::BlockingQueuedConnection);
    update();
}

void QmlMainWindow::sync()
{
    Q_ASSERT(QThread::currentThread() == render_thread);

    if (!quick_tex)
        return;

    beginFrame();
    if (!quick_frame)
        return;
    quick_need_render.storeRelaxed(quick_render->sync() ? 1 : 0);
}

void QmlMainWindow::beginFrame()
{
    if (quick_frame)
        return;

    if (render_backend == RenderBackend::OpenGL) {
        if (!makeOpenGLContextCurrent())
            return;
        quick_frame = true;
        quick_render->beginFrame();
        return;
    }

    struct pl_vulkan_hold_params hold_params = {
        .tex = quick_tex,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .qf = VK_QUEUE_FAMILY_IGNORED,
        .semaphore = {
            .sem = quick_sem,
            .value = ++quick_sem_value,
        }
    };
    pl_vulkan_hold_ex(placebo_vulkan->gpu, &hold_params);

    VkSemaphoreWaitInfo wait_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &quick_sem,
        .pValues = &quick_sem_value,
    };
    vk_funcs.vkWaitSemaphores(placebo_vulkan->device, &wait_info, UINT64_MAX);

    quick_frame = true;
    quick_render->beginFrame();
}

void QmlMainWindow::endFrame()
{
    if (!quick_frame)
        return;

    if (render_backend == RenderBackend::OpenGL) {
        quick_frame = false;
        quick_render->endFrame();
        doneOpenGLContextCurrent();
        return;
    }

    quick_frame = false;
    quick_render->endFrame();

    struct pl_vulkan_release_params release_params = {
        .tex = quick_tex,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .qf = VK_QUEUE_FAMILY_IGNORED,
    };
    pl_vulkan_release_ex(placebo_vulkan->gpu, &release_params);
}

void QmlMainWindow::render()
{
    Q_ASSERT(QThread::currentThread() == render_thread);

    struct RenderActiveGuard {
        QAtomicInteger<int> &flag;
        RenderActiveGuard(QAtomicInteger<int> &flag) : flag(flag) { flag.storeRelease(1); }
        ~RenderActiveGuard() { flag.storeRelease(0); }
    } render_guard(render_active);

    bool schedule_next_update = false;
    auto finalize_render = [this]() {
        bool pending = false;
        {
            QMutexLocker locker(&render_schedule_mutex);
            render_scheduled = false;
            pending = render_pending;
            render_pending = false;
        }
        if (pending) {
            QMetaObject::invokeMethod(this, [this]() {
                update();
            }, Qt::QueuedConnection);
        }
    };
    if (swapchain_recreate_pending.fetchAndStoreRelaxed(0)) {
        destroySwapchain();
        createSwapchain();
        resizeSwapchain();
    }

    // Before anything touches the queue (see performHardReset for the ordering).
    if (hard_reset_pending.loadAcquire() != HardResetNone)
        performHardReset();

    if (placebo_reset_pending.fetchAndStoreRelaxed(0) != 0)
        resetPlaceboQueue();

    if (!placebo_swapchain)
    {
        finalize_render();
        return;
    }

    if (renderer_cache_flush_pending.fetchAndStoreRelaxed(0) && placebo_renderer)
        pl_renderer_flush_cache(placebo_renderer);

    const struct pl_render_params *render_params = &pl_render_default_params;
    const PlaceboUpscaler configured_upscaler = settings->GetPlaceboUpscaler();
    // One read per frame: the GUI thread can switch the preset mid-render.
    const VideoPreset preset = video_preset;
    switch (preset) {
    case VideoPreset::Fast:
        render_params = &pl_render_fast_params;
        break;
    case VideoPreset::Default:
        render_params = &pl_render_default_params;
        pl_options_set_str(this->renderparams_opts, "deinterlace", "yes");
        if (render_params->deinterlace_params)
            this->renderparams_opts->params.deinterlace_params = render_params->deinterlace_params;
        break;
    case VideoPreset::HighQuality:
        render_params = &pl_render_high_quality_params;
        pl_options_set_str(this->renderparams_opts, "deinterlace", "yes");
        if (render_params->deinterlace_params)
            this->renderparams_opts->params.deinterlace_params = render_params->deinterlace_params;
        break;
    case VideoPreset::HighQualitySpatial:
        render_params = &pl_render_high_quality_params;
        pl_options_set_str(this->renderparams_opts, "deinterlace", "yes");
        if (render_params->deinterlace_params)
            this->renderparams_opts->params.deinterlace_params = render_params->deinterlace_params;
        break;
    case VideoPreset::HighQualityAdvancedSpatial:
        render_params = &pl_render_high_quality_params;
        pl_options_set_str(this->renderparams_opts, "deinterlace", "yes");
        if (render_params->deinterlace_params)
            this->renderparams_opts->params.deinterlace_params = render_params->deinterlace_params;
        break;
    case VideoPreset::Custom:
        if (renderparams_changed) {
            renderparams_changed = false;
            QMap<QString, QString> paramsData = settings->GetPlaceboValues();
            QMapIterator<QString, QString> i(paramsData);
            bool invalid_render_params = false;
            {
                QMutexLocker locker(&placebo_state_mutex);
                this->renderparams_opts->params = pl_render_default_params;
                pl_options_set_str(this->renderparams_opts, "deinterlace", "yes");
                if (pl_render_default_params.deinterlace_params)
                    this->renderparams_opts->params.deinterlace_params = pl_render_default_params.deinterlace_params;
                while (i.hasNext()) {
                    i.next();
                    if ((i.key() == QLatin1String("upscaler") || i.key() == QLatin1String("plane_upscaler"))
                        && uses_custom_upscale_hook(configured_upscaler)) {
                        continue;
                    }
                    if(!pl_options_set_str(this->renderparams_opts, i.key().toUtf8().constData(), i.value().toUtf8().constData()))
                    {
                        invalid_render_params = true;
                        qCCritical(chiakiGui) << "Failed to load custom render param: " << i.key() << " with value: " << i.value();
                    }
                }
            }
            if (invalid_render_params)
                qCInfo(chiakiGui) << "Updated custom render parameters with one or more invalid parameters.";
            else
                qCInfo(chiakiGui) << "Updated custom render parameters successfully.";
        }
        render_params = &(this->renderparams_opts->params);
        break;
    }

    struct pl_render_params params = *render_params;
    // NAX5_DEBAND_GRAIN (default 0): the HighQuality presets deband with grain, which
    // costs GPU time and adds noise to an already compressed image. Nothing else changes.
    struct pl_deband_params deband_params_local;
    if (params.deband_params
        && (preset == VideoPreset::HighQuality
            || preset == VideoPreset::HighQualitySpatial
            || preset == VideoPreset::HighQualityAdvancedSpatial)) {
        deband_params_local = *params.deband_params;
        deband_params_local.grain = nax5_deband_grain();
        params.deband_params = &deband_params_local;
    }
    // pl_render_high_quality_params has no plane_upscaler, so 4:2:0 chroma goes through
    // the luma upscaler (ewa_lanczossharp): a full-resolution polar pass before the
    // downscale to the panel. Spline36 is separable and much cheaper; it only affects
    // plane (chroma) resampling, luma scaling and the spatial upscaler hooks are untouched.
    if (nax5_fast_chroma()
        && (preset == VideoPreset::HighQuality
            || preset == VideoPreset::HighQualitySpatial
            || preset == VideoPreset::HighQualityAdvancedSpatial))
        params.plane_upscaler = &pl_filter_spline36;
    if (!params.frame_mixer)
        params.frame_mixer = pl_find_filter_config("oversample", PL_FILTER_FRAME_MIXING);
    // With immediate render every frame is presented as it arrives, so the present
    // rate follows the network rather than vsync. libplacebo then sees the fps ratio
    // hovering around its 0.01 threshold and flips blending on and off every few
    // seconds (~900 times in a 47-minute session); while on, it blends the new frame
    // with the previous one, which costs an extra pass and softens motion.
    const bool mix_frames = frameMixActive(preset);
    if (!mix_frames)
        params.frame_mixer = nullptr;
    qparams.interpolation_threshold = mix_frames ? 0.01 : 1000.0;

    const bool has_settings = settings != nullptr;
    const bool apply_display_target = has_settings && stream_session_active.loadAcquire() != 0;
    const int target_trc = apply_display_target ? settings->GetDisplayTargetTrc() : 0;
    const int target_prim = apply_display_target ? settings->GetDisplayTargetPrim() : 0;
    const int target_peak = apply_display_target ? settings->GetDisplayTargetPeak() : 0;
    const int target_contrast = apply_display_target ? settings->GetDisplayTargetContrast() : 0;

    uint64_t ts_pre_update = chiaki_time_now_monotonic_us();
    double refresh_rate = screen() ? screen()->refreshRate() : 60.0;
    if (refresh_rate <= 1.0)
        refresh_rate = 60.0;
    const bool pending_frame_waiting = hasPendingFrame();
    const bool vsync_on = present_vsync_enabled.loadAcquire() != 0;
    // FIFO: QScreen's rate is stale under gamescope (reports 89.9 Hz on a 60 Hz panel),
    // so use the period measured from the swap intervals once there is one. MAILBOX /
    // IMMEDIATE: unchanged (0, no vsync).
    const int present_mode_now = present_mode_actual.loadRelaxed();
    const bool fifo_mode = present_mode_now == VK_PRESENT_MODE_FIFO_KHR
        || present_mode_now == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    const int measured_period_us = fifo_mode ? panel_period_us.loadRelaxed() : 0;
    const double vsync_duration = !vsync_on ? 0.0
        : measured_period_us > 0 ? measured_period_us / 1000000.0
        : 1.0 / refresh_rate;
    qparams.timeout = 0;
    qparams.radius = pl_frame_mix_radius(&params);
    qparams.vsync_duration = vsync_duration;
    const int depth_limit = pending_frame_waiting ? effectiveQueueDepthLimit() : 0;
    enum pl_queue_status queue_status;
    bool replay_attempted = false;
    bool replay_enqueued = false;
    // frame_mix points into pl_queue's entries until pl_render_image_mix is done; a
    // pl_queue_reset from the GUI thread would free them under us. Held to the end of
    // render() (every return path); lock order frame_mix_mutex -> placebo_state_mutex.
    QMutexLocker frame_mix_locker(&frame_mix_mutex);
    {
        QMutexLocker locker(&placebo_state_mutex);
        // ts_start is also moved by presentFrame/queueStoredFrame, read it under the lock.
        qparams.pts = playback_started
            ? (double)(ts_pre_update - ts_start) / 1000000.0 + vsync_duration
            : 0.0;
        present_window_s = vsync_duration;
        queue_status = pl_queue_update(placebo_queue, &frame_mix, &qparams);
        const bool pending_frame_blocked = pending_frame_waiting
            && pl_queue_num_frames(placebo_queue) >= depth_limit;
        if (frame_mix.num_frames == 0 && (!pending_frame_waiting || pending_frame_blocked)) {
            replay_attempted = true;
            double used_origin = -1.0;
            if (enqueueKeptFrame(queue_pts_origin, this->renderparams_opts->params.deinterlace_params, used_origin)) {
                replay_enqueued = true;
                queue_pts_origin = used_origin;
                if (!playback_started) {
                    uint64_t now_us = chiaki_time_now_monotonic_us();
                    ts_start = now_us;
                    playback_started = true;
                }
                queue_status = pl_queue_update(placebo_queue, &frame_mix, &qparams);
            }
        }
    }
    switch (queue_status) {
    case PL_QUEUE_ERR:
        finalize_render();
        return;
    case PL_QUEUE_EOF:
    case PL_QUEUE_OK:
    case PL_QUEUE_MORE:
        break;
    }
    if (applyPendingFrameIfQueueHasCapacity()) {
        {
            QMutexLocker locker(&placebo_state_mutex);
            queue_status = pl_queue_update(placebo_queue, &frame_mix, &qparams);
        }
        switch (queue_status) {
        case PL_QUEUE_ERR:
            finalize_render();
            return;
        case PL_QUEUE_EOF:
        case PL_QUEUE_OK:
        case PL_QUEUE_MORE:
            break;
        }
    }
    refreshPendingFrameAge();
    // Stable until the swap: every pl_queue_reset needs frame_mix_mutex, held here.
    const quint64 mix_generation = placebo_queue_generation.loadAcquire();

    // Under FIFO every extra present of an unchanged frame takes a swapchain image, and
    // a new frame then waits behind up to two repeats (+17-33 ms). The display keeps
    // showing the last image anyway, so skip the present when nothing would change:
    // same single frame (no blending), no Qt Quick overlay/menu update, same swapchain,
    // same scaling and target settings. Custom presets keep frame mixing, never skipped.
    PresentKey present_key;
    bool present_key_valid = false;
    if (skipRepeatActive() && has_video && !mix_frames && frame_mix.num_frames == 1
        && queue_status != PL_QUEUE_EOF && frame_mix.signatures) {
        present_key.signature = frame_mix.signatures[0];
        present_key.queue_generation = placebo_queue_generation.loadAcquire();
        present_key.preset = preset;
        present_key.mode = video_mode;
        present_key.zoom = zoom_factor;
        present_key.upscaler = static_cast<int>(configured_upscaler);
        present_key.target_trc = target_trc;
        present_key.target_prim = target_prim;
        present_key.target_peak = target_peak;
        present_key.target_contrast = target_contrast;
        present_key.swapchain_size = swapchain_size;
        present_key.vsync = vsync_on;
        present_key_valid = true;
        const bool overlay_dirty = quick_frame
            || quick_need_sync.loadRelaxed() != 0
            || quick_need_render.loadRelaxed() != 0;
        if (last_present_valid && present_key == last_present && !overlay_dirty
            && placebo_reset_pending.loadAcquire() == 0
            && swapchain_recreate_pending.loadRelaxed() == 0) {
            repeat_skips.fetchAndAddRelaxed(1);
            // Frames queued ahead of the shown one become due by pts, not by arrival,
            // so keep polling at the refresh interval then; otherwise the next frame
            // arrives through update() and a slow heartbeat is enough.
            const bool frames_ahead = pl_queue_num_frames(placebo_queue) > 1;
            const bool fast = frames_ahead || pending_frame_waiting || hasPendingFrame();
            finalize_render();
            QMetaObject::invokeMethod(this, [this, fast]() {
                if (fast)
                    scheduleUpdate();
                else
                    scheduleHeartbeatUpdate();
            }, Qt::QueuedConnection);
            return;
        }
        // Diagnostics only (behaviour unchanged): this render shows the same frame as the
        // last swap but is not skipped. One reason each, in this order.
        if (last_swapped_valid && present_key.signature == last_swapped_sig
            && present_key.queue_generation == last_swapped_gen) {
            if (overlay_dirty)
                skip_miss_overlay_current.fetchAndAddRelaxed(1);
            else if (!last_present_valid)
                skip_miss_invalid_current.fetchAndAddRelaxed(1);
            else if (!(present_key == last_present))
                skip_miss_key_current.fetchAndAddRelaxed(1);
            else
                skip_miss_gate_current.fetchAndAddRelaxed(1);
        }
    }

    if (render_backend == RenderBackend::OpenGL && quick_need_render.loadRelaxed()) {
        quick_need_render.storeRelaxed(0);
        beginFrame();
        if (quick_frame) {
            clearQuickOpenGLTarget(quick_fbo);
            quick_render->render();
        }
        endFrame();
    }

    struct pl_swapchain_frame sw_frame = {};
    if (!pl_swapchain_start_frame(placebo_swapchain, &sw_frame)) {
        qCWarning(chiakiGui) << "Failed to start Placebo frame!";
        finalize_render();
        return;
    }
    // Whatever this frame ends up showing, it is no longer the recorded present.
    last_present_valid = false;
    auto close_started_frame = [this, ts_pre_update, mix_generation, fifo_mode](bool present) {
        if (!pl_swapchain_submit_frame(placebo_swapchain)) {
            qCWarning(chiakiGui) << "Failed to submit Placebo frame!";
            return false;
        }
        if (!present)
            return false;
        pl_swapchain_swap_buffers(placebo_swapchain);
        display_frames_current.fetchAndAddRelaxed(1);
        // Stats: render time (queue update -> after swap); for NEW frames only: the gap
        // since the previous new frame's swap (swap_gap_max_ms), late presents (that gap
        // > 1.5 frame durations) and the age since the decoder received the frame's last
        // packet. A new frame is a signature change that isn't a replay of the same
        // decoded frame (replays carry the original's arrival stamp).
        const uint64_t now_us = chiaki_time_now_monotonic_us();
        const int render_us = static_cast<int>(qMin<uint64_t>(now_us - ts_pre_update, INT_MAX));
        nax5_atomic_max(render_us_max_current, render_us);

        // Panel period, FIFO only (swaps are paced by vblank): median swap interval of
        // each ~1 s window; used for vsync_duration in render() once measured.
        if (fifo_mode) {
            if (last_swap_us && now_us > last_swap_us && now_us - last_swap_us < 100000)
                panel_intervals_us.append(static_cast<quint32>(now_us - last_swap_us));
            if (!panel_window_start_us) {
                panel_window_start_us = now_us;
            } else if (now_us - panel_window_start_us >= 1000000) {
                if (panel_intervals_us.size() >= 10) {
                    std::sort(panel_intervals_us.begin(), panel_intervals_us.end());
                    const quint32 median = panel_intervals_us[panel_intervals_us.size() / 2];
                    if (median >= 3000 && median <= 50000)
                        panel_period_us.storeRelaxed(static_cast<int>(median));
                }
                panel_intervals_us.clear();
                panel_window_start_us = now_us;
            }
        } else if (panel_window_start_us) {
            panel_intervals_us.clear();
            panel_window_start_us = 0;
        }
        last_swap_us = now_us;

        const bool has_frame = frame_mix.num_frames > 0 && frame_mix.signatures;
        const uint64_t signature = has_frame ? frame_mix.signatures[0] : 0;
        const AVFrame *av0 = has_frame ? nax5_avframe_of(frame_mix.frames[0]) : nullptr;
        const uint64_t arrival0 = nax5_frame_arrival_us(av0);
        const bool sig_changed = has_frame
            && (!last_new_valid || signature != last_new_sig || mix_generation != last_new_gen);
        const bool is_new = sig_changed && !(arrival0 && arrival0 == last_new_arrival_us);
        if (swap_stats_restart.loadRelaxed() && swap_stats_restart.fetchAndStoreAcquire(0))
            last_new_swap_us = 0;
        if (is_new) {
            if (last_new_swap_us && now_us > last_new_swap_us) {
                const uint64_t gap_us = now_us - last_new_swap_us;
                nax5_atomic_max(swap_gap_us_max_current, static_cast<int>(qMin<uint64_t>(gap_us, INT_MAX)));
                // Frame duration: the frame's own, else 1/max_fps of the session, else 1/60.
                double duration_s = 0.0;
                if (av0 && av0->duration > 0) {
                    AVRational tb = av0->time_base;
                    if (tb.num <= 0 || tb.den <= 0)
                        tb = AVRational{ frame_tb_num.loadRelaxed(), frame_tb_den.loadAcquire() };
                    if (tb.num > 0 && tb.den > 0)
                        duration_s = static_cast<double>(av0->duration) * av_q2d(tb);
                }
                if (duration_s < 0.002 || duration_s > 0.2) {
                    const int max_fps = session_max_fps.loadRelaxed();
                    duration_s = max_fps > 0 ? 1.0 / max_fps : 1.0 / 60.0;
                }
                if (static_cast<double>(gap_us) > 1.5 * duration_s * 1000000.0)
                    late_presents_current.fetchAndAddRelaxed(1);
            }
            last_new_sig = signature;
            last_new_gen = mix_generation;
            last_new_valid = true;
            last_new_swap_us = now_us;
            last_new_arrival_us = arrival0;
        }
        if (has_frame) {
            last_swapped_sig = signature;
            last_swapped_gen = mix_generation;
            last_swapped_valid = true;
        }

        if (is_new) {
            for (int i = 0; i < frame_mix.num_frames; i++) {
                const uint64_t arrival_us = i == 0 ? arrival0 : nax5_frame_arrival_us(nax5_avframe_of(frame_mix.frames[i]));
                if (arrival_us && now_us > arrival_us) {
                    const quint64 age_us = qMin<quint64>(now_us - arrival_us, nax5_age_sum_mask);
                    nax5_atomic_max(present_age_us_max_current, static_cast<int>(qMin<quint64>(age_us, INT_MAX)));
                    present_age_packed_current.fetchAndAddRelaxed((quint64(1) << nax5_age_count_shift) + age_us);
                }
            }
        }

        if (frame_trace_log_active.loadRelaxed() != 0 && has_frame) {
            FrameTraceEntry e;
            e.pts = 0.0;
            if (av0 && av0->pts != AV_NOPTS_VALUE) {
                e.pts = static_cast<double>(av0->pts);
                if (frame_trace_pts_seconds.loadRelaxed()) {
                    AVRational tb = av0->time_base;
                    if (tb.num <= 0 || tb.den <= 0)
                        tb = AVRational{ frame_tb_num.loadRelaxed(), frame_tb_den.loadAcquire() };
                    if (tb.num > 0 && tb.den > 0)
                        e.pts *= av_q2d(tb);
                }
            }
            e.arrival_us = arrival0;
            e.present_us = now_us;
            e.render_ms = render_us / 1000.0f;
            e.queue_depth = pl_queue_num_frames(placebo_queue);
            // Signatures restart with every queue reset: compare with the generation.
            e.repeated = signature == last_trace_signature && mix_generation == last_trace_generation ? 1 : 0;
            last_trace_signature = signature;
            last_trace_generation = mix_generation;
            QMutexLocker locker(&frame_trace_mutex);
            if (frame_trace_ring.size() < 4096)
                frame_trace_ring.append(e);
            else
                frame_trace_dropped++;
        }
        return true;
    };

    const struct pl_frame *hint_frame = nullptr;
    for (int i = 0; i < frame_mix.num_frames; i++) {
        if (frame_mix.timestamps[i] > 0.0f)
            break;
        hint_frame = frame_mix.frames[i];
    }
    if (!hint_frame && frame_mix.num_frames)
        hint_frame = frame_mix.frames[0];
    struct pl_color_space hint = hint_frame ? hint_frame->color : pl_color_space{};
    struct pl_color_space target_csp = sw_frame.color_space;
    if (target_csp.hdr.max_luma <= 0.0f) {
        target_csp.hdr.max_luma = 0.0f;
        target_csp.hdr.min_luma = 0.0f;
    }

    if(target_trc)
        hint.transfer = static_cast<pl_color_transfer>(target_trc);

    if(target_prim)
    {
        hint.primaries = static_cast<pl_color_primaries>(target_prim);
        hint.hdr.prim = *pl_raw_primaries_get(hint.primaries);
    }

    if(target_peak)
        hint.hdr.max_luma = target_peak;

    switch(target_contrast)
    {
        case -1:
            hint.hdr.min_luma = 1e-7;
            break;
        case 0:
            hint.hdr.min_luma = target_csp.hdr.min_luma;
            break;
        default:
            // Infer max_luma for current pl_color_space
            struct pl_nominal_luma_params hint_params = {};
            hint_params.color = &hint;
            hint_params.metadata = PL_HDR_METADATA_HDR10;
            hint_params.scaling = PL_HDR_NITS;
            hint_params.out_max = &hint.hdr.max_luma;
            pl_color_space_nominal_luma_ex(&hint_params);
            hint.hdr.min_luma = hint.hdr.max_luma / (float)target_contrast;
            break;
    }
    pl_swapchain_colorspace_hint(placebo_swapchain, &hint);

    struct pl_frame target_frame = {};
    pl_frame_from_swapchain(&target_frame, &sw_frame);
    if (target_frame.num_planes <= 0) {
        qCWarning(chiakiGui) << "Swapchain frame contains no planes, skipping render";
        close_started_frame(false);
        finalize_render();
        return;
    }
    struct pl_overlay_part overlay_part = {
        .src = {0, 0, static_cast<float>(swapchain_size.width()), static_cast<float>(swapchain_size.height())},
        .dst = {0, 0, static_cast<float>(swapchain_size.width()), static_cast<float>(swapchain_size.height())},
    };
    if (render_backend == RenderBackend::OpenGL) {
        overlay_part.src.y0 = static_cast<float>(swapchain_size.height());
        overlay_part.src.y1 = 0.0f;
    }
    struct pl_color_repr overlay_repr = pl_color_repr_rgb;
    overlay_repr.alpha = PL_ALPHA_PREMULTIPLIED;
    struct pl_overlay overlay = {
        .tex = quick_tex,
        .repr = overlay_repr,
        .color = pl_color_space_srgb,
        .parts = &overlay_part,
        .num_parts = 1,
    };
    target_frame.overlays = &overlay;
    target_frame.num_overlays = 1;
    if(target_prim)
    {
        target_frame.color.primaries = static_cast<pl_color_primaries>(target_prim);
        target_frame.color.hdr.prim = *pl_raw_primaries_get(target_frame.color.primaries);
    }

    if(target_trc)
        target_frame.color.transfer = static_cast<pl_color_transfer>(target_trc);

    if(target_peak && !target_frame.color.hdr.max_luma)
    {
        target_frame.color.hdr.max_luma = target_peak;
    }
    if(!target_frame.color.hdr.min_luma)
    {
        switch(target_contrast)
        {
            case -1:
                target_frame.color.hdr.min_luma = 1e-7;
                break;
            case 0:
                target_frame.color.hdr.min_luma = target_csp.hdr.min_luma;
                break;
            default:
                // Infer max_luma for current pl_color_space
                struct pl_nominal_luma_params target_params = {};
                target_params.color = &target_frame.color;
                target_params.metadata = PL_HDR_METADATA_HDR10;
                target_params.scaling = PL_HDR_NITS;
                target_params.out_max = &target_frame.color.hdr.max_luma;
                pl_color_space_nominal_luma_ex(&target_params);
                target_frame.color.hdr.min_luma = target_frame.color.hdr.max_luma / (float)target_contrast;
                break;
        }
    }
    if (quick_need_render.loadRelaxed()) {
        quick_need_render.storeRelaxed(0);
        beginFrame();
        if (quick_frame) {
            if (render_backend == RenderBackend::OpenGL)
                clearQuickOpenGLTarget(quick_fbo);
            quick_render->render();
        }
    }
    endFrame();

    if (frame_mix.num_frames == 0) {
        if (replay_attempted && replay_enqueued) {
            close_started_frame(false);
            schedule_next_update = true;
            finalize_render();
            QMetaObject::invokeMethod(this, [this]() {
                scheduleUpdate();
            }, Qt::QueuedConnection);
            return;
        }

        const bool rendered = pl_render_image(placebo_renderer, nullptr, &target_frame, &params);
        if (!rendered) {
            qCWarning(chiakiGui) << "Failed to render held/overlay-only frame";
            close_started_frame(false);
            finalize_render();
            return;
        }

        close_started_frame(true);
        schedule_next_update = (has_video && queue_status != PL_QUEUE_EOF)
            || pending_frame_waiting
            || quick_need_sync.loadRelaxed() != 0
            || quick_need_render.loadRelaxed() != 0
            || placebo_reset_pending.loadAcquire() != 0;
        finalize_render();
        if (schedule_next_update) {
            QMetaObject::invokeMethod(this, [this]() {
                scheduleUpdate();
            }, Qt::QueuedConnection);
        }
        return;
    }

    if (hint_frame) {
        pl_rect2df crop = hint_frame->crop;
        switch (video_mode) {
        case VideoMode::Normal:
            pl_rect2df_aspect_copy(&target_frame.crop, &crop, 0.0);
            break;
        case VideoMode::Stretch:
            // Nothing to do, target.crop already covers the full image
            break;
        case VideoMode::Zoom:
            if(zoom_factor == -1)
                pl_rect2df_aspect_copy(&target_frame.crop, &crop, 1.0);
            else
            {
                const float z = powf(2.0f, zoom_factor);
                const float sx = z * fabsf(pl_rect_w(crop)) / pl_rect_w(target_frame.crop);
                const float sy = z * fabsf(pl_rect_h(crop)) / pl_rect_h(target_frame.crop);
                pl_rect2df_stretch(&target_frame.crop, sx, sy);
            }
            break;
        }
    }
    // Disable background transparency by default if the swapchain does not
    // appear to support alpha transaprency
    if (sw_frame.color_repr.alpha == PL_ALPHA_NONE)
        params.background_transparency = 0.0;

    const struct pl_hook *fsrcnnx_hook = nullptr;
    if (hint_frame) {
        const float src_width = fabsf(pl_rect_w(hint_frame->crop));
        const float src_height = fabsf(pl_rect_h(hint_frame->crop));
        const float dst_width = fabsf(pl_rect_w(target_frame.crop));
        const float dst_height = fabsf(pl_rect_h(target_frame.crop));
        if (src_width > 0.0f && src_height > 0.0f) {
            const float upscale_factor = qMin(dst_width / src_width, dst_height / src_height);
            fsrcnnx_hook = select_spatial_hook(video_preset, configured_upscaler, upscale_factor,
                                               fsr_hook, fsrcnnx_hook_8, fsrcnnx_hook_16);
        }
    }
    const struct pl_hook *active_hooks[] = {fsrcnnx_hook};
    const bool hook_changed = fsrcnnx_hook != last_spatial_hook_logged;
    last_spatial_hook_logged = fsrcnnx_hook;
    if (fsrcnnx_hook) {
        params.hooks = active_hooks;
        params.num_hooks = 1;
        // Logged once per change of hook (the choice depends on the upscale factor).
        if (hook_changed) {
            const char *name = nullptr;
            switch (video_preset) {
            case VideoPreset::HighQualitySpatial:
                name = fsrcnnx_hook == fsr_hook ? "spatial upscaler: FSR (small upscale fallback)"
                                                : "spatial upscaler: FSRCNNX x2 8-0-4-1";
                break;
            case VideoPreset::HighQualityAdvancedSpatial:
                name = fsrcnnx_hook == fsr_hook ? "spatial upscaler: FSR (small upscale fallback)"
                                                : "spatial upscaler: FSRCNNX x2 16-0-4-1";
                break;
            case VideoPreset::Custom:
                if (fsrcnnx_hook == fsr_hook)
                    name = "custom upscaler: FSR";
                else if (configured_upscaler == PlaceboUpscaler::FSRCNNX8)
                    name = "custom upscaler: FSRCNNX x2 8-0-4-1";
                else if (configured_upscaler == PlaceboUpscaler::FSRCNNX16)
                    name = "custom upscaler: FSRCNNX x2 16-0-4-1";
                break;
            default:
                break;
            }
            if (name)
                qCInfo(chiakiGui).noquote() << "Activating" << name;
        }
    }

    for (int i = 0; i < frame_mix.num_frames; i++) {
        const struct pl_frame *mix_frame = frame_mix.frames[i];
        if (!mix_frame || mix_frame->num_planes <= 0) {
            qCWarning(chiakiGui) << "Frame mix contains an invalid frame, skipping render";
            if (placebo_reset_pending.loadAcquire() == 0)
                schedulePlaceboReset();
            close_started_frame(false);
            finalize_render();
            return;
        }
    }

    if (!pl_render_image_mix(placebo_renderer, &frame_mix, &target_frame, &params))
    {
        qCWarning(chiakiGui) << "Failed to render Placebo frame!";
        close_started_frame(false);
        finalize_render();
        return;
    }

    if (close_started_frame(true) && present_key_valid) {
        last_present = present_key;
        last_present_valid = true;
    }

    schedule_next_update = (has_video && queue_status != PL_QUEUE_EOF)
        || pending_frame_waiting
        || quick_need_sync.loadRelaxed() != 0
        || quick_need_render.loadRelaxed() != 0
        || placebo_reset_pending.loadAcquire() != 0;

    finalize_render();
    if (schedule_next_update) {
        QMetaObject::invokeMethod(this, [this]() {
            scheduleUpdate();
        }, Qt::QueuedConnection);
    }

}

bool QmlMainWindow::handleShortcut(QKeyEvent *event)
{
    if (event->modifiers() == Qt::NoModifier) {
        switch (event->key()) {
        case Qt::Key_F11:
            if (windowState() != Qt::WindowFullScreen)
                fullscreenTime();
            else
                normalTime();
            return true;
        default:
            break;
        }
    }

    if (!event->modifiers().testFlag(Qt::ControlModifier))
        return false;

    switch (event->key()) {
    case Qt::Key_S:
        if (has_video)
            setVideoMode(videoMode() == VideoMode::Stretch ? VideoMode::Normal : VideoMode::Stretch);
        return true;
    case Qt::Key_Z:
        if (has_video)
            setVideoMode(videoMode() == VideoMode::Zoom ? VideoMode::Normal : VideoMode::Zoom);
        return true;
    case Qt::Key_M:
        if (session)
            session->ToggleMute();
        return true;
    case Qt::Key_O:
        emit menuRequested();
        return true;
    case Qt::Key_I:
        emit streamStatsToggleRequested();
        return true;
    case Qt::Key_Q:
#ifndef Q_OS_MACOS
        close();
#endif
        return true;
    default:
        return false;
    }
}

bool QmlMainWindow::amdCard() const
{
    return amd_card;
}

bool QmlMainWindow::nvidiaCard() const
{
    return nvidia_card;
}

bool QmlMainWindow::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
        if (static_cast<QMouseEvent*>(event)->source() != Qt::MouseEventNotSynthesized)
            return true;
        if (session && !grab_input) {
            if (event->type() == QEvent::MouseMove)
                session->HandleMouseMoveEvent(static_cast<QMouseEvent*>(event), width(), height());
            else if (event->type() == QEvent::MouseButtonPress)
                session->HandleMousePressEvent(static_cast<QMouseEvent*>(event));
            else if (event->type() == QEvent::MouseButtonRelease)
                session->HandleMouseReleaseEvent(static_cast<QMouseEvent*>(event));
            return true;
        }
        QGuiApplication::sendEvent(quick_window, event);
        break;
    case QEvent::MouseButtonDblClick:
        if(!settings->GetFullscreenDoubleClickEnabled())
            break;
        if (session && !grab_input) {
            if (windowState() != Qt::WindowFullScreen)
                fullscreenTime();
            else
                normalTime();
        }
        break;
    case QEvent::KeyPress:
        if (handleShortcut(static_cast<QKeyEvent*>(event)))
            return true;
    case QEvent::KeyRelease:
        if (session && !grab_input) {
            QKeyEvent *e = static_cast<QKeyEvent*>(event);
            if (!e->spontaneous()) {
                QGuiApplication::sendEvent(quick_window, event);
                return true;
            }
            if (e->timestamp())
                session->HandleKeyboardEvent(e);
            return true;
        }
        QGuiApplication::sendEvent(quick_window, event);
        break;
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
        if (session && !grab_input) {
            session->HandleTouchEvent(static_cast<QTouchEvent*>(event), width(), height());
            return true;
        }
        QGuiApplication::sendEvent(quick_window, event);
        break;
    case QEvent::Close:
        if (!backend->closeRequested()) {
            event->ignore();
            return true;
        }
        if (quick_render->thread() == QThread::currentThread())
            destroySwapchain();
        else
            QMetaObject::invokeMethod(quick_render, std::bind(&QmlMainWindow::destroySwapchain, this), Qt::BlockingQueuedConnection);
        break;
    default:
        break;
    }

    bool ret = QWindow::event(event);

    switch (event->type()) {
    case QEvent::Expose:
        if (isExposed())
            updateSwapchain();
        else
            if (quick_render->thread() == QThread::currentThread())
                destroySwapchain();
            else
                QMetaObject::invokeMethod(quick_render, std::bind(&QmlMainWindow::destroySwapchain, this), Qt::BlockingQueuedConnection);
        break;
    case QEvent::Move:
        if(!session && isWindowAdjustable())
            settings->SetGeometry(geometry());
        else if(session && settings->GetWindowType() == WindowType::AdjustableResolution && isStreamWindowAdjustable())
            settings->SetStreamGeometry(geometry());
        break;
    case QEvent::Resize:
        if(!session && isWindowAdjustable())
            settings->SetGeometry(geometry());
        else if(session && settings->GetWindowType() == WindowType::AdjustableResolution && isStreamWindowAdjustable())
            settings->SetStreamGeometry(geometry());
        if (isExposed())
            updateSwapchain();
        break;
    default:
        break;
    }

    return ret;
}

QObject *QmlMainWindow::focusObject() const
{
    return quick_window->focusObject();
}
