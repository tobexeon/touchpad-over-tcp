#include "MultitouchSupportABI.h"
#include "TouchFrame.h"
#include "gate.h"

#include <CoreFoundation/CoreFoundation.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define FRAMEWORK_PATH "/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport"
#define QUEUE_CAPACITY 256u

typedef struct {
    size_t length;
    uint8_t bytes[TP_MAX_MESSAGE_SIZE];
} WireMessage;

typedef struct {
    WireMessage items[QUEUE_CAPACITY];
    size_t head;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t available;
} MessageQueue;

typedef struct {
    const char *host;
    const char *port;
    MessageQueue queue;
    atomic_bool running;
    atomic_bool connected;
    atomic_bool reconnect_requested;
    atomic_uint_fast32_t sequence;
    atomic_uint_fast64_t captured;
    atomic_uint_fast64_t sent;
    atomic_uint_fast64_t dropped;
    atomic_uint_fast64_t reconnects;
    atomic_bool physical_button_down;
    atomic_uint_fast64_t button_events;
    bool pressure_button_down;
    unsigned pressure_press_frames;
    uint64_t pressure_button_events;
    float pressure_threshold;
    float pressure_release_threshold;
    unsigned pressure_release_frames;
    unsigned pressure_low_frames;
    float last_max_pressure;
    /* Two-finger force press -> HID Button 2 (right click). */
    bool pressure_button2_down;
    unsigned pressure_button2_press_frames;
    unsigned pressure_button2_low_frames;
    uint64_t pressure_button2_events;
    float last_min_pressure;
    float last_avg_pressure;
    /*
     * The two-finger force press is judged on the *mean* tip pressure, not the
     * minimum. Measured on this panel, two fingers of one hand routinely report
     * pressures that differ by 2x (e.g. 96 / 138 at the peak of a deliberate
     * press), so a min-based threshold both delays the latch and loses the press
     * entirely whenever the lighter contact never reaches the threshold. The
     * mean separates cleanly instead: deliberate presses were observed at
     * 92..225 while every plain two-finger scroll stayed at or below 40.
     */
    float pressure2_threshold;
    float pressure2_release_threshold;
    TPFrame latest_frame;
    pthread_mutex_t frame_mutex;
    FILE *button_trace;
    pthread_mutex_t trace_mutex;
    /*
     * Optional foreground gate. Reading the panel directly bypasses every
     * window-focus rule macOS applies to a real trackpad, so without this the
     * pointer keeps controlling the remote host while another application owns
     * the screen. Empty means "always send", which is the previous behaviour.
     */
    const char *gate_app;
    atomic_bool gate_allowed;
    bool gate_blocked;
    /*
     * Stopping the device stream while the gate is shut removes the remaining
     * cost of the feature: without it the framework still wakes this process
     * every 8 ms to deliver contacts that are immediately discarded.
     */
    bool gate_stop_capture;
    bool capture_enabled;
    MTDeviceRef devices[8];
    size_t device_count;
    MTDeviceStartFn start_device;
    MTDeviceStopFn stop_device;
    MTDeviceIsRunningFn is_running_device;
} Agent;

typedef CFArrayRef (*CreateListFn)(void);
typedef void (*RegisterCallbackFn)(MTDeviceRef, MTContactFrameCallback);
typedef void (*RegisterButtonCallbackFn)(MTDeviceRef, MTButtonStateCallback, void *);
typedef void (*StartFn)(MTDeviceRef, int32_t);
typedef bool (*IsBuiltInFn)(MTDeviceRef);

static Agent g_agent;

static void queue_clear(MessageQueue *queue);
static bool queue_push(MessageQueue *queue, const WireMessage *message);

static uint64_t monotonic_microseconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return (uint64_t)value.tv_sec * 1000000ULL + (uint64_t)value.tv_nsec / 1000ULL;
}

static uint8_t flags_for_state(int32_t state) {
    uint8_t flags = TP_CONTACT_CONFIDENCE;
    if (state >= 1 && state <= 6) {
        flags |= TP_CONTACT_IN_RANGE;
    }
    if (state == 3 || state == 4) {
        flags |= TP_CONTACT_TIP;
    }
    return flags;
}

static bool effective_button_down(const Agent *agent) {
    return agent->pressure_button_down ||
           atomic_load_explicit(&agent->physical_button_down, memory_order_acquire);
}

static void trace_callback(Agent *agent, int32_t pressed, int32_t released) {
    if (agent->button_trace == NULL) {
        return;
    }
    pthread_mutex_lock(&agent->trace_mutex);
    fprintf(agent->button_trace,
            "{\"event\":\"button_callback\",\"time_us\":%llu,"
            "\"pressed\":%d,\"released\":%d}\n",
            (unsigned long long)monotonic_microseconds(),
            pressed,
            released);
    fflush(agent->button_trace);
    pthread_mutex_unlock(&agent->trace_mutex);
}

