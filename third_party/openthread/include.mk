TARGET_OPENTHREAD_PATH = $(OPENTHREAD_MODULE_PATH)
CFLAGS += -DOPENTHREAD_PROJECT_CORE_CONFIG_FILE=\"openthread-config-project.h\"

INCLUDE_DIRS += $(TARGET_OPENTHREAD_PATH)/openthread/include
INCLUDE_DIRS += $(TARGET_OPENTHREAD_PATH)/openthread/src/core

INCLUDE_DIRS += $(TARGET_OPENTHREAD_PATH)/openthread/examples/platforms