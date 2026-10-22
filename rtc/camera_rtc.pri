INCLUDEPATH += $$PWD
isEmpty(RTC_ROOT): RTC_ROOT = $$(RTC_ROOT)
isEmpty(RTC_ROOT): RTC_ROOT = D:/chat/_deps/rtc
!exists($$RTC_ROOT/datachannel-build/libdatachannel-static.a): error(Build the RTC dependencies described in rtc/README.md first.)
INCLUDEPATH += $$RTC_ROOT/libdatachannel/include
DEFINES += RTC_STATIC RTC_ENABLE_MEDIA=1 RTC_ENABLE_WEBSOCKET=0
LIBS += $$RTC_ROOT/datachannel-build/libdatachannel-static.a \
    $$RTC_ROOT/datachannel-build/deps/usrsctp/usrsctplib/libusrsctp.a \
    $$RTC_ROOT/datachannel-build/deps/libsrtp/libsrtp2.a \
    $$RTC_ROOT/datachannel-build/deps/libjuice/libjuice-static.a \
    $$RTC_ROOT/install/lib/libmbedtls.a $$RTC_ROOT/install/lib/libmbedx509.a $$RTC_ROOT/install/lib/libmbedcrypto.a \
    -lws2_32 -liphlpapi -lbcrypt
SOURCES += $$PWD/media_codec.cpp $$PWD/rtc_connection.cpp $$PWD/camera_window.cpp $$PWD/rtp.cpp
HEADERS += $$PWD/media_codec.h $$PWD/rtc_connection.h $$PWD/camera_window.h $$PWD/rtp.h $$PWD/wire.h
SOURCES += $$PWD/microphone_capture.cpp
HEADERS += $$PWD/microphone_capture.h
