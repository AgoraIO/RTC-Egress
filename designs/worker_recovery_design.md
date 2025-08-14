  # Worker/Egress Recovery Design
  
	// Lease settings for automatic failover
	LeaseRenewalInterval = 15 * time.Second // How often workers renew lease
	LeaseTimeout         = 45 * time.Second // When to consider worker dead(worker failure)
	CleanupInterval      = 5 * time.Second  // How often to check for expired leases and aged tasks

  Layer 1: Individual worker crash

  - Worker 2 crashes → monitorWorkers() detects → restarts only worker 2
  - Worker 2 gets same workerID=2 but in new process
  - Tasks assigned to worker 2 are recovered locally

  Layer 2: Entire egress crash

  - Entire pod crashes → new pod starts with new random podID
  - StartCleanupProcess() recovers all tasks with expired leases from old podID
  - All workers 0,1,2,3 restart fresh

  The random podID approach is perfect because the lease cleanup handles both layers automatically!

