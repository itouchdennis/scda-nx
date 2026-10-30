// Gyro aiming: the controller's gyro turns the view like a mouse, beside the right stick.
// SDL's thread reads every reading as it arrives; the game takes the turn gathered since its last look update. On the
// Switch, whose SDL2 has no sensors, libnx's six-axis readings since the last look update are taken with it instead.
#include "core/gyro.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <mutex>

#ifdef SCDA_SWITCH
#include <switch.h>
#else
#include <SDL3/SDL.h>
#endif

#include "core/log.h"

namespace gyro {

namespace {

constexpr float kDegreesPerRadian = 57.2957795f;
constexpr float kSecondsPerNs = 1e-9f;
constexpr float kMaxSampleGap = 0.05f;  // seconds; a longer gap between readings is a stall, not motion
constexpr uint64_t kStaleNs = 100'000'000;  // turn gathered while the look update did not run is dropped
constexpr uint64_t kScanNs = 1'000'000'000;  // controllers plugged in since get their sensors on
constexpr float kGravitySeconds = 0.25f;  // the accelerometer's smoothing into the controller's up
// a controller at rest stays within these through a whole window; the gyro's mean there is its drift.
// A slow steady turn passes the spread tests too, so only a mean this small is taken for drift.
constexpr float kRestSeconds = 2.0f;
constexpr float kRestGyroSpread = 1.0f;  // degrees a second
constexpr float kRestAccelSpread = 0.1f;  // metres a second squared
constexpr float kMaxDrift = 1.5f;  // degrees a second
constexpr uint64_t kCalibrateNs = 10'000'000'000;  // a calibration the controller did not lie still through gives up
// turns slower than this are smoothed over kSmoothSeconds, twice as fast pass as they are
constexpr float kSmoothSpeed = 5.0f;  // degrees a second
constexpr float kSmoothSeconds = 0.1f;

#ifndef SCDA_SWITCH
// kButtonNames in order
constexpr SDL_GamepadButton kButtons[] = {
    SDL_GAMEPAD_BUTTON_INVALID,       SDL_GAMEPAD_BUTTON_TOUCHPAD,      SDL_GAMEPAD_BUTTON_MISC1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,  SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE2};
static_assert(std::size(kButtons) == std::size(kButtonNames));
static_assert(SDL_GAMEPAD_BUTTON_COUNT <= 32);
#endif

// SDL's axes: x right, y up out of the face, z toward the player
struct Vec3 {
    float x = 0;
    float y = 0;
    float z = 0;
};

Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
float Largest(const Vec3& a) { return std::max({std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)}); }

// the lowest and highest reading on each axis
struct Spread {
    Vec3 low;
    Vec3 high;
    bool empty = true;

    void Add(const Vec3& v) {
        if (empty) {
            low = v;
            high = v;
            empty = false;
            return;
        }
        low = {std::min(low.x, v.x), std::min(low.y, v.y), std::min(low.z, v.z)};
        high = {std::max(high.x, v.x), std::max(high.y, v.y), std::max(high.z, v.z)};
    }
    float Widest() const { return empty ? 0.0f : Largest(Sub(high, low)); }
};

// the gyro's mean over a window
struct Average {
    Vec3 sum;
    int count = 0;
    float seconds = 0;