static void trace_frame(Agent *agent, const TPFrame *frame, const char *origin) {
    if (agent->button_trace == NULL) {
        return;
    }
    pthread_mutex_lock(&agent->trace_mutex);
    fprintf(agent->button_trace,
            "{\"event\":\"frame\",\"time_us\":%llu,\"origin\":\"%s\","
            "\"sequence\":%u,\"frame_flags\":%u,\"button\":%s,\"button2\":%s,"
            "\"contact_count\":%u,\"pressure\":%.4g,\"min_pressure\":%.4g,\"avg_pressure\":%.4g,\"latch\":%s,"
            "\"press_frames\":%u,\"low_frames\":%u,"
            "\"latch2\":%s,\"press2_frames\":%u,\"low2_frames\":%u,\"contacts\":[",
            (unsigned long long)frame->capture_time_us,
            origin,
            frame->sequence,
            frame->flags,
            (frame->flags & TP_FRAME_BUTTON) != 0 ? "true" : "false",
            (frame->flags & TP_FRAME_BUTTON2) != 0 ? "true" : "false",
            frame->contact_count,
            (double)agent->last_max_pressure,
            (double)agent->last_min_pressure,
            (double)agent->last_avg_pressure,
            agent->pressure_button_down ? "true" : "false",
            agent->pressure_press_frames,
            agent->pressure_low_frames,
            agent->pressure_button2_down ? "true" : "false",
            agent->pressure_button2_press_frames,
            agent->pressure_button2_low_frames);
    for (unsigned index = 0; index < frame->contact_count; ++index) {
        const TPContact *contact = &frame->contacts[index];
        if (index != 0) {
            fputs(",", agent->button_trace);
        }
        fprintf(agent->button_trace,
                "{\"id\":%u,\"state\":%u,\"flags\":%u,\"x\":%.7g,\"y\":%.7g,"
                "\"size\":%.7g}",
                contact->identifier,
                contact->state,
                contact->flags,
                contact->x,
                contact->y,
                (double)contact->size);
    }
    fputs("]}\n", agent->button_trace);
    fflush(agent->button_trace);
    pthread_mutex_unlock(&agent->trace_mutex);
}

static void trace_connection(Agent *agent) {
    if (agent->button_trace == NULL) {
        return;
    }
    pthread_mutex_lock(&agent->trace_mutex);
    fprintf(agent->button_trace,
            "{\"event\":\"connection\",\"time_us\":%llu,\"connected\":true}\n",
            (unsigned long long)monotonic_microseconds());
    fflush(agent->button_trace);
    pthread_mutex_unlock(&agent->trace_mutex);
}

static void trace_button_capability(Agent *agent, bool available) {
    if (agent->button_trace == NULL) {
        return;
    }
    pthread_mutex_lock(&agent->trace_mutex);
    fprintf(agent->button_trace,
            "{\"event\":\"button_capability\",\"time_us\":%llu,"
            "\"callback_registered\":%s}\n",
            (unsigned long long)monotonic_microseconds(),
            available ? "true" : "false");
    fflush(agent->button_trace);
    pthread_mutex_unlock(&agent->trace_mutex);
}

static bool gate_open(Agent *agent) {
    if (agent->gate_app == NULL || agent->gate_app[0] == '\0') {
        return true;
    }
    return atomic_load_explicit(&agent->gate_allowed, memory_order_relaxed);
}

static void push_frame_locked(Agent *agent, TPFrame *frame, const char *origin) {
    frame->sequence =
        atomic_fetch_add_explicit(&agent->sequence, 1, memory_order_relaxed) + 1;
    frame->capture_time_us = monotonic_microseconds();
    WireMessage message;
    message.length =
        tp_encode_message(TP_MESSAGE_FRAME, frame, message.bytes, sizeof(message.bytes));
    if (message.length != 0 && queue_push(&agent->queue, &message)) {
        trace_frame(agent, frame, origin);
    } else {
        /*
         * Assigned but unsent sequence numbers cannot be skipped inside a
         * strict MTP1 session. Reconnect to establish a fresh HELLO/RESET epoch.
         */
        atomic_store_explicit(&agent->connected, false, memory_order_release);
        atomic_store_explicit(&agent->reconnect_requested, true, memory_order_relaxed);
        queue_clear(&agent->queue);
        atomic_fetch_add_explicit(&agent->dropped, 1, memory_order_relaxed);
    }
}

static void enqueue_frame_locked(Agent *agent, TPFrame *frame, const char *origin) {
    if (!gate_open(agent)) {
        /*
         * Send exactly one empty frame when the gate closes. The receiver keeps
         * the last report it submitted, so a finger or a button that was down at
         * that moment would stay latched until the 1.5 s idle timeout; this lift
         * releases it immediately. Nothing is sent afterwards, which also means
         * no sequence numbers are consumed while the gate is shut.
         */
        if (!agent->gate_blocked) {
            agent->gate_blocked = true;
            TPFrame release = *frame;
            release.contact_count = 0;
            release.flags = 0;
            push_frame_locked(agent, &release, "gate_close");
        }
        return;
    }
    agent->gate_blocked = false;
    push_frame_locked(agent, frame, origin);
}

