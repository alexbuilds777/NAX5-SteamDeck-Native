// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_CONTROLLERMANAGER_H
#define CHIAKI_CONTROLLERMANAGER_H

#include <chiaki/controller.h>

#include <QObject>
#include <QSet>
#include <QMap>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>
#include <QAtomicInteger>

#ifdef CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER
#include <SDL.h>
#include <chiaki/orientation.h>
#endif

#define PS_TOUCHPAD_MAXX 1920
#define PS_TOUCHPAD_MAXY 1079

class Controller;

class ControllerManager : public QObject
{
	Q_OBJECT

	friend class Controller;

	private:
#ifdef CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER
		QSet<SDL_JoystickID> available_controllers;
#endif
		QMap<int, Controller *> open_controllers;
		bool creating_controller_mapping;
		bool joystick_allow_background_events;
		bool is_app_active;
		bool moved;
		uint8_t dualsense_intensity;
		// NAX5: GUI stall detection, gap between HandleEvents ticks minus the interval
		QElapsedTimer handle_events_timer;
		QAtomicInteger<quint32> gui_stall_max_ms{0};
		// NAX5 input latency bound: monotonic µs (chiaki_time_now_monotonic_us) at the start
		// of the previous / current HandleEvents tick. An event delivered by the current
		// tick's SDL_PollEvent pass happened after the previous pass drained the queue, i.e.
		// no earlier than prev_tick_start_us. 0 = no previous tick yet.
		quint64 prev_tick_start_us = 0;
		quint64 cur_tick_start_us = 0;

		void ControllerClosed(Controller *controller);
		void CheckMoved();

	private slots:
		void UpdateAvailableControllers();
		void HandleEvents();
#ifdef CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER
		void ControllerEvent(SDL_Event evt);
#endif

	public:
		static ControllerManager *GetInstance();

		ControllerManager(QObject *parent = nullptr);
		~ControllerManager();
		void SetButtonsByPos();
		void SetAllowJoystickBackgroundEvents(bool enabled);
		void SetIsAppActive(bool active);
		void SetDualSenseIntensity(uint8_t intensity) { dualsense_intensity = intensity; };
		uint8_t GetDualSenseIntensity() { return dualsense_intensity; };
		void creatingControllerMapping(bool creating_controller_mapping);
		QSet<int> GetAvailableControllers();
		Controller *OpenController(int device_id);
		// NAX5: max (gap between HandleEvents ticks - UPDATE_INTERVAL_MS) since the last
		// call, in ms; then reset. 0 when the event loop kept up.
		quint32 TakeGuiStallMaxMs() { return gui_stall_max_ms.fetchAndStoreRelaxed(0); }

	signals:
		void AvailableControllersUpdated();
		void ControllerMoved();
};

class Controller : public QObject
{
	Q_OBJECT

	friend class ControllerManager;

	private:
		Controller(int device_id, ControllerManager *manager);

#ifdef CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER
		void UpdateState(SDL_Event event);
		void SetDualSenseRumble(uint8_t left, uint8_t right);
		bool HandleButtonEvent(SDL_ControllerButtonEvent event);
		bool HandleAxisEvent(SDL_ControllerAxisEvent event);
#if SDL_VERSION_ATLEAST(2, 0, 14)
		bool HandleSensorEvent(SDL_ControllerSensorEvent event);
		bool HandleTouchpadEvent(SDL_ControllerTouchpadEvent event);
#endif
		bool WouldUndoPendingChange(const SDL_Event &event);
#endif
		// NAX5: emits StateChanged if events were applied since the last emission
		// (coalesced input, see NAX5_COALESCE_INPUT in controllermanager.cpp)
		void FlushState();

		int ref;
		ControllerManager *manager;
		int id;
		ChiakiOrientationTracker orientation_tracker;
		ChiakiControllerState state;
		bool updating_mapping_button;
		bool enable_analog_stick_mapping;
		bool is_dualsense;
		bool is_handheld;
		bool is_steam_virtual;
		bool is_steam_virtual_unmasked;
		bool is_dualsense_edge;
		bool has_led;
		bool micbutton_push;
		uint16_t firmware_version;
		bool state_dirty;
		ChiakiControllerState emitted_state;
		// NAX5 input latency: earliest possible occurrence time (monotonic µs = start of the
		// HandleEvents tick before the one that polled the event) of the oldest event folded
		// into the pending state, moved to the emitted slot when StateChanged fires.
		bool has_pending_event_poll;
		quint64 pending_event_poll_us;
		bool has_emitted_event_poll;
		quint64 emitted_event_poll_us;

#ifdef CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER
		QMap<QPair<Sint64, Sint64>, uint8_t> touch_ids;
		SDL_GameController *controller;
		ChiakiAccelNewZero accel_zero;
		ChiakiAccelNewZero real_accel;
		uint32_t last_motion_timestamp;
#endif

	public:
		~Controller();

		void Ref();
		void Unref();

		bool IsConnected();
		int GetDeviceID();
#ifdef CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER
		SDL_GameController *GetController() { return controller; };
#endif
		QString GetName();
		QString GetVIDPIDString();
		QString GetType();
		bool IsPS();
		QString GetGUIDString();
		ChiakiControllerState GetState();
		// NAX5: earliest possible occurrence time (monotonic µs, previous poll tick start) of
		// the oldest event behind the last StateChanged; true once per emission, then cleared.
		// Not available (false) for events of the very first tick.
		bool TakeLastEventPollUs(quint64 *since_us);
		void SetRumble(uint8_t left, uint8_t right);
		void SetTriggerEffects(uint8_t type_left, const uint8_t *data_left, uint8_t type_right, const uint8_t *data_right);
		void SetDualsenseMic(bool on);
		void SetHapticRumble(uint16_t left, uint16_t right);
		void StartUpdatingMapping();
		void IsUpdatingMappingButton(bool is_updating_mapping_button);
		void EnableAnalogStickMapping(bool enabled);
		void ChangeLEDColor(const uint8_t *led_color);
		void ChangePlayerIndex(const uint8_t player_index);
		bool IsDualSense();
		bool IsHandheld();
		bool IsSteamVirtual();
		bool IsSteamVirtualUnmasked();
		bool IsDualSenseEdge();
		void resetMotionControls();

	signals:
		void StateChanged();
		void MicButtonPush();
		void NewButtonMapping(QString button);
		void UpdatingControllerMapping(Controller* controller);
};
#endif // CHIAKI_CONTROLLERMANAGER_H
