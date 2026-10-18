QT += testlib widgets network multimedia
CONFIG += c++17 console
CONFIG -= app_bundle
TARGET = live_playback_smoke
INCLUDEPATH += $$PWD/..
SOURCES += $$PWD/live_playback_smoke.cpp $$PWD/../media_transfer.cpp $$PWD/../live_transfer.cpp $$PWD/../tcpclient.cpp \
    $$PWD/../mainwindow.cpp $$PWD/../media_chat.cpp $$PWD/../live_chat.cpp $$PWD/../mylistwidget.cpp $$PWD/../qnchatmessage.cpp
HEADERS += $$PWD/../media_transfer.h $$PWD/../tcpclient.h $$PWD/../mainwindow.h \
    $$PWD/../mylistwidget.h $$PWD/../mytextedit.h $$PWD/../qnchatmessage.h
RESOURCES += $$PWD/../resourse.qrc
include($$PWD/../player/media_playback.pri)
include($$PWD/../broadcast/desktop_broadcast.pri)
