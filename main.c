/*
 * touchpad-monitor: keep the ELAN1206 I2C touchpad usable on Linux >= 6.9.
 *
 * Background (ASUS ZenBook UX564EH, Intel Tiger Lake LPSS):
 *   The interrupt line shared by the touchpad's I2C controller
 *   (i2c_designware.N) and its DMA engine (idma64.N) is permanently asserted
 *   on this hardware.  Since kernel 6.9 (commit 9140ce47872b, "dmaengine:
 *   idma64: Don't try to serve interrupts when device is powered off") the
 *   idma64 handler no longer claims those interrupts, so the kernel logs
 *   "irq N: nobody cared", disables the line, and from then on the I2C
 *   controller is only serviced by the 10 Hz spurious-IRQ poller.  The
 *   touchpad still "works" but reports at 10 Hz, which libinput cannot use.
 *
 * What this program does:
 *   Re-binding idma64.N re-enables the disabled interrupt (the kernel's
 *   shared-IRQ setup path clears the spurious-disabled state).  A device can
 *   only be bound after being unbound, so: unbind idma64.N after a short idle
 *   period, re-bind it on the next touch event.  While the touchpad is in use
 *   enough real I2C interrupts are handled that the kernel keeps the line
 *   enabled; once it goes idle the kernel disables the storming line again
 *   within a few seconds, bound or not.
 *
 * Runs as root (needs /dev/input/event* and /sys/bus/platform/drivers/idma64).
 */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_TOUCHPAD_NAME "ELAN1206"
#define DEFAULT_DMA_DEVICE "idma64.1"
#define DEFAULT_IDLE_SECONDS 1.0
#define IDMA_DRIVER_DIR "/sys/bus/platform/drivers/idma64"
#define RETRY_SECONDS 2

static volatile sig_atomic_t stop_requested = 0;
static int verbose = 0;
static int dry_run = 0;

static void on_signal(int sig) {
  (void)sig;
  stop_requested = 1;
}

static void logmsg(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("touchpad-monitor: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/*
 * Find /dev/input/eventN for the touchpad.  Returns N, or -1 if not found.
 * If dma_dev is non-NULL, derive the DMA engine paired with the touchpad's
 * I2C controller from its sysfs path, e.g.
 *   .../0000:00:15.1/i2c_designware.1/i2c-1/i2c-ELAN1206:00/...  ->  idma64.1
 */
static int find_touchpad(const char *name, char *dma_dev, size_t dma_len) {
  DIR *dir = opendir("/sys/class/input");
  if (dir == NULL) {
    logmsg("opendir /sys/class/input: %s", strerror(errno));
    return -1;
  }

  int found = -1;
  struct dirent *entry;
  while (found < 0 && (entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "event", 5) != 0)
      continue;

    char path[PATH_MAX];
    snprintf(path, sizeof path, "/sys/class/input/%s/device/name",
             entry->d_name);
    FILE *f = fopen(path, "r");
    if (f == NULL)
      continue;
    char devname[256] = "";
    if (fgets(devname, sizeof devname, f) == NULL)
      devname[0] = '\0';
    fclose(f);
    devname[strcspn(devname, "\n")] = '\0';

    if (strstr(devname, name) == NULL || strstr(devname, "Touchpad") == NULL)
      continue;

    int num;
    if (sscanf(entry->d_name, "event%d", &num) != 1)
      continue;
    found = num;

    if (dma_dev != NULL) {
      char real[PATH_MAX];
      int ctrl;
      snprintf(path, sizeof path, "/sys/class/input/%s/device", entry->d_name);
      const char *dw = realpath(path, real) ? strstr(real, "i2c_designware.")
                                            : NULL;
      if (dw != NULL && sscanf(dw, "i2c_designware.%d", &ctrl) == 1)
        snprintf(dma_dev, dma_len, "idma64.%d", ctrl);
    }
  }
  closedir(dir);
  return found;
}

static int dma_bound(const char *dma_dev) {
  char path[PATH_MAX];
  snprintf(path, sizeof path, IDMA_DRIVER_DIR "/%s", dma_dev);
  return access(path, F_OK) == 0;
}

/* Write dma_dev to the idma64 driver's bind or unbind file.  0 on success. */
static int dma_set_bound(const char *dma_dev, int bind) {
  char path[PATH_MAX];
  snprintf(path, sizeof path, IDMA_DRIVER_DIR "/%s", bind ? "bind" : "unbind");
  if (verbose)
    logmsg("%s %s", bind ? "bind" : "unbind", dma_dev);
  if (dry_run)
    return 0;

  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    logmsg("open %s: %s", path, strerror(errno));
    return -1;
  }
  ssize_t n = write(fd, dma_dev, strlen(dma_dev));
  int saved = errno;
  close(fd);
  if (n < 0) {
    logmsg("write \"%s\" to %s: %s", dma_dev, path, strerror(saved));
    return -1;
  }
  return 0;
}

/*
 * Open the touchpad's event device, waiting for it to appear (the service may
 * start before the touchpad is probed, or the device may be re-created after
 * suspend).  Returns an fd, or -1 if a stop was requested while waiting.
 */
