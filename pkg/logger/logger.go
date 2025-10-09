package logger

import (
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/gin-gonic/gin"
)

// Field represents a key/value pair attached to a log entry.
type Field struct {
	key   string
	value interface{}
}

var (
	serviceName   = "unknown"
	defaultFields []Field
	output        io.Writer = os.Stdout
	mu            sync.RWMutex
)

// Init configures the standard library logger with a shared format.
// It should be called once from each service entrypoint.
func Init(service string, fields ...Field) {
	mu.Lock()
	defer mu.Unlock()

	serviceName = service
	defaultFields = append([]Field(nil), fields...)
	log.SetFlags(0)
	log.SetOutput(output)
	log.SetPrefix("")
}

// SetOutput allows tests to inject a different writer.
func SetOutput(w io.Writer) {
	mu.Lock()
	defer mu.Unlock()
	output = w
	log.SetOutput(output)
}

// With adds a key/value pair to the default fields shared across log lines.
func With(field Field) {
	mu.Lock()
	defer mu.Unlock()
	if field.key == "" {
		return
	}
	for i := range defaultFields {
		if defaultFields[i].key == field.key {
			defaultFields[i] = field
			return
		}
	}
	defaultFields = append(defaultFields, field)
}

// Info logs a message with optional structured fields.
func Info(msg string, fields ...Field) {
	logLine("INFO", msg, fields)
}

// Warn logs a warning message.
func Warn(msg string, fields ...Field) {
	logLine("WARN", msg, fields)
}

// Error logs an error message.
func Error(msg string, fields ...Field) {
	logLine("ERROR", msg, fields)
}

// Debug logs a debug message.
func Debug(msg string, fields ...Field) {
	logLine("DEBUG", msg, fields)
}

// Fatal logs a fatal message and exits the process.
func Fatal(msg string, fields ...Field) {
	logLine("FATAL", msg, fields)
	os.Exit(1)
}

func logLine(level string, msg string, fields []Field) {
	mu.RLock()
	def := append([]Field(nil), defaultFields...)
	mu.RUnlock()

	all := make([]Field, 0, len(def)+len(fields))
	all = append(all, def...)
	all = append(all, fields...)

	unique := dedupeFields(all)
	fieldStr := formatFields(unique)

	timestamp := time.Now().Format("2006/01/02 15:04:05.000")
	line := fmt.Sprintf("[%s][%s]%s", timestamp, level, msg)
	if fieldStr != "" {
		line += "(" + strings.TrimPrefix(fieldStr, " ") + ")"
	}

	log.Print(line)
}

func formatFields(fields []Field) string {
	if len(fields) == 0 {
		return ""
	}

	var b strings.Builder
	for _, field := range fields {
		if field.key == "" {
			continue
		}
		b.WriteByte(' ')
		b.WriteString(field.key)
		b.WriteByte('=')
		b.WriteString(valueToString(field.value))
	}
	return b.String()
}

func dedupeFields(fields []Field) []Field {
	result := make([]Field, 0, len(fields))
	index := make(map[string]int)

	for _, field := range fields {
		if field.key == "" {
			continue
		}
		if idx, ok := index[field.key]; ok {
			result[idx] = field
			continue
		}
		index[field.key] = len(result)
		result = append(result, field)
	}
	return result
}

func quoteIfNeeded(msg string) string {
	if msg == "" {
		return `""`
	}
	if strings.ContainsAny(msg, " \t\"") {
		return fmt.Sprintf("%q", msg)
	}
	return msg
}

func valueToString(v interface{}) string {
	switch val := v.(type) {
	case string:
		return quoteIfNeeded(val)
	case fmt.Stringer:
		return quoteIfNeeded(val.String())
	case error:
		return quoteIfNeeded(val.Error())
	case time.Duration:
		return val.String()
	default:
		return fmt.Sprint(val)
	}
}

// String creates a string field.
func String(key string, value string) Field {
	return Field{key: key, value: value}
}

// Int creates an integer field.
func Int(key string, value int) Field {
	return Field{key: key, value: value}
}

// Bool creates a boolean field.
func Bool(key string, value bool) Field {
	return Field{key: key, value: value}
}

// Duration creates a duration field.
func Duration(key string, value time.Duration) Field {
	return Field{key: key, value: value}
}

// ErrorField creates an error field.
func ErrorField(err error) Field {
	if err == nil {
		return Field{}
	}
	return Field{key: "error", value: err}
}

// GinRequestLogger logs completed requests while skipping successful health probes.
func GinRequestLogger() gin.HandlerFunc {
	return func(c *gin.Context) {
		c.Next()

		path := c.Request.URL.Path
		status := c.Writer.Status()

		if path == "/health" && status == http.StatusOK {
			return
		}

		logLine("INFO", fmt.Sprintf("[GIN] %d | %s | %s", status, c.Request.Method, path), nil)
	}
}
