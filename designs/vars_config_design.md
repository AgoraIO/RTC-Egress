 Configuration Priority (highest to lowest):

  1. Environment variables from host machine (when docker run or docker compose up)
  2. Docker-compose.yml environment section (deployment configuration)
  3. egress_config.yaml (application defaults)

  Mode-specific behavior:

  - Standalone mode: All variables resolved through the 3 layers above
  - Managed mode: AGORA_APP_ID from the 3 layers above, AGORA_ACCESS_TOKEN/AGORA_CHANNEL_NAME passed dynamically per task via IPC

  Implementation flow:

  1. Application reads egress_config.yaml as base
  2. Docker entrypoint script overrides config values with docker-compose.yml environment variables
  3. Host environment variables (at container launch) override everything

  This means:
  - docker run -e AGORA_APP_ID=your_app_id (host env) beats docker-compose environment
  - Docker-compose environment: section beats egress_config.yaml
  - egress_config.yaml provides sensible defaults

  The entrypoint validation should only require AGORA_APP_ID to be resolvable through these layers, not mandate it as a host environment variable.