static int open_touchpad(const char *name, const char *event_path,
                         char *dma_dev, size_t dma_len) {
  int announced = 0;
  while (!stop_requested) {
    char path[PATH_MAX];
    int num = -1;
    if (event_path != NULL) {
      snprintf(path, sizeof path, "%s", event_path);
      num = 0;
    } else {
      num = find_touchpad(name, dma_dev, dma_len);
      if (num >= 0)
        snprintf(path, sizeof path, "/dev/input/event%d", num);
    }
    if (num >= 0) {
      int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (fd >= 0) {
        logmsg("using %s (%s), DMA device %s", path, name, dma_dev);
        return fd;
      }
      if (!announced)
        logmsg("open %s: %s (retrying every %d s)", path, strerror(errno),
               RETRY_SECONDS);
    } else if (!announced) {
      logmsg("touchpad \"%s\" not found (retrying every %d s)", name,
             RETRY_SECONDS);
    }
    announced = 1;
    sleep(RETRY_SECONDS);
  }
  return -1;
}

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s [-t idle_seconds] [-n touchpad_name] [-d dma_device] "
          "[-e event_device] [-v] [-D] [-l]\n"
          "  -t  seconds without touch events before unbinding the DMA device "
          "(default %.1f)\n"
          "  -n  substring of the touchpad's input device name (default %s)\n"
          "  -d  DMA platform device to toggle (default: derived from the "
          "touchpad's I2C controller, else %s)\n"
          "  -e  path of the touchpad's event device (skips detection)\n"
          "  -v  log every bind/unbind\n"
          "  -D  dry run: never write to sysfs\n"
          "  -l  only print the detected touchpad and DMA device, then exit\n",
          argv0, DEFAULT_IDLE_SECONDS, DEFAULT_TOUCHPAD_NAME,
          DEFAULT_DMA_DEVICE);
}

int main(int argc, char **argv) {
  const char *name = DEFAULT_TOUCHPAD_NAME;
  const char *event_path = NULL;
  double idle = DEFAULT_IDLE_SECONDS;
  char dma_dev[32] = DEFAULT_DMA_DEVICE;
  int dma_forced = 0, list_only = 0, opt;

  while ((opt = getopt(argc, argv, "t:n:d:e:vDlh")) != -1) {
    switch (opt) {
    case 't':
      idle = atof(optarg);
      if (idle <= 0) {
        usage(argv[0]);
        return 2;
      }
      break;
    case 'n':
      name = optarg;
      break;
    case 'd':
      snprintf(dma_dev, sizeof dma_dev, "%s", optarg);
      dma_forced = 1;
      break;
    case 'e':
      event_path = optarg;
      break;
    case 'v':
      verbose = 1;
      break;
    case 'D':
      dry_run = 1;
      break;
    case 'l':
      list_only = 1;
      break;
    default:
      usage(argv[0]);
      return 2;
    }
  }

  if (list_only) {
    int num = find_touchpad(name, dma_forced ? NULL : dma_dev, sizeof dma_dev);
    if (num < 0) {
      logmsg("touchpad \"%s\" not found", name);
      return 1;
    }
    printf("touchpad: /dev/input/event%d\ndma device: %s (%s)\n", num, dma_dev,
           dma_bound(dma_dev) ? "bound" : "unbound");
    return 0;
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal; /* no SA_RESTART: epoll_wait returns EINTR */
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  int epfd = epoll_create1(EPOLL_CLOEXEC);
  if (epfd < 0) {
    logmsg("epoll_create1: %s", strerror(errno));
    return 1;
  }

  int fd = -1;
  int sleeping = 0; /* 1 while the DMA device is unbound */
  double last_event = now_seconds();

  while (!stop_requested) {
    if (fd < 0) {
      fd = open_touchpad(name, event_path, dma_forced ? NULL : dma_dev,
                         sizeof dma_dev);
      if (fd < 0)
        break;
      struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
      if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        logmsg("epoll_ctl: %s", strerror(errno));
        close(fd);
        fd = -1;
        sleep(RETRY_SECONDS);
        continue;
      }
      /* Resync with the real state, e.g. after a restart while unbound. */
      sleeping = !dma_bound(dma_dev);
      last_event = now_seconds();
    }

    /* Awake: wake up when the idle deadline passes.  Asleep: wait for touch. */
    int timeout_ms = -1;
    if (!sleeping) {
      double remain = last_event + idle - now_seconds();
      timeout_ms = remain > 0 ? (int)(remain * 1000) + 1 : 0;
    }

    struct epoll_event ev;
    int n = epoll_wait(epfd, &ev, 1, timeout_ms);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      logmsg("epoll_wait: %s", strerror(errno));
      break;
    }
    if (n == 0) {
      if (!sleeping && now_seconds() - last_event >= idle) {
        sleeping = 1;
        dma_set_bound(dma_dev, 0);
      }
      continue;
    }

    /* Drain every queued event; one read per wake-up would spin. */
    int got = 0, lost = 0, lost_errno = 0;
    for (;;) {
      struct input_event buf[64];
      ssize_t r = read(fd, buf, sizeof buf);
      if (r > 0) {
        got = 1;
        continue;
      }
      if (r < 0 && errno == EINTR)
        continue;
      if (r < 0 && errno == EAGAIN)
        break;
      lost = 1; /* EOF or ENODEV: the device went away */
      lost_errno = r < 0 ? errno : 0;
      break;
    }

    if (got) {
      last_event = now_seconds();
      if (sleeping) {
        sleeping = 0;
        dma_set_bound(dma_dev, 1);
      }
    }

    if (lost || (ev.events & (EPOLLHUP | EPOLLERR))) {
      logmsg("touchpad device lost (%s), rediscovering",
             lost_errno ? strerror(lost_errno) : "hang-up");
      epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
      close(fd);
      fd = -1;
    }
  }

  /* Leave the system in its stock state (DMA device bound) on exit. */
  if (sleeping && !dry_run && !dma_bound(dma_dev))
    dma_set_bound(dma_dev, 1);
  if (fd >= 0)
    close(fd);
  close(epfd);
  return 0;
}
