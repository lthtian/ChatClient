INCLUDEPATH += $$PWD
LIBS += -lole32 -luuid -lksuser
SOURCES += $$PWD/audio_capture.cpp $$PWD/stream_encoder.cpp $$PWD/desktop_publisher.cpp $$PWD/desktop_broadcast.cpp
HEADERS += $$PWD/audio_capture.h $$PWD/stream_encoder.h $$PWD/desktop_publisher.h $$PWD/desktop_broadcast.h
