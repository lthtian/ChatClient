QT += testlib widgets multimedia network
CONFIG += c++17 console
CONFIG -= app_bundle
TARGET = media_pipeline_smoke
SOURCES += $$PWD/media_pipeline_smoke.cpp $$PWD/../rtc/media_codec.cpp
include($$PWD/../player/media_playback.pri)
INCLUDEPATH += $$PWD/../rtc
