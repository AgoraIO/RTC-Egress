PROJECT_NAME := rtc-sdk-rtmp-cdn-demo
PROJECT_VERSION ?= "0.1."$(shell date -u +'%Y%m%d%H')
REGISTRY ?= hub.agoralab.co/adc/

.PHONY: build clean docker-build docker-push docker-run play run

build: clean
	@echo ">> build"
	@mkdir build && cd build && cmake ../src && make -j8
	@echo ">> done"

clean:
	@echo ">> clean"
	@rm -rf build
	@rm -rf out
	@echo ">> done"

docker-build:
	@echo ">> docker build"
	@echo "REGISTRY:" $(REGISTRY)
	@echo "PROJECT_NAME:" $(PROJECT_NAME)
	@echo "PROJECT_VERSION:" $(PROJECT_VERSION)
	@docker build -t $(REGISTRY)$(PROJECT_NAME):$(PROJECT_VERSION) -f Dockerfile_app .
	@echo ">> done"

docker-push:
	@echo ">> docker push"
	@docker push $(REGISTRY)$(PROJECT_NAME):$(PROJECT_VERSION)
	@echo ">> done"

docker-run:
	@echo ">> docker run"
	@docker run --rm -it -e TOKEN=aab8b8f5a8cd4469a63042fcfafe7063 -e CHANNEL_ID=test -e REMOTE_USER_ID=123 -e RTMP_URL=rtmp://examplepush.agoramdn.com/live/test --name $(PROJECT_NAME) $(REGISTRY)$(PROJECT_NAME):$(PROJECT_VERSION)

play:
	@echo ">> play"
	@ffplay rtmp://examplepull.agoramdn.com/live/test
	@echo ">> done"

run:
	@echo ">> run"
	@cd out && export LD_LIBRARY_PATH=../agora_sdk && ./rtmp_cdn_demo --token aab8b8f5a8cd4469a63042fcfafe7063 --channelId test --remoteUserId 123 --rtmpUrl rtmp://examplepush.agoramdn.com/live/test
	@echo ">> done"
