  🎯 Complete Redis-Centric Architecture (4 Pods)

  Standard Service Images:

  1. rtc-egress/api-server:latest - Task publisher (HTTP → Redis)
  2. rtc-egress/egress:latest - Native egress
  3. rtc-egress/flexible-recorder:latest - Web recorder service
  4. rtc-egress/uploader:latest - File watching & S3 uploads
  5. rtc-egress/webhook-notifier:latest - Redis keyspace listener → HTTP notifications

  Pod Architecture:

  ┌─── API Server Pod ───┐     ┌─── Egress Pod ───┐     ┌─── Flexible-Recorder Pod ───┐     ┌─── Webhook-Notifier Pod ───┐
  │ • REST API           │     │ • egress service │     │ • flexible-recorder service │     │ • Redis keyspace listener  │
  │ • Task validation    │────▶│ • 4x eg_workers  │     │ • web_recorder engine       │     │ • HTTP webhook sender      │
  │ • Push to Redis      │     │ • uploader       │     │ • uploader                  │     │ • Retry logic              │
  └──────────────────────┘     │ • pulls egress:* │     │ • pulls egress:web:*        │     │ • Notification dedup       │
           │                   └──────────────────┘     └─────────────────────────────┘     └────────────────────────────┘
           │                           ▲                              ▲                              ▲
           └────────── Redis ──────────┼──────────────────────────────┼──────────────────────────────┘
                (Task Queue +          │                              │
                 Keyspace Events)      │                              │
                                       │                              │
                             egress:record:*                  egress:web:record:*
                             egress:snapshot:*                egress:web:snapshot:*

  Scaling Strategy:

  # Independent scaling per service type
  API Server Pods: 2-3 (stateless, just publish tasks)
  Egress Pods: 3-10 (auto-scale based on egress:* queue length)
  Flexible-Recorder Pods: 1-5 (auto-scale based on egress:web:* queue length)
  Webhook-Notifier Pods: 1-3 (auto-scale based on notification volume)

  Service Responsibilities:

  API Server Pod:

  - ✅ HTTP REST API endpoints
  - ✅ Task validation and parsing
  - ✅ Redis task publishing (queue routing by layout)
  - ✅ Health checks
  - ❌ No actual processing

  Egress Pod(Pattern: egress:consumer:cmd:* or egress:region:consumer:cmd:*):

  - ✅ Redis consumer (egress:record:*, egress:snapshot:* or egress:us-east-1:record:*, egress:us-east-1:snapshot:*)
  - ✅ Native recording (flat/spotlight/customized layouts)
  - ✅ 4x eg_worker processes
  - ✅ S3 uploader for completed recordings(optional, in some case we need to deploy a separated uploader pod)
  - ✅ Task status updates to Redis

  Flexible-Recorder Pod:

  - ✅ Redis consumer (egress:web:record:*, egress:web:snapshot:* or egress:us-east-1:web:record:*, egress:us-east-1:web:snapshot:*)
  - ✅ Web recording (freestyle layout)
  - ✅ Web dispatch + web recorder engine
  - ✅ S3 uploader for completed web recordings
  - ✅ Task status updates to Redis

  Webhook-Notifier Pod:

  - ✅ Redis keyspace events listener (__keyspace@*__:egress:task:*)
  - ✅ HTTP webhook notifications
  - ✅ Retry logic with exponential backoff
  - ✅ Notification deduplication
  - ✅ State filtering (STOPPED/FAILED/PROCESSING)
