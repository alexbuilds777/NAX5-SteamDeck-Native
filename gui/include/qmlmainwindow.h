#pragma once

#include "streamsession.h"
#include "settings.h"

#include <QMutex>
#include <QRecursiveMutex>
#include <QAtomicInteger>
#include <QVector>
#include <QWindow>
#include <QElapsedTimer>
#include <QQuickWindow>
#include <QLoggingCategory>
#include <atomic>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libplacebo/opengl.h>
#include <libplacebo/options.h>
#include <libplacebo/vulkan.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/utils/frame_queue.h>
#include <libplacebo/log.h>
#include <libplacebo/cache.h>
}

#include <vulkan/vulkan.h>
#if defined(Q_OS_LINUX)
#include <xcb/xcb.h>
#include <vulkan/vulkan_xcb.h>
#include <vulkan/vulkan_wayland.h>
#elif defined(Q_OS_MACOS)
#include <vulkan/vulkan_metal.h>
#elif defined(Q_OS_WIN)
#include <vulkan/vulkan_win32.h>
#endif

Q_DECLARE_LOGGING_CATEGORY(chiakiGui);

class Settings;
class StreamSession;
class QmlBackend;
class QOffscreenSurface;
class QOpenGLContext;
class QOpenGLFramebufferObject;

class QFile;

class QmlMainWindow : public QWindow
{
    Q_OBJECT
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY hasVideoChanged)
    Q_PROPERTY(int droppedFrames READ droppedFrames NOTIFY droppedFramesChanged)
    Q_PROPERTY(bool keepVideo READ keepVideo WRITE setKeepVideo NOTIFY keepVideoChanged)
    Q_PROPERTY(VideoMode videoMode READ videoMode WRITE setVideoMode NOTIFY videoModeChanged)
    Q_PROPERTY(float ZoomFactor READ zoomFactor WRITE setZoomFactor NOTIFY zoomFactorChanged)
    Q_PROPERTY(VideoPreset videoPreset READ videoPreset WRITE setVideoPreset NOTIFY videoPresetChanged)
    Q_PROPERTY(bool directStream READ directStream NOTIFY directStreamChanged)
    Q_PROPERTY(int runtimeRendererBackend READ runtimeRendererBackend CONSTANT)
    Q_PROPERTY(double queueDepthAverage READ queueDepthAverage NOTIFY queueDepthAverageChanged)
    Q_PROPERTY(double pendingFrameAge READ pendingFrameAge NOTIFY pendingFrameAgeChanged)
    Q_PROPERTY(int streamFps READ streamFps NOTIFY fpsChanged)
    Q_PROPERTY(int displayFps READ displayFps NOTIFY fpsChanged)
    Q_PROPERTY(QString streamResolution READ streamResolution NOTIFY fpsChanged)

public:
    enum class VideoMode {
        Normal,
        Stretch,
        Zoom
    };
    Q_ENUM(VideoMode);

    enum class VideoPreset {
        Fast,
        Default,
        HighQuality,
        HighQualitySpatial,
        HighQualityAdvancedSpatial,
        Custom
    };
    Q_ENUM(VideoPreset);

    QmlMainWindow(Settings *settings,  bool exit_app_on_stream_exit = false);
    QmlMainWindow(const StreamSessionConnectInfo &connect_info);
    ~QmlMainWindow();
    void updateWindowType(WindowType type);
    void setSettings(Settings *new_settings);

    bool hasVideo() const;
    int droppedFrames() const;
    void increaseDroppedFrames();
    // A frame culled by pl_queue (discard callback): counted as dropped and as queue_culls.
    void increaseQueueCulls();
    int streamFps() const { return stream_fps; }
    int displayFps() const { return display_fps; }
    QString streamResolution() const { return stream_resolution; }

    bool directStream() const;
    int runtimeRendererBackend() const { return static_cast<int>(render_backend); }
    // SIGTERM: stop the stream without asking, then close (see main.cpp).
    void requestTerminate();

    bool keepVideo() const;
    void setKeepVideo(bool keep);

    VideoMode videoMode() const;
    void setVideoMode(VideoMode mode);

    float zoomFactor() const;
    void setZoomFactor(float factor);

    double queueDepthAverage() const;
    double pendingFrameAge() const;

    bool amdCard() const;
    bool nvidiaCard() const;
    bool wasMaximized() const { return was_maximized; };
    bool isWindowAdjustable() const { return is_window_adjustable; }
    void setWindowAdjustable(bool adjustable) { is_window_adjustable = adjustable; }

    void fullscreenTime();
    void normalTime();

    bool isStreamWindowAdjustable() { return is_stream_window_adjustable; }
    void setStreamWindowAdjustable(bool adjustable) { is_stream_window_adjustable = adjustable; }

    QmlBackend *getBackend();

    VideoPreset videoPreset() const;
    void setVideoPreset(VideoPreset mode);

    Q_INVOKABLE void grabInput();
    Q_INVOKABLE void releaseInput();

