#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "btstack.h"
#include "btstack_control_iphone.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "btstack_run_loop_corefoundation.h"
#include "btstack_tlv_posix.h"
#include "bridge_ipc.h"
#include "media_info.h"

enum { PAIRING_MS = 120000, STARTUP_MS = 18000, POLL_MS = 200, STOP_MS = 3500 };
static const hci_transport_config_uart_t transport_config = {
    .type = HCI_TRANSPORT_CONFIG_UART, .device_name = "/dev/tty.bluetooth",
    .baudrate_init = 0, .baudrate_main = 0, .flowcontrol = 1
};
static btstack_tlv_posix_t keys;
static btstack_control_t control;
static btstack_packet_callback_registration_t events;
static btstack_timer_source_t timer;
static volatile sig_atomic_t stop_requested, pair_requested;
static uint32_t deadline, pair_deadline;
static int ready, stopping, pairing, took_radio, failed;
static int report_fd = -1;
static pid_t supervisor;
static int pair_on_start = 1;

int dockbridge_trace_hci_init(void) {
    return !ready && !stopping;
}

static void report_worker_event(const db_worker_event *event) {
    if (report_fd >= 0) {
        // Keep each record within POSIX's minimum atomic pipe write size.
        typedef char event_must_fit_pipe_buf[sizeof(*event) <= 512 ? 1 : -1];
        ssize_t result;
        do { result = write(report_fd, event, sizeof(*event)); } while (result < 0 && errno == EINTR);
    }
}

void dockbridge_report(uint8_t kind) {
    db_worker_event event = { .kind = kind };
    report_worker_event(&event);
}

void dockbridge_report_media(const db_media_snapshot *media) {
    db_worker_event event = { .kind = DB_EVENT_MEDIA, .media = *media };
    report_worker_event(&event);
}

extern const btstack_audio_sink_t dockbridge_audio_sink;
extern int dockbridge_start_a2dp(void);
extern void hci_transport_h4_iphone_set_enforce_wake_device(char *path);

static void on_signal(int number) {
    if (number == SIGUSR1) pair_requested = 1;
    else stop_requested = 1;
}

// The iOS Bluetooth switch must be turned off manually before invoking --start.
// The optional upstream SpringBoard extension is not installed by this package.
int SBA_getBluetoothEnabled(void) { return 0; }
int SBA_setBluetoothEnabled(int enabled) { (void)enabled; return 0; }

static int release_radio(void) {
    if (!took_radio) return 0;
    took_radio = 0;
    int result = btstack_control_iphone.off();
    if (!result) dockbridge_report(DB_EVENT_RADIO_RELEASE);
    return result;
}

static int acquire_radio(void) {
    took_radio = 1;
    int result = btstack_control_iphone.on();
    if (result) release_radio();
    return result;
}

static void set_pairing(int enabled) {
    pairing = enabled;
    gap_set_bondable_mode(enabled);
    gap_discoverable_control(enabled);
    if (enabled) {
        dockbridge_report(DB_EVENT_PAIR_OPEN);
        pair_deadline = btstack_run_loop_get_time_ms() + PAIRING_MS;
        fprintf(stderr, "Pairing open for 120 seconds. Select DockBridge 4S in Android Bluetooth.\n");
    } else {
        dockbridge_report(DB_EVENT_PAIR_CLOSED);
        fprintf(stderr, "Pairing closed. Previously paired devices can reconnect.\n");
    }
}

static void event_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (type != HCI_EVENT_PACKET || size < 2) return;
    switch (packet[0]) {
        case BTSTACK_EVENT_STATE:
            if (size < 3) return;
            if (packet[2] == HCI_STATE_WORKING && !stopping) {
                ready = 1;
                dockbridge_report(DB_EVENT_READY);
                fprintf(stderr, "Bluetooth controller ready.\n");
                if (pair_on_start) set_pairing(1);
                pair_on_start = 0;
            } else if (packet[2] == HCI_STATE_OFF && stopping) {
                CFRunLoopStop(CFRunLoopGetCurrent());
            }
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            if (size < 12) return;
            bd_addr_t address;
            hci_event_user_confirmation_request_get_bd_addr(packet, address);
            if (pairing) gap_ssp_confirmation_response(address);
            else gap_ssp_confirmation_negative(address);
            break;
        }
        case HCI_EVENT_PIN_CODE_REQUEST: {
            if (size < 8) return;
            bd_addr_t address;
            hci_event_pin_code_request_get_bd_addr(packet, address);
            gap_pin_code_negative(address);
            break;
        }
        case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
            if (size >= 9 && !hci_event_simple_pairing_complete_get_status(packet) && pairing) {
                set_pairing(0);
            }
            break;
    }
}

