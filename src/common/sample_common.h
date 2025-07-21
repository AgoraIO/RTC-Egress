
//  Agora RTC/MEDIA SDK
//
//  Created by Jay Zhang in 2020-06.
//  Copyright (c) 2020 Agora.io. All rights reserved.
//

#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "IAgoraService.h"

agora::base::IAgoraService* createAndInitAgoraService(bool enableAudioDevice,
                                                      bool enableAudioProcessor, bool enableVideo,bool enableuseStringUid = false,bool enablelowDelay = false, const char* appid =nullptr);

int verifyLicense();

static inline std::string to_string(int val) {
  char str[32] = {0};
  snprintf(str, sizeof(str), "%d", val);
  return std::string(str); 
}

int32_t getLocalIP(agora::agora_refptr<agora::rtc::IRtcConnection>& connection, std::string& ip);

int32_t setLocalIP(agora::agora_refptr<agora::rtc::IRtcConnection>& connection, const std::string& ip);
