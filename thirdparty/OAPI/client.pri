QT += network

HEADERS += \
# Models
    $${PWD}/OAIBittiming.h \
    $${PWD}/OAIFilter.h \
    $${PWD}/OAIFrame.h \
    $${PWD}/OAIPhy.h \
    $${PWD}/OAIStatus.h \
    $${PWD}/OAITransmit_sequence.h \
    $${PWD}/OAI_api_info_get_200_response.h \
# APIs
    $${PWD}/OAIDefaultApi.h \
# Others
    $${PWD}/OAIHelpers.h \
    $${PWD}/OAIHttpRequest.h \
    $${PWD}/OAIObject.h \
    $${PWD}/OAIEnum.h \
    $${PWD}/OAIHttpFileElement.h \
    $${PWD}/OAIServerConfiguration.h \
    $${PWD}/OAIServerVariable.h \
    $${PWD}/OAIOauth.h

SOURCES += \
# Models
    $${PWD}/OAIBittiming.cpp \
    $${PWD}/OAIFilter.cpp \
    $${PWD}/OAIFrame.cpp \
    $${PWD}/OAIPhy.cpp \
    $${PWD}/OAIStatus.cpp \
    $${PWD}/OAITransmit_sequence.cpp \
    $${PWD}/OAI_api_info_get_200_response.cpp \
# APIs
    $${PWD}/OAIDefaultApi.cpp \
# Others
    $${PWD}/OAIHelpers.cpp \
    $${PWD}/OAIHttpRequest.cpp \
    $${PWD}/OAIHttpFileElement.cpp \
    $${PWD}/OAIOauth.cpp
