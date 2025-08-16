#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>
#define TOUCHPAD_NAME "ELAN1206"
#define MAX_PATH_LEN 256
#define MAX_LINE_LEN 256

int find_touchpad_event() {
  DIR *dir;
  struct dirent *entry;
  char device_path[MAX_PATH_LEN];
  int event_num;

  dir = opendir("/sys/class/input");
  if (dir == NULL) {
    perror("Error opening /sys/class/input directory");
    return -1;
  }

  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "event", 5) == 0) {
      snprintf(device_path, MAX_PATH_LEN, "/sys/class/input/%s/device/name",
               entry->d_name);

      if (access(device_path, F_OK) != -1) {
        FILE *device_file = fopen(device_path, "r");
        if (device_file != NULL) {
          char name_content[MAX_LINE_LEN];

          if (fgets(name_content, MAX_LINE_LEN, device_file) != NULL) {
            size_t len = strlen(name_content);
            if (len > 0 && name_content[len - 1] == '\n') {
              name_content[len - 1] = '\0';
            }

            if (strstr(name_content, TOUCHPAD_NAME) != NULL &&
                strstr(name_content, "Touchpad") != NULL) {

              sscanf(entry->d_name, "event%d", &event_num);

              fclose(device_file);
              break;
            }
          }
          fclose(device_file);
        }
      }
    }
  }

  closedir(dir);
  return event_num;
}
int stop_touch_pad() {
  FILE *fp = fopen("/sys/bus/platform/drivers/idma64/unbind", "w");
  if (fp == NULL) {
    perror("Failed to open file");
    return 1;
  }

  if (fprintf(fp, "idma64.1") < 0) {
    perror("Failed to write to file");
    fclose(fp);
    return 1;
  }

  fclose(fp);
  return 0;
}
int start_touch_pad() {
  FILE *fp = fopen("/sys/bus/platform/drivers/idma64/bind", "w");
  if (fp == NULL) {
    perror("Failed to open file");
    return 1;
  }

  if (fprintf(fp, "idma64.1") < 0) {
    perror("Failed to write to file");
    fclose(fp);
    return 1;
  }

  fclose(fp);
  return 0;
}
int main() {
  int touchpad_event = find_touchpad_event();
  char touchpad_event_path[MAX_PATH_LEN];
  if (touchpad_event != -1) {
    snprintf(touchpad_event_path, MAX_PATH_LEN, "/dev/input/event%d",
             touchpad_event);
    int fd = open(touchpad_event_path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
      perror("Failed to open device");
      return 1;
    }
    int epfd = epoll_create1(0);
    if (epfd < 0) {
      perror("epoll_create1 failed");
      close(fd);
      return 1;
    }
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
      perror("epoll_ctl failed");
      close(epfd);
      close(fd);
      return 1;
    }

    struct input_event event;
    time_t last_event_time = time(NULL);
    int is_sleeping = 0;

    while (1) {
      struct epoll_event events[1];
      int nfds = epoll_wait(epfd, events, 1, 1000);

      time_t current_time = time(NULL);

      if (!is_sleeping && difftime(current_time, last_event_time) >= 1.0 &&
          nfds == 0) {
        is_sleeping = 1;
        stop_touch_pad();
      }

      if (nfds > 0) {
        if (read(fd, &event, sizeof(event)) == sizeof(event)) {
          last_event_time = current_time;

          if (is_sleeping) {
            is_sleeping = 0;
            start_touch_pad();
          }
        }
      }
    }

    close(epfd);
    close(fd);
    return 0;
  } else {
    fprintf(stderr, "Touchpad device %s not found\n", TOUCHPAD_NAME);
    return 1;
  }
}
