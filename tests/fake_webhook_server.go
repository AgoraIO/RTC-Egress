package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"time"
)

// WebhookPayload represents the webhook notification payload
type WebhookPayload struct {
	TaskID    string                 `json:"taskId"`
	RequestID string                 `json:"requestId"`
	Cmd       string                 `json:"cmd"`
	Action    string                 `json:"action"`
	Channel   string                 `json:"channel"`
	State     string                 `json:"state"`
	CreatedAt time.Time              `json:"createdAt"`
	Timestamp time.Time              `json:"timestamp"`
	Data      map[string]interface{} `json:"data,omitempty"`
	Files     []interface{}          `json:"files,omitempty"`
	Error     string                 `json:"error,omitempty"`
	Message   string                 `json:"message,omitempty"`
}

var (
	receivedWebhooks []WebhookPayload
	webhookCount     int
)

func webhookHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	webhookCount++
	log.Printf("📥 Received webhook #%d from %s", webhookCount, r.RemoteAddr)

	// Log headers
	log.Printf("📋 Headers:")
	for name, values := range r.Header {
		for _, value := range values {
			log.Printf("  %s: %s", name, value)
		}
	}

	// Read and parse body
	var payload WebhookPayload
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
		log.Printf("❌ Error parsing webhook payload: %v", err)
		http.Error(w, "Invalid JSON payload", http.StatusBadRequest)
		return
	}

	// Log payload details
	log.Printf("📊 Webhook Details:")
	log.Printf("  Task ID: %s", payload.TaskID)
	log.Printf("  Request ID: %s", payload.RequestID)
	log.Printf("  Cmd: %s", payload.Cmd)
	log.Printf("  Action: %s", payload.Action)
	log.Printf("  Channel: %s", payload.Channel)
	log.Printf("  State: %s", payload.State)
	log.Printf("  Timestamp: %s", payload.Timestamp.Format(time.RFC3339))

	if payload.Error != "" {
		log.Printf("  ⚠️  Error: %s", payload.Error)
	}
	if payload.Message != "" {
		log.Printf("  💬 Message: %s", payload.Message)
	}
	if len(payload.Files) > 0 {
		log.Printf("  📁 Files: %d items", len(payload.Files))
	}
	if len(payload.Data) > 0 {
		log.Printf("  📄 Additional Data: %d fields", len(payload.Data))
	}

	// Store webhook
	receivedWebhooks = append(receivedWebhooks, payload)

	// Respond with success
	response := map[string]interface{}{
		"status":     "received",
		"timestamp":  time.Now(),
		"webhook_id": webhookCount,
		"message":    fmt.Sprintf("Webhook #%d processed successfully", webhookCount),
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	json.NewEncoder(w).Encode(response)

	log.Printf("✅ Webhook #%d processed successfully\n", webhookCount)
}