    void Push(const Vec3& v, float dt) {
        sum = Add(sum, v);
        count++;
        seconds += dt;
    }
    Vec3 Mean() const { return count ? Scale(sum, 1.0f / float(count)) : Vec3{}; }
};

// the window the controller is tested for rest through
struct RestWindow {
    Spread gyro;
    Spread accel;
    Average mean;
};

// the controller aiming: the first whose gyro reports, until it is unplugged
struct Motion {
    uint32_t pad = 0;  // SDL's joystick id; on the Switch 1 once a reading came
    uint64_t gyro_ns = 0;  // sensor clock of the last readings
    uint64_t accel_ns = 0;
    Vec3 gravity;  // smoothed accelerometer, pointing up
    Vec3 drift;  // degrees a second the gyro reads at rest
    Vec3 smoothed;  // the slow share of the turn, degrees a second
    RestWindow rest;
    bool calibrating = false;  // the next still window sets the drift, however large
    uint64_t calibrate_ns = 0;  // NowNs() it started
    bool calibrated = false;  // by hand: rest windows stop, a slow steady turn would pass for drift
    // degrees turned since the last take: about the controller's up, its front, the world's up; pitch down positive
    float yaw = 0;
    float roll = 0;
    float world_yaw = 0;
    float pitch = 0;
    uint32_t held = 0;  // bits by SDL button
    uint32_t pressed = 0;  // went down since the last take
};

std::mutex g_lock;
Motion g_motion;
bool g_toggled_off = false;  // the gyro button in kToggle mode

// seconds since the last reading, 0 for the first and for a stall
float Gap(uint64_t& last, uint64_t now) {
    int64_t ns = last ? int64_t(now - last) : 0;
    last = now;
    float seconds = float(ns) * kSecondsPerNs;
    return seconds > 0 && seconds <= kMaxSampleGap ? seconds : 0.0f;
}

Vec3 Up(const Motion& m) {
    float length = Length(m.gravity);
    if (length < 1.0f) return {0, 1, 0};  // no accelerometer yet
    return Scale(m.gravity, 1.0f / length);
}

void ReadAccel(Motion& m, const Vec3& accel, uint64_t ns) {
    bool first = !m.accel_ns;
    float dt = Gap(m.accel_ns, ns);
    float k = first ? 1.0f : dt / (kGravitySeconds + dt);
    m.gravity = Add(m.gravity, Scale(Sub(accel, m.gravity), k));
    m.rest.accel.Add(accel);
}

// a window the controller lay still through sets the drift to the gyro's mean over it
void UpdateDrift(Motion& m, const Vec3& gyro, float dt) {
    RestWindow& w = m.rest;
    w.gyro.Add(gyro);
    w.mean.Push(gyro, dt);
    if (w.mean.seconds < kRestSeconds) return;
    Vec3 mean = w.mean.Mean();
    bool still = w.gyro.Widest() < kRestGyroSpread && w.accel.Widest() < kRestAccelSpread;
    if (still && m.calibrating) {
        m.drift = mean;
        m.calibrating = false;
        m.calibrated = true;
        LOGR("gyro: drift %.3f %.3f %.3f", m.drift.x, m.drift.y, m.drift.z);
    } else if (still && !m.calibrated && Largest(mean) < kMaxDrift) {
        m.drift = mean;
    }
    w = {};
}

// the slow share of a turn is smoothed, the fast share passes as it is: sensor noise goes, quick aim keeps no lag
Vec3 ApplySmoothing(Motion& m, const Vec3& turn, float dt) {
    float fast = std::clamp(Length(turn) / kSmoothSpeed - 1.0f, 0.0f, 1.0f);
    float k = dt / (kSmoothSeconds + dt);
    m.smoothed = Add(m.smoothed, Scale(Sub(Scale(turn, 1.0f - fast), m.smoothed), k));
    return Add(Scale(turn, fast), m.smoothed);
}

void ReadGyro(Motion& m, const Vec3& radians, uint64_t ns) {
    float dt = Gap(m.gyro_ns, ns);
    Vec3 gyro = Scale(radians, kDegreesPerRadian);
    UpdateDrift(m, gyro, dt);
    if (m.calibrating) return;
    Vec3 turn = ApplySmoothing(m, Sub(gyro, m.drift), dt);
    m.yaw += turn.y * dt;
    m.roll += turn.z * dt;  // the left side down turns left
    m.world_yaw += Dot(turn, Up(m)) * dt;
    m.pitch -= turn.x * dt;  // turning about x raises the controller's front
}

uint64_t NowNs() {
#ifdef SCDA_SWITCH
    return armTicksToNs(armGetSystemTick());
#else
    return SDL_GetTicksNS();
#endif
}

#ifdef SCDA_SWITCH
// Player 1's controller: the console's Joy-Con in handheld, a Pro Controller, or the right Joy-Con of a pair
constexpr float kTwoPi = 6.2831853f;
constexpr float kGravity = 9.80665f;  // metres a second squared

struct Sensors {
    HidSixAxisSensorHandle handheld{}, full{}, dual[2]{};
    bool started = false;
    uint32_t current = 0;  // the handle read last (type_value): another controller starts the clock afresh
    uint64_t sampling = 0;  // the last reading taken
    uint64_t clock = 0;  // the readings' own, their delta times summed
};
Sensors g_sensors;

void StartSensors() {
    Sensors& s = g_sensors;
    if (s.started) return;
    hidGetSixAxisSensorHandles(&s.handheld, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    hidGetSixAxisSensorHandles(&s.full, 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
    hidGetSixAxisSensorHandles(s.dual, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
    for (HidSixAxisSensorHandle h : {s.handheld, s.full, s.dual[0], s.dual[1]}) hidStartSixAxisSensor(h);
    s.started = true;
    LOGR("gyro: six-axis sensors on");
}

// Nintendo's axes are x right, y forward, z up out of the face; SDL's, which all of the above uses, x right, y up out
// of the face, z toward the player. A rotation, no mirror, so readings and turns map alike
Vec3 FromNintendo(const HidVector& v, float scale) { return {v.x * scale, v.z * scale, -v.y * scale}; }

// every reading since the last call; angular velocity in turns a second, acceleration in g
void Poll(Motion& m) {
    StartSensors();
    Sensors& s = g_sensors;
    HidSixAxisSensorHandle h{};
    uint32_t style = hidGetNpadStyleSet(HidNpadIdType_No1);
    if (hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld) h = s.handheld;
    else if (style & HidNpadStyleTag_NpadFullKey) h = s.full;
    else if (style & HidNpadStyleTag_NpadJoyDual) h = s.dual[1];
    else return;
    if (h.type_value != s.current) {
        s.current = h.type_value;
        s.sampling = 0;
        m.gyro_ns = m.accel_ns = 0;
    }
    HidSixAxisSensorState states[32];
    size_t n = hidGetSixAxisSensorStates(h, states, std::size(states));
    std::sort(states, states + n, [](const HidSixAxisSensorState& a, const HidSixAxisSensorState& b) {
        return a.sampling_number < b.sampling_number;
    });
    for (size_t i = 0; i < n; i++) {
        const HidSixAxisSensorState& r = states[i];
        if (!(r.attributes & HidSixAxisSensorAttribute_IsConnected) || r.sampling_number <= s.sampling) continue;
        s.sampling = r.sampling_number;
        s.clock += r.delta_time;
        m.pad = 1;
        ReadAccel(m, FromNintendo(r.acceleration, kGravity), s.clock);
        ReadGyro(m, FromNintendo(r.angular_velocity, kTwoPi), s.clock);
    }
}

// the gyro button rows name buttons the Switch's controllers have not: none, the gyro always on
uint32_t ButtonBit(uint32_t) { return 0; }
#else
void ReadSensor(Motion& m, const SDL_GamepadSensorEvent& s) {
    if (!m.pad && s.sensor == SDL_SENSOR_GYRO) m.pad = s.which;
    if (s.which != m.pad) return;
    Vec3 data{s.data[0], s.data[1], s.data[2]};
    uint64_t ns = s.sensor_timestamp ? s.sensor_timestamp : s.timestamp;
    if (s.sensor == SDL_SENSOR_GYRO) ReadGyro(m, data, ns);
    if (s.sensor == SDL_SENSOR_ACCEL) ReadAccel(m, data, ns);
}

void ReadButton(Motion& m, const SDL_GamepadButtonEvent& b) {
    if (b.which != m.pad || b.button >= 32) return;
    uint32_t bit = 1u << b.button;
    if (b.down && !(m.held & bit)) m.pressed |= bit;
    m.held = b.down ? m.held | bit : m.held & ~bit;
}

// SDL's thread that reads the controllers, every event as it arrives
bool SDLCALL OnEvent(void*, SDL_Event* e) {
    std::lock_guard lock(g_lock);
    switch (e->type) {
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (e->gdevice.which == g_motion.pad) g_motion = {};
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            ReadButton(g_motion, e->gbutton);
            break;
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
            ReadSensor(g_motion, e->gsensor);
            break;
    }
    return true;
}

void EnableSensors() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids) return;
    for (int i = 0; i < count; i++) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (!pad || !SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) continue;
        if (SDL_GamepadSensorEnabled(pad, SDL_SENSOR_GYRO)) continue;
        SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
        SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
        LOGR("gyro: %s", SDL_GetGamepadName(pad));
    }
    SDL_free(ids);
}

// once the controllers are up: the event watch, then the sensors of new controllers each scan
void EnsureSensors() {
    static bool watching = false;
    static uint64_t scanned = 0;
    if (!SDL_WasInit(SDL_INIT_GAMEPAD)) return;
    if (!watching) watching = SDL_AddEventWatch(OnEvent, nullptr);
    uint64_t now = SDL_GetTicksNS();
    if (scanned && now - scanned < kScanNs) return;
    scanned = now;
    EnableSensors();
}

uint32_t ButtonBit(uint32_t index) {
    if (index == 0 || index >= std::size(kButtons)) return 0;
    return 1u << kButtons[index];
}
#endif

// a calibration past kCalibrateNs stops, so a controller never laid down does not leave the gyro turning nothing
void EndStaleCalibration(Motion& m, uint64_t now) {
    if (!m.calibrating || now - m.calibrate_ns < kCalibrateNs) return;
    m.calibrating = false;
    m.rest = {};
    LOGR("gyro: calibration gave up, the controller did not lie still");
}

// the gyro button: off or on while held, or a press flips it; no button keeps the gyro on
bool UpdateSwitch(const Settings& s, const Motion& m) {
    uint32_t bit = ButtonBit(s.button);
    if (!bit) return true;
    bool held = m.held & bit;
    if (s.button_mode == kOnWhileHeld) return held;
    if (s.button_mode != kToggle) return !held;
    if (m.pressed & bit) g_toggled_off = !g_toggled_off;
    return !g_toggled_off;
}

float AxisTurn(const Motion& m, uint32_t axis) {
    if (axis == kRoll) return m.roll;
    if (axis == kWorld) return m.world_yaw;
    return m.yaw;
}

}  // namespace

