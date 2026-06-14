LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := openxr_loader
LOCAL_SRC_FILES := $(LOCAL_PATH)/../build/generated/openxr-loader/jniLibs/$(TARGET_ARCH_ABI)/libopenxr_loader.so
include $(PREBUILT_SHARED_LIBRARY)

include $(GL4ES_PATH)/Android.mk
include $(TOP_DIR)/rtcw/Android.mk
