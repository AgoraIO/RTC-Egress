# Task Dispatching Design

## Concepts
  1. State Definition
	- TaskStatePending    = "PENDING"    // Task waiting in queue (can timeout after TaskTimeout)
	- TaskStateProcessing = "PROCESSING" // Worker actively working on task
	- TaskStateSuccess    = "SUCCESS"    // Task completed successfully
	- TaskStateFailed     = "FAILED"     // Fatal error(Can not be retried/recovered), like wrong access_token, no more disk space, etc. no user stop call needed
	- TaskStateTimeout    = "TIMEOUT"    // Task aged out while PENDING (business staleness). no user stop call needed


  2. Atomic State Transitions


  - PENDING → PROCESSING: Worker atomically moves task via BRPopLPush
  - PENDING → TIMEOUT: Cleanup atomically checks queue presence + state with WATCH
  - PROCESSING → PENDING: Worker failure recovery
  - PROCESSING → SUCCESS/FAILED: Worker completion


  3. Task State Transitions
  | Scenario            | State   | Retry? | Stays in Redis?         | Timeout Window        |
  |---------------------|---------|--------|---------------------|-----------------------|
  | Business timeout    | TIMEOUT | ❌ No   | ✅ Yes                  | 30s from last PENDING |
  | Worker crash        | PENDING | ✅ Yes  | ✅ Yes                  | Fresh 30s window      |
  | Unrecoverable error | FAILED  | ❌ No   | ✅ Yes(Terminate task)  | N/A                   |
  | Success             | SUCCESS | ❌ No   | ✅ Yes                  | N/A                   |


  4. Race Condition Prevention

  // Atomic check: task must be BOTH in queue AND still PENDING
  rq.client.Watch(ctx, func(tx *redis.Tx) error {
      // Check state is PENDING
      // Check task age > 30s
      // Check task still in queue (not picked up by worker)
      // If all true: atomically mark TIMEOUT + remove from queue
  }, taskKey, queueName)


  5. State Flow

  PENDING (age ≤ 30s) → Worker picks up → PROCESSING → SUCCESS/FAILED
  PENDING (age > 30s) → Cleanup marks → TIMEOUT (final state)
  PROCESSING → Worker crashes → Cleanup recovers → PENDING (retry)


  6. Key Benefits

  - ✅ Time-sensitive: 30s timeout for RTC egress
  - ✅ Atomic: No race conditions between worker pickup and timeout
  - ✅ Resource efficient: TIMEOUT tasks stop processing
  - ✅ Clear separation: Business timeout vs worker failure
  - ✅ Future ready: TIMEOUT state ready for notification module

  The implementation ensures that PENDING tasks can only transition to either PROCESSING (by worker) or TIMEOUT (by cleanup), never both

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
    "state": "PROCESSING", // PENDING, PROCESSING, SUCCESS, FAILED
    "created_at": "2025-01-13T14:32:18Z",
    "pending_at": "2025-01-13T14:32:18Z",
    "processed_at": "2025-01-13T14:32:19Z",
    "completed_at": null,
    "error": "",
    "worker_id": 1, // Worker ID
    "lease_expiry": "2025-01-13T14:33:04Z",
    "retry_count": 0
  }
```

## Task State Transitions
### Worker failure/crash/loss pushes task to PENDING. BE atomic, one PENDING task can only be proccesing by one worker(marked as PROCESSING) or marked as TIMEOUT. All the task state transition should be atomic.
### No task loss, No task duplication