func statusHandler(w http.ResponseWriter, r *http.Request) {
	status := map[string]interface{}{
		"service":           "fake-webhook-server",
		"status":            "running",
		"port":              9999,
		"uptime":            time.Since(startTime).String(),
		"webhooks_received": webhookCount,
		"last_webhook":      nil,
	}

	if len(receivedWebhooks) > 0 {
		status["last_webhook"] = receivedWebhooks[len(receivedWebhooks)-1]
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(status)
}

func webhooksHandler(w http.ResponseWriter, r *http.Request) {
	response := map[string]interface{}{
		"total_count": webhookCount,
		"webhooks":    receivedWebhooks,
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(response)
}

func clearHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	oldCount := webhookCount
	receivedWebhooks = []WebhookPayload{}
	webhookCount = 0

	response := map[string]interface{}{
		"status":         "cleared",
		"previous_count": oldCount,
		"message":        "All webhook history cleared",
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(response)

	log.Printf("🧹 Webhook history cleared (was %d webhooks)", oldCount)
}

func rootHandler(w http.ResponseWriter, r *http.Request) {
	html := `<!DOCTYPE html>
<html>
<head>
    <title>Fake Webhook Server</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 40px; background: #f5f5f5; }
        .container { max-width: 800px; margin: 0 auto; background: white; padding: 30px; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        h1 { color: #333; border-bottom: 2px solid #007acc; padding-bottom: 10px; }
        .status { background: #e8f4fd; padding: 15px; border-radius: 5px; margin: 20px 0; }
        .endpoint { background: #f8f9fa; padding: 10px; border-left: 4px solid #007acc; margin: 10px 0; font-family: monospace; }
        button { background: #007acc; color: white; border: none; padding: 10px 20px; border-radius: 4px; cursor: pointer; margin: 5px; }
        button:hover { background: #005fa3; }
        .webhooks { margin-top: 20px; }
        .webhook { background: #f8f9fa; padding: 10px; margin: 5px 0; border-radius: 4px; border-left: 4px solid #28a745; }
    </style>
    <script>
        function refreshStatus() {
            fetch('/status')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('status').innerHTML = 
                        '<strong>Webhooks Received:</strong> ' + data.webhooks_received + '<br>' +
                        '<strong>Uptime:</strong> ' + data.uptime;
                    loadWebhooks();
                });
        }
        
        function loadWebhooks() {
            fetch('/webhooks')
                .then(response => response.json())
                .then(data => {
                    const container = document.getElementById('webhooks');
                    if (data.webhooks.length === 0) {
                        container.innerHTML = '<p>No webhooks received yet.</p>';
                        return;
                    }
                    
                    container.innerHTML = data.webhooks.map(webhook => 
                        '<div class="webhook">' +
                        '<strong>Task:</strong> ' + webhook.taskId + ' | ' +
                        '<strong>State:</strong> ' + webhook.state + ' | ' +
                        '<strong>Time:</strong> ' + new Date(webhook.timestamp).toLocaleString() +
                        '</div>'
                    ).join('');
                });
        }
        
        function clearWebhooks() {
            fetch('/clear', { method: 'POST' })
                .then(response => response.json())
                .then(data => {
                    alert(data.message);
                    refreshStatus();
                });
        }
        
        setInterval(refreshStatus, 2000);  // Auto-refresh every 2 seconds
        window.onload = refreshStatus;
    </script>
</head>
<body>
    <div class="container">
        <h1>🪝 Fake Webhook Server</h1>
        
        <div class="status">
            <div id="status">Loading...</div>
        </div>
        
        <h2>📋 API Endpoints</h2>
        <div class="endpoint"><strong>POST</strong> http://localhost:9999/webhook - Receive webhooks</div>
        <div class="endpoint"><strong>GET</strong> http://localhost:9999/status - Server status</div>
        <div class="endpoint"><strong>GET</strong> http://localhost:9999/webhooks - List all received webhooks</div>
        <div class="endpoint"><strong>POST</strong> http://localhost:9999/clear - Clear webhook history</div>
        
        <button onclick="refreshStatus()">🔄 Refresh</button>
        <button onclick="clearWebhooks()">🧹 Clear History</button>
        
        <h2>📥 Recent Webhooks</h2>
        <div id="webhooks">Loading...</div>
    </div>
</body>
</html>`

	w.Header().Set("Content-Type", "text/html")
	fmt.Fprint(w, html)
}

var startTime = time.Now()

func main() {
	log.Printf("🚀 Starting Fake Webhook Server on port 9999...")
	log.Printf("📍 Webhook URL: http://localhost:9999/webhook")
	log.Printf("🌐 Web Interface: http://localhost:9999")

	http.HandleFunc("/", rootHandler)
	http.HandleFunc("/webhook", webhookHandler)
	http.HandleFunc("/status", statusHandler)
	http.HandleFunc("/webhooks", webhooksHandler)
	http.HandleFunc("/clear", clearHandler)

	log.Printf("✅ Fake webhook server ready!")
	log.Printf("💡 Use this URL in webhook_notifier_config.yaml:")
	log.Printf("   webhook:")
	log.Printf("     url: \"http://localhost:9999/webhook\"")

	if err := http.ListenAndServe(":9999", nil); err != nil {
		log.Fatalf("❌ Server failed: %v", err)
	}
}
