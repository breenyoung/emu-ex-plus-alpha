ifndef inc_input_evdev
inc_input_evdev := 1

include $(imagineSrcDir)/input/build.mk

configDefs += CONFIG_INPUT_EVDEV

SRC += input/evdev/evdev.cc

endif
