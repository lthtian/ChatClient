QT += testlib widgets multimedia network
CONFIG += c++17 console
CONFIG -= app_bundle
TARGET = media_reinforcement
INCLUDEPATH += $$PWD/..
SOURCES += $$PWD/media_reinforcement.cpp
include($$PWD/../player/media_playback.pri)
