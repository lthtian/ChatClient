QT += testlib widgets network multimedia
CONFIG += c++17 console
CONFIG -= app_bundle
TARGET = hls_playback_smoke
INCLUDEPATH += $$PWD/..
SOURCES += $$PWD/hls_playback_smoke.cpp $$PWD/../media_transfer.cpp $$PWD/../tcpclient.cpp
HEADERS += $$PWD/../media_transfer.h $$PWD/../tcpclient.h
include($$PWD/../player/media_playback.pri)