/*
 * Stopping the device stream is the only way to stop the framework from waking
 * this process every 8 ms to deliver contacts that would be discarded anyway.
 * The callback registration survives MTDeviceStop on the devices seen so far,
 * but a start that does not take effect is retried and reported rather than
 * left silent: losing the panel is far worse than one extra attempt.
 */
static void ApplyCaptureState(Agent *agent, bool enabled) {
    if (!agent->gate_stop_capture || agent->device_count == 0 ||
        agent->start_device == NULL || agent->stop_device == NULL) {
        return;
    }
    if (agent->capture_enabled == enabled) {
        return;
    }
    for (size_t index = 0; index < agent->device_count; ++index) {
        MTDeviceRef device = agent->devices[index];
        if (!enabled) {
            agent->stop_device(device);
            continue;
        }
        agent->start_device(device, 0);
        if (agent->is_running_device != NULL && !agent->is_running_device(device)) {
            agent->start_device(device, 0);
            if (!agent->is_running_device(device)) {
                fprintf(stderr, "warning: device %zu refused to restart\n", index);
            }
        }
    }
    agent->capture_enabled = enabled;
    fprintf(stderr, "capture %s for %zu device(s), captured=%llu so far\n",
            enabled ? "started" : "stopped",
            agent->device_count,
            (unsigned long long)atomic_load_explicit(&agent->captured, memory_order_relaxed));
    fflush(stderr);
}

/*
 * The gate cannot be evaluated from the contact callback alone: that callback
 * only runs while a finger is on the panel, so switching windows with idle
 * hands left the previous decision in place, and the first frame after the
 * switch was already forwarded. A dedicated watcher polls the frontmost
 * application and, the moment the gate closes, pushes one empty frame so the
 * receiver releases whatever was still held down.
 */
static void *gate_watch_main(void *context) {
    Agent *agent = context;
    while (atomic_load_explicit(&agent->running, memory_order_relaxed)) {
        usleep(100000);
        if (agent->gate_app == NULL) {
            continue;
        }
        char foreground[192];
        const bool allowed =
            tp_gate_status(agent->gate_app, foreground, sizeof(foreground)) != 0;
        const bool previous =
            atomic_exchange_explicit(&agent->gate_allowed, allowed, memory_order_relaxed);
        if (allowed == previous) {
            continue;
        }
        fprintf(stderr, "gate %s (frontmost: %s)\n", allowed ? "open" : "closed", foreground);
        fflush(stderr);
        if (allowed) {
            ApplyCaptureState(agent, true);
            continue;
        }
        pthread_mutex_lock(&agent->frame_mutex);
        TPFrame release = agent->latest_frame;
        release.contact_count = 0;
        release.flags = 0;
        enqueue_frame_locked(agent, &release, "gate_close");
        pthread_mutex_unlock(&agent->frame_mutex);
        ApplyCaptureState(agent, false);
    }
    return NULL;
}

static void button_callback(MTDeviceRef device,
                            int32_t pressed,
                            int32_t released,
                            void *context) {
    (void)device;
    Agent *agent = context;
    trace_callback(agent, pressed, released);
    if (pressed != 0) {
        atomic_store_explicit(&agent->physical_button_down, true, memory_order_release);
    }
    if (released != 0) {
        atomic_store_explicit(&agent->physical_button_down, false, memory_order_release);
    }
    atomic_fetch_add_explicit(&agent->button_events, 1, memory_order_relaxed);
    if (!atomic_load_explicit(&agent->connected, memory_order_acquire)) {
        return;
    }

    pthread_mutex_lock(&agent->frame_mutex);
    /*
     * The clickpad switch is the authoritative release. Drop the pressure latch
     * here instead of waiting for finger pressure to stay below the release
     * threshold for N frames: that delay pushed Button 1 back to 0 only tens of
     * milliseconds after the switch opened, so a quick second click landed
     * while the button was still down and Windows saw one long press instead of
     * a double click.
     */
    if (released != 0) {
        agent->pressure_button_down = false;
        agent->pressure_press_frames = 0;
        agent->pressure_low_frames = 0;
    }
    TPFrame frame = agent->latest_frame;
    /* Keep both button bits: rebuilding the flags from only the left button
       dropped a right-click that was already latched. */
    frame.flags = 0;
    if (effective_button_down(agent)) {
        frame.flags |= TP_FRAME_BUTTON;
    }
    if (agent->pressure_button2_down) {
        frame.flags |= TP_FRAME_BUTTON2;
    }
    frame.device_time_us = 0;
    agent->latest_frame = frame;
    enqueue_frame_locked(agent, &frame, "button_callback");
    pthread_mutex_unlock(&agent->frame_mutex);
}

