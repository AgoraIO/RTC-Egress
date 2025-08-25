package uploader

import (
	"context"
	"log"
	"os"
	"path/filepath"
	"time"

	"github.com/AgoraIO/RTC-Egress/pkg/utils"
	"github.com/fsnotify/fsnotify"
)

// S3Config is now defined in types.go

type Watcher struct {
	outputDir string
	s3Config  S3Config
	uploader  *S3Uploader
}

func NewWatcher(outputDir string, s3Config S3Config) (*Watcher, error) {
	// Create output directory only if not in container AND it doesn't exist
	if !utils.IsRunningInContainer() {
		if _, err := os.Stat(outputDir); os.IsNotExist(err) {
			if err := os.MkdirAll(outputDir, 0755); err != nil {
				return nil, err
			}
		}
	}
	// In container mode, directory should already exist via volume mounts

	uploader, err := NewS3Uploader(s3Config)
	if err != nil {
		return nil, err
	}

	return &Watcher{
		outputDir: outputDir,
		s3Config:  s3Config,
		uploader:  uploader,
	}, nil
}

func (w *Watcher) Start(ctx context.Context) error {
	log.Printf("Initializing file system watcher for directory: %s", w.outputDir)

	watcher, err := fsnotify.NewWatcher()
	if err != nil {
		log.Printf("Failed to create file system watcher: %v", err)
		return err
	}
	defer watcher.Close()

	// Add the output directory to the watcher
	if err := watcher.Add(w.outputDir); err != nil {
		log.Printf("Failed to add directory %s to watcher: %v", w.outputDir, err)
		return err
	}

	log.Printf("Processing existing files in directory: %s", w.outputDir)
	// Process existing files in the directory
	if err := w.processExistingFiles(); err != nil {
		log.Printf("Error processing existing files: %v", err)
	}

	log.Println("Starting to watch for file system events...")
	for {
		select {
		case event, ok := <-watcher.Events:
			if !ok {
				log.Println("File system events channel closed")
				return nil
			}

			log.Printf("File system event: %s", event)

			// Only process create/write events
			if event.Op&fsnotify.Create == fsnotify.Create || event.Op&fsnotify.Write == fsnotify.Write {
				// Skip directories
				info, err := os.Stat(event.Name)
				if err != nil {
					log.Printf("Error getting file info for %s: %v", event.Name, err)
					continue
				}

				if info.IsDir() {
					log.Printf("Skipping directory: %s", event.Name)
					continue
				}

				log.Printf("New/updated file detected: %s (size: %d bytes)", event.Name, info.Size())
				// Small delay to ensure the file is fully written
				time.Sleep(100 * time.Millisecond)
				log.Printf("Attempting to upload file: %s", event.Name)
				if err := w.uploader.UploadFile(event.Name); err != nil {
					log.Printf("Failed to upload file %s: %v", event.Name, err)
				} else {
					log.Printf("Successfully uploaded file: %s", event.Name)
				}
			}

		case err, ok := <-watcher.Errors:
			if !ok {
				log.Println("File system errors channel closed")
				return nil
			}
			log.Printf("File system watcher error: %v", err)

		case <-ctx.Done():
			log.Println("Context cancelled, stopping file system watcher")
			return nil
		}
	}
}

func (w *Watcher) processExistingFiles() error {
	entries, err := os.ReadDir(w.outputDir)
	if err != nil {
		return err
	}

	for _, entry := range entries {
		if !entry.IsDir() {
			filePath := filepath.Join(w.outputDir, entry.Name())
			if err := w.uploader.UploadFile(filePath); err != nil {
				log.Printf("Failed to upload existing file %s: %v", filePath, err)
			}
		}
	}
	return nil
}