static int expired(uint32_t now, uint32_t until) {
    return (int32_t)(now - until) >= 0;
}

static void poll_state(btstack_timer_source_t *source) {
    uint32_t now = btstack_run_loop_get_time_ms();
    if (supervisor && getppid() != supervisor) stop_requested = 1;
    if (stopping && expired(now, deadline)) {
        CFRunLoopStop(CFRunLoopGetCurrent());
        return;
    }
    if (!ready && !stopping && expired(now, deadline)) {
        fprintf(stderr, "Bluetooth controller did not answer HCI initialization.\n");
        dockbridge_report(DB_EVENT_INIT_TIMEOUT);
        failed = 1;
        stop_requested = 1;
    }
    if (stop_requested && !stopping) {
        stopping = 1;
        deadline = now + STOP_MS;
        dockbridge_audio_sink.close();
        hci_power_control(HCI_POWER_OFF);
    } else if (ready && !stopping) {
        if (pair_requested) {
            pair_requested = 0;
            set_pairing(1);
        } else if (pairing && expired(now, pair_deadline)) {
            set_pairing(0);
        }
        dockbridge_media_tick(now);
    }
    btstack_run_loop_set_timer(source, POLL_MS);
    btstack_run_loop_add_timer(source);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--service")) return dockbridge_service();
    if (argc == 2 && (!strcmp(argv[1], "--worker") || !strcmp(argv[1], "--worker-automatic"))) {
        pair_on_start = !strcmp(argv[1], "--worker");
        report_fd = DB_WORKER_FD;
        supervisor = getppid();
        fcntl(report_fd, F_SETFD, FD_CLOEXEC);
    } else if (argc != 2 || strcmp(argv[1], "--start") != 0) {
        fprintf(stderr, "Turn off Bluetooth in iOS Settings, then run as root: %s --start\n", argv[0]);
        return 2;
    }
    struct utsname device;
    if (getuid() != 0 || uname(&device) || strcmp(device.machine, "iPhone4,1") ||
        access(transport_config.device_name, F_OK)) {
        fprintf(stderr, "Root and an iPhone 4S with /dev/tty.bluetooth are required.\n");
        return 2;
    }
    if (access("/usr/sbin/BlueTool", X_OK)) {
        perror("BlueTool");
        return 2;
    }
    setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
    umask(077);
    int lock = open(DB_STATE_DIR "/receiver.lock", O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
        perror("DockBridge lock");
        return 2;
    }
    fcntl(lock, F_SETFD, FD_CLOEXEC);
    setvbuf(stdout, NULL, _IOLBF, 0);
    fprintf(stderr, "DockBridge PID %d. Stop with Ctrl+C or SIGTERM, reopen pairing with SIGUSR1.\n", getpid());
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGUSR1, on_signal);

    OSStatus status = AudioSessionInitialize(CFRunLoopGetCurrent(), kCFRunLoopDefaultMode, NULL, NULL);
    UInt32 category = kAudioSessionCategory_MediaPlayback;
    if (!status) status = AudioSessionSetProperty(kAudioSessionProperty_AudioCategory, sizeof(category), &category);
    if (status) fprintf(stderr, "Audio session initialization: %d\n", (int)status);

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_corefoundation_get_instance());
    hci_transport_h4_iphone_set_enforce_wake_device("/dev/btwake");
    hci_init(hci_transport_h4_instance(NULL), &transport_config);
    control = btstack_control_iphone;
    control.on = acquire_radio;
    control.off = release_radio;
    control.register_for_power_notifications = NULL;
    hci_set_control(&control);
    const btstack_tlv_t *database = btstack_tlv_posix_init_instance(&keys, DB_STATE_DIR "/link-keys.tlv");
    hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(database, &keys));
    btstack_audio_sink_set_instance(&dockbridge_audio_sink);
    if (dockbridge_start_a2dp()) return 1;
    events.callback = event_handler;
    hci_add_event_handler(&events);
    gap_ssp_set_enable(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(0);
    gap_set_bondable_mode(0);
    gap_set_class_of_device(0x240414);
    gap_set_security_level(LEVEL_2);
    gap_connectable_control(1);

    int result = hci_power_control(HCI_POWER_ON);
    if (!result) {
        deadline = btstack_run_loop_get_time_ms() + STARTUP_MS;
        btstack_run_loop_set_timer_handler(&timer, poll_state);
        btstack_run_loop_set_timer(&timer, POLL_MS);
        btstack_run_loop_add_timer(&timer);
        btstack_run_loop_execute();
    }
    dockbridge_audio_sink.close();
    if (took_radio) {
        hci_transport_h4_instance(NULL)->close();
        release_radio();
    }
    btstack_tlv_posix_deinit(&keys);
    close(lock);
    return result || failed ? 1 : 0;
}
