package uploader

import (
	"fmt"
	"log"
	"math"
	"os"
	"path/filepath"

	"github.com/aws/aws-sdk-go/aws"
	"github.com/aws/aws-sdk-go/aws/credentials"
	"github.com/aws/aws-sdk-go/aws/session"
	"github.com/aws/aws-sdk-go/service/s3"
	"github.com/aws/aws-sdk-go/service/s3/s3manager"
)

type S3Uploader struct {
	bucket   string
	region   string
	endpoint string
	client   *s3manager.Uploader
}

func NewS3Uploader(config S3Config) (*S3Uploader, error) {
	if config.Bucket == "" {
		return nil, fmt.Errorf("S3 bucket not configured")
	}

	// Log configuration details (without sensitive data)
	log.Printf("Initializing S3 uploader with bucket: %s, region: %s, endpoint: %s",
		config.Bucket, config.Region, config.Endpoint)

	// Create credentials object
	creds := credentials.NewStaticCredentials(
		config.AccessKey,
		config.SecretKey,
		"",
	)

	// Verify credentials are valid
	_, err := creds.Get()
	if err != nil {
		return nil, fmt.Errorf("invalid AWS credentials: %v", err)
	}

	// Create AWS config with explicit credentials
	awsConfig := &aws.Config{
		Region:           aws.String(config.Region),
		Credentials:      creds,
		S3ForcePathStyle: aws.Bool(true),
	}

	// Set custom endpoint if provided
	if config.Endpoint != "" {
		log.Printf("Using custom endpoint: %s", config.Endpoint)
		awsConfig.Endpoint = aws.String(config.Endpoint)
	}

	// Create a new session with the config
	sess := session.Must(session.NewSession(awsConfig))

	// Verify the session is valid
	if _, err := sess.Config.Credentials.Get(); err != nil {
		return nil, fmt.Errorf("failed to verify AWS credentials: %v", err)
	}

	// Create a new S3 client
	s3Svc := s3.New(sess)

	// Create a new uploader with the S3 client
	uploader := s3manager.NewUploaderWithClient(s3Svc)

	log.Printf("Successfully initialized S3 uploader for bucket: %s", config.Bucket)

	return &S3Uploader{
		bucket:   config.Bucket,
		region:   config.Region,
		endpoint: config.Endpoint,
		client:   uploader,
	}, nil
}

// min returns the smaller of x or y
func min(x, y int) int {
	return int(math.Min(float64(x), float64(y)))
}

func (u *S3Uploader) UploadFile(filePath string) error {
	log.Printf("Preparing to upload file: %s", filePath)

	// Open the file for use
	file, err := os.Open(filePath)
	if err != nil {
		return fmt.Errorf("failed to open file %s: %v", filePath, err)
	}
	defer file.Close()

	// Get file info to get the file size
	fileInfo, err := file.Stat()
	if err != nil {
		return fmt.Errorf("failed to get file info %s: %v", filePath, err)
	}

	// Get just the filename for the S3 key
	key := filepath.Base(filePath)

	log.Printf("Uploading to S3 - Bucket: %s, Key: %s, Size: %d bytes", u.bucket, key, fileInfo.Size())
	log.Printf("S3 Configuration - Region: %s, Endpoint: %s", u.region, u.endpoint)

	// Upload the file to S3 using the pre-configured uploader
	result, err := u.client.Upload(&s3manager.UploadInput{
		Bucket: aws.String(u.bucket),
		Key:    aws.String(key),
		Body:   file,
	})

	if err != nil {
		errMsg := fmt.Errorf("S3 upload failed: %v", err)
		log.Printf("Error: %v", errMsg)
		return errMsg
	}

	log.Printf("Successfully uploaded file to %s", result.Location)
	return nil
}
