QT += widgets multimedia network
CONFIG += c++17

!equals(QT_MAJOR_VERSION, 5): error(This player requires Qt 5 Multimedia APIs.)
isEmpty(FFMPEG_ROOT): FFMPEG_ROOT = $$(FFMPEG_ROOT)
isEmpty(FFMPEG_ROOT): FFMPEG_ROOT = D:/chat/_deps/ffmpeg-8.1.1-full_build-shared
!exists($$FFMPEG_ROOT/include/libavformat/avformat.h): error(FFMPEG_ROOT must point to the shared FFmpeg development package.)

INCLUDEPATH += $$PWD $$FFMPEG_ROOT/include
LIBS += -L$$FFMPEG_ROOT/lib -lavformat -lavcodec -lavutil -lswscale -lswresample
SOURCES += $$PWD/media_decoder.cpp $$PWD/audio_output.cpp $$PWD/media_window.cpp $$PWD/video_view.cpp $$PWD/video_geometry.cpp
HEADERS += $$PWD/media_source.h $$PWD/media_decoder.h $$PWD/audio_output.h $$PWD/media_window.h $$PWD/video_view.h $$PWD/video_geometry.h

SOURCES += $$PWD/track_decoder.cpp $$PWD/playback.cpp
HEADERS += $$PWD/media_types.h $$PWD/track_decoder.h $$PWD/playback.h
