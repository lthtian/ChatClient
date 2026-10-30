QT += testlib widgets multimedia network
CONFIG += c++17 console
CONFIG -= app_bundle
TARGET = media_tracks_test
INCLUDEPATH += $$PWD/..
SOURCES += $$PWD/media_tracks_test.cpp
include($$PWD/../player/media_playback.pri)
