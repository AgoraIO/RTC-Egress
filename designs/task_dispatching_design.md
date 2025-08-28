# Task Dispatching Design

  Rules
  
  ✅ Web/Native Shared Dispatch Logic

  - Created TaskDispatcher interface that abstracts the Redis polling, lease management, and regional prioritization
  - Both native and web recorder systems use the same dispatch logic
  - Implemented TaskProcessor interface for pluggable task handling

  ✅ WorkerManagerWebRecorderProxy

  - Created HTTP client that delegates tasks to external web recorder service
  - Implements retry logic, timeout handling, and proper error propagation
  - Supports authentication via bearer tokens
  - Handles start/stop/status operations via RESTful API

  ✅ Standalone Flexible Recorder

  - Built as cmd/flexible-recorder with streamlined naming convention
  - Integrated with existing build.sh system
  - Uses same configuration patterns as native egress service
  - Supports both YAML config files and environment variables

  ✅ Proper Task Segregation

  - Native tasks: egress:record:*, egress:snapshot:*
  - Web tasks: egress:web:record:*, egress:web:snapshot:*
  - Different workers automatically handle their respective task types

  ✅ Full Feature Parity

  - Redis-based task queuing and processing
  - Lease management and automatic failover
  - Regional task prioritization
  - Health monitoring and pod registration
  - Graceful shutdown and error handling

## Concepts
  1. State Definition
	- TaskStateEnqueued    = "ENQUEUED"    // Task waiting in queue (can timeout after TaskTimeout)
	- TaskStateProcessing = "PROCESSING" // Worker actively working on task
	- TaskStateSuccess    = "SUCCESS"    // Task completed successfully
	- TaskStateFailed     = "FAILED"     // Fatal error(Can not be retried/recovered), like wrong access_token, no more disk space, etc. no user stop call needed
	- TaskStateTimeout    = "TIMEOUT"    // Task aged out while ENQUEUEDs (business staleness). no user stop call needed


  2. Atomic State Transitions


  - ENQUEUED → PROCESSING: Worker atomically moves task via BRPopLPush
  - ENQUEUED → TIMEOUT: Cleanup atomically checks queue presence + state with WATCH
  - PROCESSING → ENQUEUED: Worker failure recovery
  - PROCESSING → SUCCESS/FAILED: Worker completion


  3. Task State Transitions
  | Scenario            | State    | Retry?  | Stays in Redis?         | Timeout Window        |
  |---------------------|----------|---------|---------------------|-----------------------|
  | Business timeout    | TIMEOUT  | ❌ No   | ✅ Yes                  | 30s from last ENQUEUED |
  | Worker crash        | ENQUEUED | ✅ Yes  | ✅ Yes                  | Fresh 30s window      |
  | Unrecoverable error | FAILED   | ❌ No   | ✅ Yes(Terminate task)  | N/A                   |
  | Success             | SUCCESS  | ❌ No   | ✅ Yes                  | N/A                   |


  4. Race Condition Prevention

  // Atomic check: task must be BOTH in queue AND still ENQUEUED
  rq.client.Watch(ctx, func(tx *redis.Tx) error {
      // Check state is ENQUEUED
      // Check task age > 30s
      // Check task still in queue (not picked up by worker)
      // If all true: atomically mark TIMEOUT + remove from queue
  }, taskKey, queueName)


  5. State Flow

  ENQUEUED (age ≤ 30s) → Worker picks up → PROCESSING → SUCCESS/FAILED
  ENQUEUED (age > 30s) → Cleanup marks → TIMEOUT (final state)
  PROCESSING → Worker crashes → Cleanup recovers → ENQUEUED (retry)


  6. Key Benefits

  - ✅ Time-sensitive: 30s timeout for RTC egress
  - ✅ Atomic: No race conditions between worker pickup and timeout
  - ✅ Resource efficient: TIMEOUT tasks stop processing
  - ✅ Clear separation: Business timeout vs worker failure
  - ✅ Future ready: TIMEOUT state ready for notification module

  The implementation ensures that ENQUEUED tasks can only transition to either PROCESSING (by worker) or TIMEOUT (by cleanup), never both

## Pull-Based Regional Architecture

### Core Concept: Pods Pull Tasks

```mermaid
  ┌─────────────────────────────────────────────────────────┐
  │                Redis Task Queues                        │
  │  ┌─────────────────┐  ┌─────────────────┐               │
  │  │ Regional Queues │  │ Global Queues   │               │
  │  │ (High Priority) │  │ (Overflow)      │               │
  │  └─────────────────┘  └─────────────────┘               │
  └─────────────────────────────────────────────────────────┘
             ▲                    ▲
             │ PULL               │ PULL
             │                    │
  ┌──────────┴─────────┐  ┌───────┴──────────┐  ┌──────────────────┐
  │   us-east-1        │  │   eu-west-1      │  │   ap-south-1     │
  │                    │  │                  │  │                  │
  │ Pod-xdew1: [0][1]  │  │ Pod-gf32: [0][1] │  │ Pod-hf32: [2][3]  │
  │ Pod-xdew2: [2][3]  │  │ Pod-gcs4: [2][4] │  │ Pod-xdew3: [4][5]  │
  │        ↑           │  │        ↑         │  │        ↑         │
  │    Fetch Tasks     │  │    Fetch Tasks   │  │    Fetch Tasks   │
  └────────────────────┘  └──────────────────┘  └──────────────────┘
```  

### Task Object (JSON stored in Redis)

```json
  {
    "id": "a1b2c3d4e5f6g7h8", // Your specified task ID
    "type": "snapshot",
    "action": "start",
    "channel": "meeting-room-alpha",
    "request_id": "req_usr79_001",
    "payload": {
      "access_token": "007eJxSULJSqLNSUCjPyM9VsLJSUKOhKFNrq1CqVGOkZpZUqmknKpUw1AD",
      "workerUid": 42,
      "interval_in_ms": 15000,
      "layout": "flat",
      "uid": ["user123", "user456"]
    },
    "state": "PROCESSING", // ENQUEUED, PROCESSING, SUCCESS, FAILED
    "created_at": "2025-01-13T14:32:18Z",
    "enqueued_at": "2025-01-13T14:32:18Z",
    "processed_at": "2025-01-13T14:32:19Z",
    "completed_at": null,
    "error": "",
    "worker_id": 1, // Worker ID
    "lease_expiry": "2025-01-13T14:33:04Z",
    "retry_count": 0
  }
```

## Task State Transitions
### Worker failure/crash/loss pushes task to ENQUEUED. BE atomic, one ENQUEUED task can only be proccesing by one worker(marked as PROCESSING) or marked as TIMEOUT. All the task state transition should be atomic.
### No task loss, No task duplication
