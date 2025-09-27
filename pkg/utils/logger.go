package utils

import (
	"log"

	"github.com/gin-gonic/gin"
)

// MyCustomLogger logs completed requests while skipping successful health probes.
func MyCustomLogger() gin.HandlerFunc {
	return func(c *gin.Context) {
		c.Next()

		path := c.Request.URL.Path
		status := c.Writer.Status()

		if path == "/health" && status == 200 {
			return
		}

		log.Printf("[GIN] %d | %s | %s", status, c.Request.Method, path)
	}
}
