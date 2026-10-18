QT += testlib widgets network multimedia
CONFIG += c++17 console
CONFIG -= app_bundle
TARGET = live_stability_smoke
INCLUDEPATH += $$PWD/..
SOURCES += $$PWD/live_stability_smoke.cpp $$PWD/../media_transfer.cpp $$PWD/../live_transfer.cpp $$PWD/../tcpclient.cpp
HEADERS += $$PWD/../media_transfer.h $$PWD/../tcpclient.h
include($$PWD/../player/media_playback.pri)
include($$PWD/../broadcast/desktop_broadcast.pri)
