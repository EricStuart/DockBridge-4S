#include "bridge_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define AUTO_MODE_FILE DB_STATE_DIR "/auto-receive"
enum { RETRY_FIRST = 3, RETRY_MAX = 60, STABLE_SECONDS = 60 };
static volatile sig_atomic_t exiting;
static pid_t worker;
static int event_pipe = -1;
static db_worker_event worker_event;
static size_t event_received;
static double deadline, retry_at, ready_since;
static unsigned retry_delay = RETRY_FIRST;
static uint8_t auto_mode;
static int radio_taken;
static db_reply state;

static double monotonic_seconds(void) {
    static mach_timebase_info_data_t scale;
    if (!scale.denom) mach_timebase_info(&scale);
    return mach_absolute_time() * (double)scale.numer / scale.denom / 1000000000.0;
}

static uint8_t read_auto_mode(void) {
    int fd = open(AUTO_MODE_FILE, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return DB_AUTO_OFF;
    char data[3];
    ssize_t count = read(fd, data, sizeof(data));
    close(fd);
    if (count != 2 || data[1] != '\n' || data[0] < '0' || data[0] > '2') return DB_AUTO_OFF;
    return (uint8_t)(data[0] - '0');
}

static int save_auto_mode(uint8_t mode) {
    char temporary[] = AUTO_MODE_FILE ".XXXXXX";
    int fd = mkstemp(temporary);
    if (fd < 0) return -1;
    const char data[] = { (char)('0' + mode), '\n' };
    int failed = write(fd, data, sizeof(data)) != sizeof(data) || fsync(fd);
    if (close(fd)) failed = 1;
    if (failed || rename(temporary, AUTO_MODE_FILE)) {
        unlink(temporary);
        return -1;
    }
    auto_mode = mode;
    return 0;
}

static void schedule_retry(double now) {
    if (exiting || auto_mode != DB_AUTO_ARMED) return;
    fprintf(stderr, "DockBridge: restarting receiver in %u seconds.\n", retry_delay);
    retry_at = now + retry_delay;
    retry_delay = retry_delay <= RETRY_MAX / 2 ? retry_delay * 2 : RETRY_MAX;
}

static void stop_service(int signal_number) {
    (void)signal_number;
    exiting = 1;
}

static int start_worker(int listener, int automatic) {
    int events[2];
    if (pipe(events)) return -1;
    pid_t pid = fork();
    if (!pid) {
        close(listener);
        close(events[0]);
        if (events[1] != DB_WORKER_FD) {
            if (dup2(events[1], DB_WORKER_FD) < 0) _exit(126);
            close(events[1]);
        }
        int log = open(DB_STATE_DIR "/run.log", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
        if (log >= 0) {
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
            close(log);
        }
        execl(DB_EXECUTABLE, DB_EXECUTABLE, automatic ? "--worker-automatic" : "--worker", (char *)NULL);
        _exit(126);
    }
    close(events[1]);
    if (pid < 0) {
        close(events[0]);
        return -1;
    }
    fcntl(events[0], F_SETFL, O_NONBLOCK);
    fcntl(events[0], F_SETFD, FD_CLOEXEC);
    event_pipe = events[0];
    event_received = 0;
    worker = pid;
    retry_at = ready_since = 0;
    state = (db_reply){ .phase = DB_STARTING };
    radio_taken = 0;
    deadline = monotonic_seconds() + 40;
    return 0;
}

static void read_events(void) {
    if (event_pipe < 0) return;
    ssize_t count;
    while ((count = read(event_pipe, (uint8_t *)&worker_event + event_received,
                         sizeof(worker_event) - event_received)) > 0) {
        event_received += (size_t)count;
        if (event_received < sizeof(worker_event)) continue;
        event_received = 0;
        switch (worker_event.kind) {
            case DB_EVENT_RADIO_ACQUIRE: radio_taken = 1; break;
            case DB_EVENT_RADIO_RELEASE: radio_taken = 0; break;
            case DB_EVENT_READY:
                if (state.phase == DB_STARTING) {
                    state.phase = DB_RECEIVING;
                    ready_since = monotonic_seconds();
                }
                break;
            case DB_EVENT_PAIR_OPEN: state.pairing = 1; break;
            case DB_EVENT_PAIR_CLOSED: state.pairing = 0; break;
            case DB_EVENT_AUDIO_STARTED: state.audio = 1; break;
            case DB_EVENT_AUDIO_STOPPED: state.audio = 0; break;
            case DB_EVENT_AUDIO_ERROR: state.audio = 2; break;
            case DB_EVENT_INIT_TIMEOUT:
                if (!state.diagnostic) state.diagnostic = DB_DIAGNOSTIC_TIMEOUT;
                break;
            case DB_EVENT_TRANSPORT_ERROR: state.diagnostic = DB_DIAGNOSTIC_TRANSPORT; break;
            case DB_EVENT_MEDIA:
                if (state.phase == DB_RECEIVING) state.media = worker_event.media;
                break;
        }
    }
}

static void restore_after_crash(void) {
    static const char *const jobs[] = {
        "/System/Library/LaunchDaemons/com.apple.BlueTool.plist",
        "/System/Library/LaunchDaemons/com.apple.BTServer.le.plist",
        "/System/Library/LaunchDaemons/com.apple.BTServer.plist"
    };
    for (unsigned i = 0; i < sizeof(jobs) / sizeof(*jobs); i++) {
        if (access(jobs[i], F_OK)) continue;
        pid_t pid = fork();
        if (!pid) {
            execl("/bin/launchctl", "launchctl", "load", jobs[i], (char *)NULL);
            _exit(127);
        }
        if (pid > 0) {
            while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        }
    }
}

static void reap_worker(void) {
    if (!worker) return;
    int result;
    if (waitpid(worker, &result, WNOHANG) != worker) return;
    read_events();
    if (radio_taken) restore_after_crash();
    radio_taken = 0;
    close(event_pipe);
    event_pipe = -1;
    worker = 0;
    uint8_t phase = WIFEXITED(result) && WEXITSTATUS(result) == 0 ? DB_SYSTEM : DB_FAILED;
    state = (db_reply){ .phase = phase, .diagnostic = phase == DB_FAILED ? state.diagnostic : DB_DIAGNOSTIC_NONE };
    double now = monotonic_seconds();
    if (ready_since && now - ready_since >= STABLE_SECONDS) retry_delay = RETRY_FIRST;
    schedule_retry(now);
}

static void request_stop(void) {
    if (!worker || state.phase == DB_STOPPING) return;
    kill(worker, SIGTERM);
    deadline = monotonic_seconds() + 9;
    state = (db_reply){ .phase = DB_STOPPING };
}

static void retry_receiver(int listener, double now) {
    if (exiting || worker || auto_mode != DB_AUTO_ARMED || !retry_at || now < retry_at) return;
    retry_at = 0;
    if (start_worker(listener, 1)) {
        state = (db_reply){ .phase = DB_FAILED };
        schedule_retry(now);
    }
}

static uint8_t handle_command(int listener, uint8_t command) {
    switch (command) {
        case DB_STATUS: return DB_OK;
        case DB_START:
            if (worker) return DB_BUSY;
            if (auto_mode == DB_AUTO_PAUSED && save_auto_mode(DB_AUTO_ARMED)) return DB_ERROR;
            if (!start_worker(listener, 0)) return DB_OK;
            state = (db_reply){ .phase = DB_FAILED };
            schedule_retry(monotonic_seconds());
            return DB_ERROR;
        case DB_STOP: {
            retry_at = 0;
            int failed = auto_mode != DB_AUTO_OFF && save_auto_mode(DB_AUTO_PAUSED);
            if (failed) auto_mode = DB_AUTO_PAUSED;
            if (worker) request_stop();
            else state = (db_reply){ .phase = DB_SYSTEM };
            return failed ? DB_ERROR : DB_OK;
        }
        case DB_PAIR:
            return !worker || state.phase != DB_RECEIVING || kill(worker, SIGUSR1) ? DB_BUSY : DB_OK;
        case DB_AUTO_ENABLE:
            if (state.phase == DB_STOPPING) return DB_BUSY;
            if (save_auto_mode(DB_AUTO_ARMED)) return DB_ERROR;
            retry_delay = RETRY_FIRST;
            if (worker || !start_worker(listener, 0)) return DB_OK;
            state = (db_reply){ .phase = DB_FAILED };
            schedule_retry(monotonic_seconds());
            return DB_ERROR;
        case DB_AUTO_DISABLE:
            if (save_auto_mode(DB_AUTO_OFF)) return DB_ERROR;
            retry_at = 0;
            return DB_OK;
        default: return DB_ERROR;
    }
}

static void serve_client(int listener) {
    int client = accept(listener, NULL, NULL);
    if (client < 0) return;
    fcntl(client, F_SETFD, FD_CLOEXEC);
    uid_t uid;
    gid_t gid;
    struct pollfd input = { .fd = client, .events = POLLIN };
    uint8_t command;
    if (getpeereid(client, &uid, &gid) || (uid != 0 && uid != 501) ||
        poll(&input, 1, 200) <= 0 || read(client, &command, 1) != 1) {
        close(client);
        return;
    }
    uint8_t result = handle_command(listener, command);
    db_reply reply = state;
    reply.result = result;
    reply.auto_mode = auto_mode;
    write(client, &reply, sizeof(reply));
    close(client);
}

int dockbridge_service(void) {
    if (getuid() != 0) return 2;
    umask(077);
    int lock = open(DB_STATE_DIR "/service.lock", O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) return 1;
    fcntl(lock, F_SETFD, FD_CLOEXEC);
    struct stat info;
    if (lstat(DB_SOCKET_DIR, &info) || !S_ISDIR(info.st_mode) || info.st_uid != 0 || (info.st_mode & 022)) return 1;
    if (!lstat(DB_SOCKET_PATH, &info)) {
        if (!S_ISSOCK(info.st_mode) || info.st_uid != 0 || unlink(DB_SOCKET_PATH)) return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, stop_service);
    signal(SIGINT, stop_service);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) return 1;
    fcntl(listener, F_SETFD, FD_CLOEXEC);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    strcpy(address.sun_path, DB_SOCKET_PATH);
    address.sun_len = SUN_LEN(&address);
    if (bind(listener, (struct sockaddr *)&address, address.sun_len) ||
        chmod(DB_SOCKET_PATH, 0666) || listen(listener, 4)) return 1;
    auto_mode = read_auto_mode();
    schedule_retry(monotonic_seconds());
    while (!exiting || worker) {
        if (exiting) request_stop();
        struct pollfd fds[] = {
            { .fd = exiting ? -1 : listener, .events = POLLIN },
            { .fd = event_pipe, .events = POLLIN }
        };
        poll(fds, 2, 250);
        read_events();
        reap_worker();
        if (worker && (state.phase == DB_STARTING || state.phase == DB_STOPPING) && monotonic_seconds() >= deadline) {
            if (state.phase == DB_STARTING) request_stop();
            else kill(worker, SIGKILL);
        }
        if (!exiting && (fds[0].revents & POLLIN)) serve_client(listener);
        retry_receiver(listener, monotonic_seconds());
    }
    close(listener);
    unlink(DB_SOCKET_PATH);
    close(lock);
    return 0;
}