static void queue_clear(MessageQueue *queue) {
    pthread_mutex_lock(&queue->mutex);
    queue->head = 0;
    queue->count = 0;
    pthread_mutex_unlock(&queue->mutex);
}

static bool queue_push(MessageQueue *queue, const WireMessage *message) {
    bool accepted = true;
    pthread_mutex_lock(&queue->mutex);
    if (queue->count == QUEUE_CAPACITY) {
        accepted = false;
    } else {
        const size_t tail = (queue->head + queue->count) % QUEUE_CAPACITY;
        queue->items[tail] = *message;
        ++queue->count;
        pthread_cond_signal(&queue->available);
    }
    pthread_mutex_unlock(&queue->mutex);
    return accepted;
}

static bool queue_pop(MessageQueue *queue, WireMessage *message) {
    pthread_mutex_lock(&queue->mutex);
    while (queue->count == 0 &&
           atomic_load_explicit(&g_agent.running, memory_order_relaxed) &&
           !atomic_load_explicit(&g_agent.reconnect_requested, memory_order_relaxed)) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 100000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000L;
        }
        (void)pthread_cond_timedwait(&queue->available, &queue->mutex, &deadline);
    }
    if (queue->count == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }
    *message = queue->items[queue->head];
    queue->head = (queue->head + 1) % QUEUE_CAPACITY;
    --queue->count;
    pthread_mutex_unlock(&queue->mutex);
    return true;
}

static bool send_all(int socket_fd, const uint8_t *bytes, size_t length) {
    while (length > 0) {
        const ssize_t sent = send(socket_fd, bytes, length, 0);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            return false;
        }
        bytes += (size_t)sent;
        length -= (size_t)sent;
    }
    return true;
}

