QT += widgets multimedia network
CONFIG += c++17 console
CONFIG -= app_bundle
TARGET = rtc_smoke
SOURCES += $$PWD/rtc_smoke.cpp
include($$PWD/../player/media_playback.pri)
include($$PWD/../broadcast/desktop_broadcast.pri)
include($$PWD/../rtc/camera_rtc.pri)