public slots:
    void resetPlaceboQueue();
    void schedulePlaceboReset();
    void queuePlaceboReset(bool preserve_timeline);

    void updatePlacebo();
    void updateVSync();
    void show();
    void presentFrame(ChiakiFfmpegFrame frame, int32_t frames_lost);

    AVBufferRef *vulkanHwDeviceCtx();

signals:
    void hasVideoChanged();
    void droppedFramesChanged();
    void keepVideoChanged();
    void videoModeChanged();
    void zoomFactorChanged();
    void videoPresetChanged();
    void menuRequested();
    void streamStatsToggleRequested();
    void fpsChanged();
    void directStreamChanged();
    void queueDepthAverageChanged();
    void pendingFrameAgeChanged();

private:
    bool makeOpenGLContextCurrent();
    void doneOpenGLContextCurrent();

    void init(Settings *settings, bool exit_app_on_stream_exit = false);
    pl_gpu placeboGpu() const;
    void update();
    void scheduleUpdate();
    void scheduleHeartbeatUpdate();
    void saveShaderCache();
    void createSwapchain();
    void destroySwapchain();
    void resizeSwapchain();
    void updateSwapchain();
    void sync();
    void beginFrame();
    void endFrame();
    void render();
    void applyPendingFrame();
    bool applyPendingFrameIfQueueHasCapacity();
    bool hasPendingFrame() const;
    int effectiveQueueDepthLimit() const;
    bool storePendingFrame(ChiakiFfmpegFrame &frame, bool take_ownership = false);
    bool storeResetSeedFrame(const AVFrame *frame, double pts, float duration, quint64 generation);
    bool storeResetSeedFromPendingFrame(quint64 generation);
    bool applyKeptFrameSnapshot();
    bool applyResetSeedFrame(quint64 generation);
    bool queueStoredFrame(AVFrame *frame, double pts, float duration,
                          void (*discard_cb)(const struct pl_source_frame *));
    void refreshPendingFrameAge();
    void snapshotPendingFrame();
    bool handleShortcut(QKeyEvent *event);
    bool event(QEvent *event) override;
    QObject *focusObject() const override;
    void updateQueueDepthAverage(int depth);
    void updatePendingFrameAge(double age);
    void snapshotLastFrame(AVFrame *frame, double pts, float duration, bool take_ownership = false);
    void clearSnapshotFrame();
    bool enqueueKeptFrame(double queue_pts_origin_hint, bool deinterlace_enabled, double &used_origin);
    bool has_video = false;
    struct pl_queue_params qparams;
    struct pl_frame_mix frame_mix;
    uint64_t ts_start = 0;
    double queue_pts_origin = -1.0;
    double newest_queued_frame_pts = -1.0;
    bool playback_started = false;
    bool preserve_playback_timeline = false;
    bool was_maximized = false;
    bool amd_card = false;
    bool nvidia_card = false;
    bool direct_stream = false;
    bool keep_video = false;
    RenderBackend render_backend = RenderBackend::Vulkan;
    int grab_input = 0;
    int dropped_frames = 0;
    bool is_window_adjustable = false;
    bool is_stream_window_adjustable = false;
    QAtomicInteger<int> dropped_frames_current = 0;
    // Decoded frames arriving from the stream and frames presented to the display,
    // counted per second for the stats overlay.
    QAtomicInteger<int> stream_frames_current = 0;
    QAtomicInteger<int> display_frames_current = 0;
    QAtomicInteger<int> stream_frame_width = 0;
    QAtomicInteger<int> stream_frame_height = 0;
    int stream_fps = 0;
    int display_fps = 0;
    QString stream_resolution;
    // Per-second CSV of stream health, ~/.local/share/NAX5/NAX5/stream-stats/<start>.csv,
    // useful to pick a bitrate for a given network.
    QFile *stats_log = nullptr;
    QElapsedTimer stats_log_clock;
    bool stats_log_failed = false; // open failed for this session, don't retry every second
    int stats_log_frames_lost = 0;
    void writeStreamStatsRow(int stream_fps, int display_fps, int dropped, const QString &resolution);
    void closeStreamStatsLog();
    // Previous-second baselines for the per-second deltas of the new CSV columns
    // (GUI thread only, captured when the log is opened).
    struct StatsBaseline {
        ChiakiNax5Counters counters = {};
        quint32 decoder_drops = 0;
        quint32 fb_sends = 0;
        quint64 audio_dropped_bytes = 0;
        quint32 audio_starved = 0; // Nax5AudioStats::starved_polls (cumulative)
        quint64 queue_generation = 0;
        bool panel_period_noted = false; // "# note panel_period_ms=" written for this log
        quint64 cpu_total = 0, cpu_idle = 0;
        quint64 proc_ticks = 0;
        qint64 udp_rcvbuf_err = -1;
        qint64 elapsed_ms = 0;
        double screen_hz = 0.0;
    } stats_base;
    // Per-second samples and running totals for the "# summary" lines (GUI thread only).
    struct StatsAccum {
        int rows = 0;
        QVector<float> stream_fps, display_fps, loss_pct, audio_ms, present_age_ms, console_rtt_ms, ping_ms, wifi_dbm, apu_w;
        // stream_fps of the rows from 5 s after the first decoded frame on (startup excluded).
        QVector<float> stream_fps_steady;
        qint64 first_frame_ms = -1; // t of the first row with stream_fps > 0
        quint64 frames_lost = 0, fec_ok = 0, fec_failed = 0, decoder_drops = 0, queue_culls = 0, queue_resets = 0,
                late_presents = 0, catch_ups = 0, repeat_skips = 0, audio_plc = 0;
        double audio_trim_ms = 0.0, render_ms_max = 0.0, swap_gap_max_ms = 0.0, apu_temp_c_max = -1.0;
        int bat_pct_start = -1, bat_pct_end = -1;
        int has_b_frames = -1;
    } stats_acc;
    QString stats_quit_reason;            // from StreamSession::SessionQuit, for the summary
    QString stats_present_mode_header;    // present mode written into the header
    void writeStreamStatsSummary(QFile *file, const StatsAccum &acc, qint64 duration_ms);
    // Render-thread counters for the CSV, reset by the row writer every second.
    QAtomicInteger<int> queue_culls_current = 0;
    QAtomicInteger<int> late_presents_current = 0;
    QAtomicInteger<int> swap_gap_us_max_current = 0;
    QAtomicInteger<int> render_us_max_current = 0;
    QAtomicInteger<int> present_age_us_max_current = 0;
    // Sum (low 40 bits, microseconds) and count (high 24 bits) of the present ages in ONE
    // atomic, so the row writer takes a consistent pair with a single fetchAndStore.
    QAtomicInteger<quint64> present_age_packed_current = 0;
    // Why a render that would repeat the last present was not skipped (NAX5_SKIP_REPEAT
    // diagnostics; exclusive, checked in this order: overlay, invalid, key, gate).
    QAtomicInteger<int> skip_miss_overlay_current = 0;
    QAtomicInteger<int> skip_miss_key_current = 0;
    QAtomicInteger<int> skip_miss_invalid_current = 0;
    QAtomicInteger<int> skip_miss_gate_current = 0;
    // Qt Quick scene updates requested this second (sceneChanged + renderRequested of
    // quick_render, GUI thread): CSV column qml_updates. Each one costs a Qt Quick
    // sync/render and makes the next repeat present unskippable (skip_miss_overlay).
    QAtomicInteger<int> qml_updates_current = 0;
    // Render thread only: the frame of the last successful swap, and the last swap that
    // showed a NEW frame (signature changed, not a replay of the same decoded frame).
    uint64_t last_swapped_sig = 0;
    quint64 last_swapped_gen = 0;
    bool last_swapped_valid = false;
    uint64_t last_new_sig = 0;
    quint64 last_new_gen = 0;
    bool last_new_valid = false;
    uint64_t last_new_swap_us = 0;
    uint64_t last_new_arrival_us = 0;
    // Set by the GUI thread when the stats log opens: the next new frame starts a fresh
    // gap measurement (no gap back to the previous session's last frame).
    QAtomicInteger<int> swap_stats_restart = 0;
    // Panel period measured from swap intervals while the swapchain is FIFO(_RELAXED)
    // (QScreen reports a stale rate under gamescope). Render thread writes, GUI reads.
    QAtomicInteger<int> panel_period_us = 0;
    uint64_t last_swap_us = 0;            // render thread only (any swap)
    uint64_t panel_window_start_us = 0;   // render thread only
    QVector<quint32> panel_intervals_us;  // render thread only
    // Set by the GUI thread when the stats log opens (0 = unknown): the decoder's
    // pts time base and the session's max fps, for the render-thread statistics.
    QAtomicInteger<int> frame_tb_num = 0;
    QAtomicInteger<int> frame_tb_den = 0;
    QAtomicInteger<int> session_max_fps = 0;
    // Deferred pl_queue_reset (ordering: see performHardReset()): set by the GUI thread
    // when frame_mix_mutex could not be taken within 50 ms, cleared by the render thread
    // after the reset (or by the GUI thread while holding frame_mix_mutex, after doing
    // an equivalent reset itself).
    enum HardReset : int { HardResetNone = 0, HardResetRollback = 1, HardResetSessionEnd = 2 };
    QAtomicInteger<int> hard_reset_pending = HardResetNone;
    void performHardReset(); // render thread
    void parkFrameForHardReset(ChiakiFfmpegFrame &frame); // GUI thread
    // Per-frame trace (NAX5_FRAME_TRACE=1): the render thread appends at swap time, the
    // 1 s timer flushes into <stats csv>_frames.csv.
    struct FrameTraceEntry {
        double pts;
        uint64_t arrival_us;
        uint64_t present_us;
        float render_ms;
        int queue_depth;
        int repeated;
    };
    QMutex frame_trace_mutex;
    QVector<FrameTraceEntry> frame_trace_ring; // guarded by frame_trace_mutex
    int frame_trace_dropped = 0;               // guarded by frame_trace_mutex
    QFile *frame_trace_log = nullptr;          // GUI thread
    QAtomicInteger<int> frame_trace_log_active = 0; // render thread checks before appending
    QAtomicInteger<int> frame_trace_pts_seconds = 0; // column is pts (s) rather than pts_raw
    uint64_t last_trace_signature = ~0ULL;     // render thread only
    quint64 last_trace_generation = ~0ULL;     // render thread only (signatures restart per queue)
    void flushFrameTrace();
    bool going_full = false;
    VideoMode video_mode = VideoMode::Normal;
    float zoom_factor = 0;
    VideoPreset video_preset = VideoPreset::HighQuality;
    Settings *settings = {};

    QmlBackend *backend = {};
    bool terminating = false;
    StreamSession *session = {};
    AVBufferRef *vulkan_hw_dev_ctx = nullptr;
    // Written on the GUI thread (presentFrame, under placebo_state_mutex), read by the
    // render thread (effectiveQueueDepthLimit) and the stats writer.
    std::atomic<double> queue_depth_average{0.0};
    double pending_frame_age = 0.0;
    // Values last announced to QML (queueDepthAverageChanged / pendingFrameAgeChanged),
    // in the units the stats panel shows: depth x10 (toFixed(1)) and age in ms (toFixed(0)).
    // GUI thread only (overlay_values_timer).
    int overlay_notified_depth_x10 = -1; // -1: never announced (values are >= 0)
    int overlay_notified_age_ms = -1;
    uint64_t last_placebo_reset_ts = 0;
    uint64_t pending_frame_stored_us = 0;
    mutable QMutex pending_frame_age_mutex;

    pl_cache placebo_cache = {};
    pl_log placebo_log = {};
    pl_vk_inst placebo_vk_inst = {};
    pl_vulkan placebo_vulkan = {};
    pl_opengl placebo_opengl = {};
    pl_swapchain placebo_swapchain = {};
    pl_renderer placebo_renderer = {};
    pl_queue placebo_queue = {};
    std::array<pl_tex, 8> placebo_tex{};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    int vk_decode_queue_index = -1;
    QSize swapchain_size;
    QThread *render_thread = {};
    bool owns_render_thread = false;
    QMutex render_schedule_mutex;
    // Lock order: frame_mix_mutex -> placebo_state_mutex (never the other way round).
    // frame_mix_mutex protects the pl_frame entries that pl_queue_update hands out in
    // frame_mix: the render thread holds it from the first pl_queue_update until after
    // pl_render_image_mix/swap, every pl_queue_reset (which frees those entries) takes it.
    // Recursive because render() re-enters through applyPendingFrame -> queueStoredFrame.
    QRecursiveMutex frame_mix_mutex;
    QMutex placebo_state_mutex;
    bool render_scheduled = false;
    bool render_pending = false;
    QMutex pending_frame_mutex;
    AVFrame *pending_frame = nullptr;
    double pending_pts = 0.0;
    float pending_duration = 0.0f;
    double pending_frame_queue_origin = 0.0;
    QAtomicInteger<int> pending_frame_present = 0;
    QMutex reset_seed_mutex;
    AVFrame *reset_seed_frame = nullptr;
    double reset_seed_pts = 0.0;
    float reset_seed_duration = 0.0f;
    quint64 reset_seed_generation = 0;
    QAtomicInteger<quint64> snapshot_generation = 0;
    QAtomicInteger<quint64> pending_reset_snapshot_generation = 0;
    QAtomicInteger<quint64> last_reset_snapshot_generation = 0;
    QAtomicInteger<int> stream_session_active = 0;
    QMutex kept_frame_mutex;
    AVFrame *kept_frame = nullptr;
    double kept_frame_pts = 0.0;
    float kept_frame_duration = 0.0f;
    AVFrame *fallback_frame = nullptr;
    AVFrame *keptReplaySourceLocked(); // kept_frame_mutex must be held
    void prepareReplayFallback();
    quint64 fallback_attempt_generation = 0; // guarded by kept_frame_mutex
    QAtomicInteger<int> catch_up_count = 0;
    double present_window_s = 1.0 / 60.0; // guarded by placebo_state_mutex
    bool immediateRenderActive() const;
    bool frameMixActive(VideoPreset preset) const;
    bool skipRepeatActive() const;
    void requestFrameRender();
    void anchorPlaybackLocked(double src_pts, float duration); // placebo_state_mutex must be held
    QAtomicInteger<int> swapchain_recreate_pending = 0;
    QAtomicInteger<int> renderer_cache_flush_pending = 0;
    QAtomicInteger<int> placebo_reset_pending = 0;
    QAtomicInteger<int> placebo_reset_preserve_timeline = 0;
    QAtomicInteger<int> reset_seed_capture_active = 0;
    QAtomicInteger<quint64> reset_seed_capture_generation = 0;
    QAtomicInteger<quint64> placebo_reset_throttle_generation = 0;
    QAtomicInteger<int> render_active = 0;
    // Set from the ACTUAL swapchain present mode (render thread), read by render() and
    // the stats writer (GUI thread).
    QAtomicInteger<int> present_vsync_enabled = 1;
    // Actual VkPresentModeKHR of the current swapchain, -1 before the first one / OpenGL.
    QAtomicInteger<int> present_mode_actual = -1;
    const struct pl_hook *last_spatial_hook_logged = nullptr; // render thread only
    // Everything that decides what a present shows. render() skips the present when
    // it would show exactly what the last one did (NAX5_SKIP_REPEAT).
    struct PresentKey {
        uint64_t signature = 0; // pl_queue frame signature, restarts at 0 after pl_queue_reset
        quint64 queue_generation = 0;
        VideoPreset preset = VideoPreset::HighQuality;
        VideoMode mode = VideoMode::Normal;
        float zoom = 0.0f;
        int upscaler = 0;
        int target_trc = 0;
        int target_prim = 0;
        int target_peak = 0;
        int target_contrast = 0;
        QSize swapchain_size;
        bool vsync = true;
        bool operator==(const PresentKey &o) const
        {
            return signature == o.signature && queue_generation == o.queue_generation
                && preset == o.preset && mode == o.mode && zoom == o.zoom
                && upscaler == o.upscaler && target_trc == o.target_trc
                && target_prim == o.target_prim && target_peak == o.target_peak
                && target_contrast == o.target_contrast && swapchain_size == o.swapchain_size
                && vsync == o.vsync;
        }
    };
    PresentKey last_present; // render thread only
    bool last_present_valid = false; // render thread only
    // Bumped with every pl_queue_reset (under placebo_state_mutex), which restarts signatures.
    QAtomicInteger<quint64> placebo_queue_generation = 0;
    QAtomicInteger<int> repeat_skips = 0;
    bool update_timer_heartbeat = false; // update_timer is the slow idle heartbeat
    uint64_t shader_cache_saved_signature = 0; // GUI thread

    QVulkanInstance *qt_vk_inst = {};
    QOpenGLContext *qt_gl_context = {};
    QOffscreenSurface *qt_gl_offscreen_surface = {};
    QQmlEngine *qml_engine = {};
    QQuickWindow *quick_window = {};
    QQuickRenderControl *quick_render = {};
    QQuickItem *quick_item = {};
    pl_tex quick_tex = {};
    QOpenGLFramebufferObject *quick_fbo = {};
    VkSemaphore quick_sem = VK_NULL_HANDLE;
    uint64_t quick_sem_value = 0;
    QTimer *update_timer = {};
    bool quick_frame = false;
    QAtomicInteger<int> quick_need_sync = 0;
    QAtomicInteger<int> quick_need_render = 0;
    QString pending_renderer_fallback_reason;
    pl_options renderparams_opts = {};
    bool renderparams_changed = false;
    const struct pl_hook *fsr_hook = nullptr;
    const struct pl_hook *fsrcnnx_hook_8 = nullptr;
    const struct pl_hook *fsrcnnx_hook_16 = nullptr;

    struct {
        PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
#if defined(Q_OS_LINUX)
        PFN_vkCreateXcbSurfaceKHR vkCreateXcbSurfaceKHR;
        PFN_vkCreateWaylandSurfaceKHR vkCreateWaylandSurfaceKHR;
#elif defined(Q_OS_MACOS)
        PFN_vkCreateMetalSurfaceEXT vkCreateMetalSurfaceEXT;
#elif defined(Q_OS_WIN)
        PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR;
#endif
        PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR;
        PFN_vkWaitSemaphores vkWaitSemaphores;
        PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties;
        PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
        PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR;
    } vk_funcs;

    friend class QmlBackend;
};