Turn Take(const Settings& s) {
    static uint64_t last_take = 0;
    if (!s.enabled) return {};
#ifndef SCDA_SWITCH
    EnsureSensors();
#endif
    uint64_t now = NowNs();
    bool stale = now - last_take > kStaleNs;
    last_take = now;
    std::lock_guard lock(g_lock);
    Motion& m = g_motion;
#ifdef SCDA_SWITCH
    Poll(m);
#endif
    EndStaleCalibration(m, now);
    Turn turn;
    if (stale) m.pressed = 0;  // presses while the look did not run
    bool on = UpdateSwitch(s, m);
    if (on && !stale) {
        turn.yaw = AxisTurn(m, s.axis) * s.sensitivity;
        turn.pitch = m.pitch * s.sensitivity;
    }
    turn.recenter = !stale && (m.pressed & ButtonBit(s.recenter_button));
    m.yaw = 0;
    m.roll = 0;
    m.world_yaw = 0;
    m.pitch = 0;
    m.pressed = 0;
    return turn;
}

void StartCalibration() {
    std::lock_guard lock(g_lock);
    if (!g_motion.pad) return;  // no gyro read yet
    g_motion.calibrating = true;
    g_motion.calibrate_ns = NowNs();
    g_motion.rest = {};
}

// the menu asks each frame; on the Switch the sensors are read here too, as the look update does not run in the menu
bool Calibrating() {
    std::lock_guard lock(g_lock);
#ifdef SCDA_SWITCH
    if (g_motion.calibrating) Poll(g_motion);
#endif
    EndStaleCalibration(g_motion, NowNs());
    return g_motion.calibrating;
}

}  // namespace gyro
