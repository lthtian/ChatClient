QT += testlib widgets network multimedia
CONFIG += c++17 console
CONFIG -= app_bundle
TARGET = video_chat_smoke
INCLUDEPATH += $$PWD/..
SOURCES += $$PWD/video_chat_smoke.cpp \
    $$PWD/../mainwindow.cpp $$PWD/../media_chat.cpp $$PWD/../media_transfer.cpp \
    $$PWD/../tcpclient.cpp $$PWD/../mylistwidget.cpp $$PWD/../qnchatmessage.cpp
HEADERS += $$PWD/../mainwindow.h $$PWD/../media_transfer.h $$PWD/../tcpclient.h \
    $$PWD/../mylistwidget.h $$PWD/../mytextedit.h $$PWD/../qnchatmessage.h
RESOURCES += $$PWD/../resourse.qrc
include($$PWD/../player/media_playback.pri)
