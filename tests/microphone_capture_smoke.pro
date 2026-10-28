QT += core gui multimedia network widgets
CONFIG += c++17 console
CONFIG -= app_bundle
TARGET = microphone_capture_smoke
isEmpty(FFMPEG_ROOT): FFMPEG_ROOT = D:/chat/_deps/ffmpeg-8.1.1-full_build-shared
INCLUDEPATH += $$PWD/../broadcast $$PWD/../rtc $$PWD/../player $$FFMPEG_ROOT/include
SOURCES += $$PWD/microphone_capture_smoke.cpp $$PWD/../broadcast/audio_capture.cpp \
    $$PWD/../rtc/microphone_capture.cpp $$PWD/../rtc/media_codec.cpp $$PWD/../player/track_decoder.cpp
HEADERS += $$PWD/../rtc/microphone_capture.h
LIBS += -L$$FFMPEG_ROOT/lib -lavcodec -lavutil -lswresample -lswscale -lole32 -luuid -lksuser
