#!/usr/bin/env bash
set -euo pipefail

DEFAULT_BASE_URL="http://localhost:8091"
DEFAULT_APP_ID="2655d20a82fc47cebcff82d5bd5d53ef"

usage() {
  cat <<USAGE
Usage:
  $(basename "$0") start [base_url] [app_id] <access_token>
  $(basename "$0") stop [base_url] [app_id] <task_id>
  $(basename "$0") status [base_url] [app_id] <task_id>

Defaults:
  base_url: ${DEFAULT_BASE_URL}
  app_id:   ${DEFAULT_APP_ID}

Examples:
  $(basename "$0") start myAccessToken
  $(basename "$0") start http://localhost:9000 customAppId myAccessToken
  $(basename "$0") stop aTaskId
  $(basename "$0") stop http://localhost:9000 customAppId aTaskId
  $(basename "$0") status aTaskId
  $(basename "$0") status http://localhost:9000 customAppId aTaskId
USAGE
}

random_request_id() {
  hexdump -n6 -v -e '/1 "%02x"' /dev/urandom
}

print_response() {
  local response=$1
  if command -v jq >/dev/null 2>&1; then
    if ! printf '%s' "$response" | jq .; then
      printf '%s\n' "$response"
    fi
  else
    printf '%s\n' "$response"
  fi
}

start_request() {
  local base_url=$1
  local app_id=$2
  local access_token=$3
  local request_id
  request_id=$(random_request_id)
  local payload
  payload=$(cat <<JSON
{
  "request_id": "${request_id}",
  "cmd": "record",
  "payload": {
    "layout": "flat",
    "channel": "egress_test",
    "access_token": "${access_token}",
    "workerUid": 42,
    "users": ["803231", "322353"],
    "filename_pattern": "file_output",
    "format": "hls",
    "video": {
      "width": 1280,
      "height": 720,
      "frameRate": 30,
      "bitrate": 1000
    },
    "audio": {
      "sampleRate": 48000,
      "channels": 2,
      "bitrate": 128
    }
  }
}
JSON
  )
  local response
  response=$(curl -sS -X POST "${base_url}/egress/v1/${app_id}/tasks" \
    -H "Content-Type: application/json" \
    -d "${payload}")

  print_response "$response"
}

stop_request() {
  local base_url=$1
  local app_id=$2
  local task_id=$3
  local request_id
  request_id=$(random_request_id)

  local payload
  payload=$(cat <<JSON
{
  "request_id": "${request_id}"
}
JSON
  )

  local response
  response=$(curl -sS -X POST "${base_url}/egress/v1/${app_id}/tasks/${task_id}/stop" \
    -H "Content-Type: application/json" \
    -d "${payload}")

  print_response "$response"
}

status_request() {
  local base_url=$1
  local app_id=$2
  local task_id=$3
  local request_id
  request_id=$(random_request_id)

  local payload
  payload=$(cat <<JSON
{
  "request_id": "${request_id}"
}
JSON
  )

  local response
  response=$(curl -sS -X POST "${base_url}/egress/v1/${app_id}/tasks/${task_id}/status" \
    -H "Content-Type: application/json" \
    -d "${payload}")

  print_response "$response"
}

main() {
  if [[ $# -lt 1 ]]; then
    usage
    exit 1
  fi

  local command=$1
  shift

  case $command in
    start)
      if [[ $# -lt 1 || $# -gt 3 ]]; then
        usage
        exit 1
      fi

      local base_url=${DEFAULT_BASE_URL}
      local app_id=${DEFAULT_APP_ID}
      local access_token
      case $# in
        1)
          access_token=$1
          ;;
        2)
          if [[ $1 == http*://* ]]; then
            base_url=$1
            access_token=$2
          else
            app_id=$1
            access_token=$2
          fi
          ;;
        3)
          base_url=$1
          app_id=$2
          access_token=$3
          ;;
      esac
      start_request "$base_url" "$app_id" "$access_token"
      ;;
    stop)
      if [[ $# -lt 1 || $# -gt 3 ]]; then
        usage
        exit 1
      fi

      local base_url=${DEFAULT_BASE_URL}
      local app_id=${DEFAULT_APP_ID}
      local task_id

      case $# in
        1)
          task_id=$1
          ;;
        2)
          if [[ $1 == http*://* ]]; then
            base_url=$1
            task_id=$2
          else
            app_id=$1
            task_id=$2
          fi
          ;;
        3)
          base_url=$1
          app_id=$2
          task_id=$3
          ;;
      esac

      stop_request "$base_url" "$app_id" "$task_id"
      ;;
    status)
      if [[ $# -lt 1 || $# -gt 3 ]]; then
        usage
        exit 1
      fi

      local base_url=${DEFAULT_BASE_URL}
      local app_id=${DEFAULT_APP_ID}
      local task_id

      case $# in
        1)
          task_id=$1
          ;;
        2)
          if [[ $1 == http*://* ]]; then
            base_url=$1
            task_id=$2
          else
            app_id=$1
            task_id=$2
          fi
          ;;
        3)
          base_url=$1
          app_id=$2
          task_id=$3
          ;;
      esac

      status_request "$base_url" "$app_id" "$task_id"
      ;;
    *)
      usage
      exit 1
      ;;
  esac
}

main "$@"
