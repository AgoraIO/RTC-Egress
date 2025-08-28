package uploader

// S3Config holds the configuration for connecting to an S3-compatible storage service
type S3Config struct {
	// Bucket is the name of the S3 bucket to upload files to
	Bucket string `yaml:"bucket" mapstructure:"bucket"`
	// Region is the AWS region where the bucket is located
	Region string `yaml:"region" mapstructure:"region"`
	// AccessKey is the AWS access key ID
	AccessKey string `yaml:"access_key" mapstructure:"access_key"`
	// SecretKey is the AWS secret access key
	SecretKey string `yaml:"secret_key" mapstructure:"secret_key"`
	// Endpoint is the custom endpoint URL for S3-compatible storage (e.g., Cloudflare R2, MinIO)
	Endpoint string `yaml:"endpoint" mapstructure:"endpoint"`
}

// Config holds the configuration for the uploader package
type Config struct {
	// S3 contains the S3-specific configuration
	S3 S3Config `yaml:"s3"`
}
