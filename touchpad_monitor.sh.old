#!/bin/bash

TOUCHPAD_NAME="ELAN1206"

find_touchpad_event() {
  for device in /sys/class/input/event*/device/name; do
    if [ -f "$device" ]; then
      if grep -q "$TOUCHPAD_NAME" "$device"; then
        event_path=$(dirname "$device")
        event_num=$(basename "$(dirname "$event_path")" | sed 's/event//')
        if [[ $(cat "$device") == *"Touchpad"* ]]; then
          echo "/dev/input/event$event_num"
          return 0
        fi
      fi
    fi
  done
  return 1
}

TOUCHPAD_EVENT=$(find_touchpad_event)
if [ -z "$TOUCHPAD_EVENT" ]; then
  exit 1
fi

LAST_EVENT_TIME=$(date +%s)
IS_SLEEPING=false

exec 3< <(evtest "$TOUCHPAD_EVENT")

while true; do
  if IFS= read -r -t 1 line <&3; then
    CURRENT_TIME=$(date +%s)
    LAST_EVENT_TIME=$CURRENT_TIME

    if [ "$IS_SLEEPING" = true ]; then
      # wake
      echo 'idma64.1' | sudo tee /sys/bus/platform/drivers/idma64/bind
      IS_SLEEPING=false
    fi

  else
    CURRENT_TIME=$(date +%s)
    DIFF=$((CURRENT_TIME - LAST_EVENT_TIME))
    if [ "$DIFF" -ge 1 ] && [ "$IS_SLEEPING" = false ]; then
      # sleep
      echo 'idma64.1' | sudo tee /sys/bus/platform/drivers/idma64/unbind
      IS_SLEEPING=true
    fi
  fi
done