static int connect_to_receiver(const char *host, const char *port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *addresses = NULL;
    const int lookup = getaddrinfo(host, port, &hints, &addresses);
    if (lookup != 0) {
        fprintf(stderr, "receiver lookup failed: %s\n", gai_strerror(lookup));
        return -1;
    }

    int connected_socket = -1;
    for (struct addrinfo *address = addresses; address != NULL; address = address->ai_next) {
        const int candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate < 0) {
            continue;
        }
        int enabled = 1;
        (void)setsockopt(candidate, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
        (void)setsockopt(candidate, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
        if (connect(candidate, address->ai_addr, address->ai_addrlen) == 0) {
            connected_socket = candidate;
            break;
        }
        close(candidate);
    }
    freeaddrinfo(addresses);
    return connected_socket;
}

static bool send_control(int socket_fd, enum TPMessageType type) {
    TPFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.sequence =
        atomic_fetch_add_explicit(&g_agent.sequence, 1, memory_order_relaxed) + 1;
    frame.capture_time_us = monotonic_microseconds();
    WireMessage message;
    message.length = tp_encode_message(type, &frame, message.bytes, sizeof(message.bytes));
    return message.length > 0 && send_all(socket_fd, message.bytes, message.length);
}

static void *sender_main(void *context) {
    Agent *agent = context;
    while (atomic_load_explicit(&agent->running, memory_order_relaxed)) {
        const int socket_fd = connect_to_receiver(agent->host, agent->port);
        if (socket_fd < 0) {
            sleep(1);
            continue;
        }
        atomic_fetch_add_explicit(&agent->reconnects, 1, memory_order_relaxed);
        queue_clear(&agent->queue);
        if (!send_control(socket_fd, TP_MESSAGE_HELLO) ||
            !send_control(socket_fd, TP_MESSAGE_RESET)) {
            close(socket_fd);
            continue;
        }
        atomic_store_explicit(&agent->reconnect_requested, false, memory_order_relaxed);
        pthread_mutex_lock(&agent->frame_mutex);
        memset(&agent->latest_frame, 0, sizeof(agent->latest_frame));
        pthread_mutex_unlock(&agent->frame_mutex);
        atomic_store_explicit(&agent->connected, true, memory_order_release);
        trace_connection(agent);
        fprintf(stderr, "connected to %s:%s\n", agent->host, agent->port);

        WireMessage message;
        while (atomic_load_explicit(&agent->running, memory_order_relaxed) &&
               !atomic_load_explicit(&agent->reconnect_requested, memory_order_relaxed) &&
               queue_pop(&agent->queue, &message)) {
            if (!send_all(socket_fd, message.bytes, message.length)) {
                int so_error = 0;
                socklen_t error_len = sizeof(so_error);
                (void)getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &so_error, &error_len);
                char peek = 0;
                ssize_t peek_result = recv(socket_fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
                fprintf(stderr,
                        "send failed: errno=%d (%s); so_error=%d (%s); "
                        "peer_recv=%zd peer_errno=%d; seq=%u\n",
                        errno, strerror(errno), so_error, strerror(so_error),
                        peek_result, errno,
                        (unsigned)atomic_load_explicit(&agent->sequence, memory_order_relaxed));
                break;
            }
            atomic_fetch_add_explicit(&agent->sent, 1, memory_order_relaxed);
        }
        atomic_store_explicit(&agent->connected, false, memory_order_release);
        queue_clear(&agent->queue);
        close(socket_fd);
        fprintf(stderr, "receiver disconnected; retrying\n");
    }
    return NULL;
}

static void contact_callback(MTDeviceRef device,
                             MTTouch *touches,
                             size_t touch_count,
                             double timestamp,
                             size_t frame_number) {
    (void)device;
    (void)frame_number;
    atomic_fetch_add_explicit(&g_agent.captured, 1, memory_order_relaxed);
    if (!atomic_load_explicit(&g_agent.connected, memory_order_acquire)) {
        return;
    }
    if (touch_count > TP_MAX_CONTACTS || (touch_count > 0 && touches == NULL)) {
        atomic_fetch_add_explicit(&g_agent.dropped, 1, memory_order_relaxed);
        return;
    }

    pthread_mutex_lock(&g_agent.frame_mutex);
    TPFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.device_time_us = timestamp > 0 ? (uint64_t)(timestamp * 1000000.0) : 0;
    frame.contact_count = (uint16_t)touch_count;
    size_t tip_count = 0;
    float maximum_pressure = 0;
    float minimum_pressure = 0;
    float pressure_sum = 0;
    for (size_t index = 0; index < touch_count; ++index) {
        const MTTouch *source = &touches[index];
        if ((flags_for_state(source->state) & TP_CONTACT_TIP) != 0) {
            if (tip_count == 0 || source->pressure < minimum_pressure) {
                minimum_pressure = source->pressure;
            }
            if (source->pressure > maximum_pressure) {
                maximum_pressure = source->pressure;
            }
            pressure_sum += source->pressure;
            ++tip_count;
        }
    }
    g_agent.last_max_pressure = maximum_pressure;
    g_agent.last_min_pressure = minimum_pressure;
    g_agent.last_avg_pressure = tip_count > 0 ? pressure_sum / (float)tip_count : 0.0f;

    /*
     * Pressure clicking is deliberately single-finger only. Multi-finger
     * contacts belong to the Windows gesture recognizer and must never carry a
     * latched Button 1 merely because one finger crossed the force threshold.
     *
     * Hysteresis: the latch is armed after three single-tip frames at/above
     * pressure_threshold, and released either when the finger lifts / a second
     * finger lands, OR after pressure stays below release_threshold for
     * release_frames consecutive single-tip frames. Without the pressure
     * release path the button stayed latched through an entire ordinary single
     * finger move after one force peak, so Windows interpreted the move as a
     * drag (pointer "jumps", single clicks turning into double clicks, etc.).
     * release_threshold (default 50) is intentionally well below both the
     * press threshold (90) and the measured light-touch maximum (68), so a
     * genuine force drag (median ~189) never releases early.
     */
    if (tip_count != 1) {
        if (g_agent.pressure_button_down) {
            ++g_agent.pressure_button_events;
        }
        g_agent.pressure_button_down = false;
        g_agent.pressure_press_frames = 0;
        g_agent.pressure_low_frames = 0;
    } else if (!g_agent.pressure_button_down && g_agent.pressure_threshold > 0) {
        if (maximum_pressure >= g_agent.pressure_threshold) {
            ++g_agent.pressure_press_frames;
            if (g_agent.pressure_press_frames >= 2) {
                g_agent.pressure_button_down = true;
                ++g_agent.pressure_button_events;
                g_agent.pressure_low_frames = 0;
            }
        } else {
            g_agent.pressure_press_frames = 0;
        }
    } else if (g_agent.pressure_button_down) {
        if (g_agent.pressure_release_threshold > 0 &&
            maximum_pressure < g_agent.pressure_release_threshold) {
            ++g_agent.pressure_low_frames;
            if (g_agent.pressure_low_frames >= g_agent.pressure_release_frames) {
                g_agent.pressure_button_down = false;
                g_agent.pressure_low_frames = 0;
                g_agent.pressure_press_frames = 0;
            }
        } else {
            g_agent.pressure_low_frames = 0;
        }
    }

    /*
     * Two-finger force press -> HID Button 2 (secondary / right click).
     * Judged on the mean tip pressure (see pressure2_threshold) armed after two
     * frames. Released when the finger count leaves two or the mean pressure
     * stays below pressure2_release_threshold for release_frames. This replaces
     * Windows tap-to-right-click, which we disable at the OS level so there is
     * no tap logic at all.
     */
    if (tip_count != 2) {
        if (g_agent.pressure_button2_down) {
            ++g_agent.pressure_button2_events;
        }
        g_agent.pressure_button2_down = false;
        g_agent.pressure_button2_press_frames = 0;
        g_agent.pressure_button2_low_frames = 0;
    } else if (!g_agent.pressure_button2_down && g_agent.pressure2_threshold > 0) {
        if (g_agent.last_avg_pressure >= g_agent.pressure2_threshold) {
            ++g_agent.pressure_button2_press_frames;
            if (g_agent.pressure_button2_press_frames >= 2) {
                g_agent.pressure_button2_down = true;
                ++g_agent.pressure_button2_events;
                g_agent.pressure_button2_low_frames = 0;
            }
        } else if (g_agent.pressure_button2_press_frames > 0) {
            /* Leaky arm: a single noisy frame must not discard an otherwise
               valid press, but the counter still cannot creep up from noise
               because every miss costs one hit. */
            --g_agent.pressure_button2_press_frames;
        }
    } else if (g_agent.pressure_button2_down) {
        /*
         * Two-finger right click keeps a longer hold than Button 1. Button 1 can
         * release in a couple of frames now because the clickpad switch also
         * drives it, but Button 2 has no switch behind it -- releasing on a
         * brief pressure dip would cut the context menu short.
         */
        const unsigned release_frames = g_agent.pressure_release_frames > 8u
                                            ? g_agent.pressure_release_frames
                                            : 8u;
        if (g_agent.pressure2_release_threshold > 0 &&
            g_agent.last_avg_pressure < g_agent.pressure2_release_threshold) {
            ++g_agent.pressure_button2_low_frames;
            if (g_agent.pressure_button2_low_frames >= release_frames) {
                g_agent.pressure_button2_down = false;
                g_agent.pressure_button2_low_frames = 0;
                g_agent.pressure_button2_press_frames = 0;
            }
        } else {
            g_agent.pressure_button2_low_frames = 0;
        }
    }

    frame.flags = 0;
    if (effective_button_down(&g_agent)) {
        frame.flags |= TP_FRAME_BUTTON;
    }
    if (g_agent.pressure_button2_down) {
        frame.flags |= TP_FRAME_BUTTON2;
    }
    for (size_t index = 0; index < touch_count; ++index) {
        const MTTouch *source = &touches[index];
        TPContact *destination = &frame.contacts[index];
        destination->identifier = (uint32_t)source->identifier;
        destination->state = (uint8_t)source->state;
        destination->flags = flags_for_state(source->state);
        destination->x = source->normalized.position.x;
        destination->y = source->normalized.position.y;
        destination->velocity_x = source->normalized.velocity.x;
        destination->velocity_y = source->normalized.velocity.y;
        destination->size = source->size;
        destination->angle = source->angle;
        destination->major_axis = source->major_axis;
        destination->minor_axis = source->minor_axis;
        destination->density = source->density;
    }
    g_agent.latest_frame = frame;
    enqueue_frame_locked(&g_agent, &frame, "contact_callback");
    pthread_mutex_unlock(&g_agent.frame_mutex);
}

static void stop_agent(int signal_number) {
    (void)signal_number;
    atomic_store_explicit(&g_agent.running, false, memory_order_relaxed);
}

static void *load_symbol(void *framework, const char *name, bool required) {
    dlerror();
    void *symbol = dlsym(framework, name);
    const char *error = dlerror();
    if (required && error != NULL) {
        fprintf(stderr, "missing private symbol %s: %s\n", name, error);
        return NULL;
    }
    return symbol;
}

static int usage(const char *program) {
    fprintf(stderr,
            "usage: %s HOST [PORT] [--duration SECONDS] [--button-trace PATH] "
            "[--gate APP] [--gate-keep-capturing] "
            "[--pressure-threshold VALUE] [--pressure-release-threshold VALUE] "
            "[--pressure-release-frames VALUE] [--pressure2-threshold VALUE] "
            "[--pressure2-release-threshold VALUE]\n"
            "  --gate APP  only send while APP is the frontmost application, "
            "matched by bundle identifier or name\n"
            "  --gate-keep-capturing  keep the device stream running while the "
            "gate is shut (default stops it)\n",
            program);
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return usage(argv[0]);
    }
    const char *host = argv[1];
    const char *port = "39871";
    const char *button_trace_path = NULL;
    const char *gate_app = NULL;
    float pressure_threshold = 90.0f;
    float pressure_release_threshold = 50.0f;
    float pressure2_threshold = 70.0f;
    float pressure2_release_threshold = 45.0f;
    int pressure_release_frames = 3;
    int duration = 0;
    bool port_seen = false;
    bool gate_stop_capture = true;
    for (int index = 2; index < argc; ++index) {
        if (strcmp(argv[index], "--duration") == 0) {
            if (++index >= argc) {
                return usage(argv[0]);
            }
            duration = atoi(argv[index]);
            if (duration <= 0 || duration > 86400) {
                return usage(argv[0]);
            }
        } else if (strcmp(argv[index], "--button-trace") == 0) {
            if (++index >= argc || argv[index][0] == '\0') {
                return usage(argv[0]);
            }
            button_trace_path = argv[index];
        } else if (strcmp(argv[index], "--gate") == 0) {
            if (++index >= argc || argv[index][0] == '\0') {
                return usage(argv[0]);
            }
            gate_app = argv[index];
        } else if (strcmp(argv[index], "--gate-keep-capturing") == 0) {
            gate_stop_capture = false;
        } else if (strcmp(argv[index], "--pressure-threshold") == 0) {
            if (++index >= argc) {
                return usage(argv[0]);
            }
            char *end = NULL;
            pressure_threshold = strtof(argv[index], &end);
            if (end == argv[index] || *end != '\0' || pressure_threshold < 0 ||
                pressure_threshold > 1000000) {
                return usage(argv[0]);
            }
        } else if (strcmp(argv[index], "--pressure-release-threshold") == 0) {
            if (++index >= argc) {
                return usage(argv[0]);
            }
            char *end = NULL;
            pressure_release_threshold = strtof(argv[index], &end);
            if (end == argv[index] || *end != '\0' || pressure_release_threshold < 0 ||
                pressure_release_threshold > 1000000) {
                return usage(argv[0]);
            }
        } else if (strcmp(argv[index], "--pressure-release-frames") == 0) {
            if (++index >= argc) {
                return usage(argv[0]);
            }
            char *end = NULL;
            long value = strtol(argv[index], &end, 10);
            if (end == argv[index] || *end != '\0' || value < 1 || value > 1000000) {
                return usage(argv[0]);
            }
            pressure_release_frames = (int)value;
        } else if (strcmp(argv[index], "--pressure2-threshold") == 0) {
            if (++index >= argc) {
                return usage(argv[0]);
            }
            char *end = NULL;
            pressure2_threshold = strtof(argv[index], &end);
            if (end == argv[index] || *end != '\0' || pressure2_threshold < 0 ||
                pressure2_threshold > 1000000) {
                return usage(argv[0]);
            }
        } else if (strcmp(argv[index], "--pressure2-release-threshold") == 0) {
            if (++index >= argc) {
                return usage(argv[0]);
            }
            char *end = NULL;
            pressure2_release_threshold = strtof(argv[index], &end);
            if (end == argv[index] || *end != '\0' || pressure2_release_threshold < 0 ||
                pressure2_release_threshold > 1000000) {
                return usage(argv[0]);
            }
        } else if (!port_seen) {
            port = argv[index];
            port_seen = true;
        } else {
            return usage(argv[0]);
        }
    }

    memset(&g_agent, 0, sizeof(g_agent));
    g_agent.host = host;
    g_agent.port = port;
    g_agent.pressure_threshold = pressure_threshold;
    g_agent.pressure_release_threshold = pressure_release_threshold;
    g_agent.pressure_release_frames = (unsigned)pressure_release_frames;
    g_agent.pressure2_threshold = pressure2_threshold;
    g_agent.pressure2_release_threshold = pressure2_release_threshold;
    g_agent.gate_app = gate_app;
    g_agent.gate_stop_capture = gate_stop_capture;
    g_agent.capture_enabled = true;
    atomic_init(&g_agent.gate_allowed, true);
    if (gate_app != NULL) {
        char foreground[192];
        const bool allowed = tp_gate_status(gate_app, foreground, sizeof(foreground)) != 0;
        atomic_store_explicit(&g_agent.gate_allowed, allowed, memory_order_relaxed);
        fprintf(stderr, "foreground gate: %s (currently %s, frontmost: %s)\n",
                gate_app,
                allowed ? "open" : "closed",
                foreground);
    }
    atomic_init(&g_agent.running, true);
    atomic_init(&g_agent.connected, false);
    atomic_init(&g_agent.reconnect_requested, false);
    pthread_mutex_init(&g_agent.queue.mutex, NULL);
    pthread_cond_init(&g_agent.queue.available, NULL);
    pthread_mutex_init(&g_agent.frame_mutex, NULL);
    pthread_mutex_init(&g_agent.trace_mutex, NULL);
    if (button_trace_path != NULL) {
        g_agent.button_trace = fopen(button_trace_path, "w");
        if (g_agent.button_trace == NULL) {
            fprintf(stderr,
                    "cannot open button trace %s: %s\n",
                    button_trace_path,
                    strerror(errno));
            return 1;
        }
        fprintf(stderr, "button trace: %s\n", button_trace_path);
    }
    signal(SIGINT, stop_agent);
    signal(SIGTERM, stop_agent);
    signal(SIGPIPE, SIG_IGN);

    pthread_t sender;
    if (pthread_create(&sender, NULL, sender_main, &g_agent) != 0) {
        fprintf(stderr, "cannot create sender thread\n");
        return 1;
    }
    pthread_t gate_watcher;
    const bool gate_watcher_running =
        g_agent.gate_app != NULL &&
        pthread_create(&gate_watcher, NULL, gate_watch_main, &g_agent) == 0;
    if (g_agent.gate_app != NULL && !gate_watcher_running) {
        fprintf(stderr, "cannot create gate watcher thread\n");
    }
    void *framework = dlopen(FRAMEWORK_PATH, RTLD_NOW | RTLD_LOCAL);
    if (framework == NULL) {
        fprintf(stderr, "cannot load MultitouchSupport: %s\n", dlerror());
        stop_agent(0);
        pthread_join(sender, NULL);
        return 1;
    }
    CreateListFn create_list = NULL;
    RegisterCallbackFn register_callback = NULL;
    RegisterButtonCallbackFn register_button_callback = NULL;
    StartFn start = NULL;
    IsBuiltInFn is_built_in = NULL;
    MTDeviceStopFn stop = NULL;
    MTDeviceIsRunningFn is_running = NULL;
    *(void **)(&create_list) = load_symbol(framework, "MTDeviceCreateList", true);
    *(void **)(&register_callback) = load_symbol(framework, "MTRegisterContactFrameCallback", true);
    *(void **)(&register_button_callback) =
        load_symbol(framework, "MTRegisterButtonStateCallback", false);
    *(void **)(&start) = load_symbol(framework, "MTDeviceStart", true);
    *(void **)(&is_built_in) = load_symbol(framework, "MTDeviceIsBuiltIn", false);
    *(void **)(&stop) = load_symbol(framework, "MTDeviceStop", false);
    *(void **)(&is_running) = load_symbol(framework, "MTDeviceIsRunning", false);
    if (create_list == NULL || register_callback == NULL || start == NULL) {
        stop_agent(0);
        pthread_join(sender, NULL);
        return 1;
    }
    trace_button_capability(&g_agent, register_button_callback != NULL);

    fflush(stdout);
    const int saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout >= 0) {
        (void)dup2(STDERR_FILENO, STDOUT_FILENO);
    }
    CFArrayRef devices = create_list();
    if (devices != NULL) {
        CFRetain(devices);
    }
    int started = 0;
    g_agent.start_device = start;
    g_agent.stop_device = stop;
    g_agent.is_running_device = is_running;
    if (devices != NULL) {
        for (CFIndex device_index = 0; device_index < CFArrayGetCount(devices); ++device_index) {
            MTDeviceRef device = (MTDeviceRef)CFArrayGetValueAtIndex(devices, device_index);
            if (is_built_in != NULL && !is_built_in(device)) {
                continue;
            }
            register_callback(device, contact_callback);
            if (register_button_callback != NULL) {
                register_button_callback(device, button_callback, &g_agent);
            }
            start(device, 0);
            if (g_agent.device_count < sizeof(g_agent.devices) / sizeof(g_agent.devices[0])) {
                g_agent.devices[g_agent.device_count++] = device;
            }
            ++started;
        }
    }
    fflush(stdout);
    if (saved_stdout >= 0) {
        (void)dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdout);
    }
    if (started == 0) {
        fprintf(stderr, "no built-in multitouch device found\n");
        stop_agent(0);
        pthread_join(sender, NULL);
        return 1;
    }
    /* Devices start enabled; if the gate is already shut, shut them again. */
    if (!gate_open(&g_agent)) {
        ApplyCaptureState(&g_agent, false);
    }

    fprintf(stderr,
            "streaming %d built-in trackpad(s) to %s:%s; physical button=%s; "
            "pressure click threshold=%.1f; release below %.1f for %u frames\n",
            started,
            host,
            port,
            register_button_callback != NULL ? "enabled" : "unavailable",
            pressure_threshold,
            pressure_release_threshold,
            (unsigned)pressure_release_frames);
    const uint64_t started_at = monotonic_microseconds();
    while (atomic_load_explicit(&g_agent.running, memory_order_relaxed)) {
        usleep(100000);
        if (duration > 0 && monotonic_microseconds() - started_at >= (uint64_t)duration * 1000000ULL) {
            stop_agent(0);
        }
    }
    pthread_join(sender, NULL);
    if (gate_watcher_running) {
        pthread_join(gate_watcher, NULL);
    }
    fprintf(stderr,
            "summary: captured=%llu sent=%llu dropped=%llu connections=%llu "
            "button_events=%llu pressure_button_events=%llu pressure_button2_events=%llu\n",
            (unsigned long long)atomic_load_explicit(&g_agent.captured, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_agent.sent, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_agent.dropped, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_agent.reconnects, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_agent.button_events, memory_order_relaxed),
            (unsigned long long)g_agent.pressure_button_events,
            (unsigned long long)g_agent.pressure_button2_events);
    return 0;
}
