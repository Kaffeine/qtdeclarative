TEMPLATE = subdirs
CONFIG += ordered
include($$OUT_PWD/qml/qtqml-config.pri)
include($$OUT_PWD/quick/qtquick-config.pri)
QT_FOR_CONFIG += qml quick-private
SUBDIRS += \
    qml

qtHaveModule(gui):qtConfig(animation) {
    SUBDIRS += \
        quick \
        qmltest

    qtConfig(quick-particles): \
        SUBDIRS += particles
    qtHaveModule(widgets): SUBDIRS += quickwidgets
}

SUBDIRS += \
    plugins \
    imports \
    qmldevtools

qmldevtools.depends = qml

qtConfig(qml-network) {
    QT_FOR_CONFIG += network
    qtConfig(localserver):qtConfig(qml-debug): SUBDIRS += qmldebug
}
