/****************************************************************************
**
** Copyright (C) 2012 Nokia Corporation and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/
**
** This file is part of the QtGui module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** GNU Lesser General Public License Usage
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this
** file. Please review the following information to ensure the GNU Lesser
** General Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU General
** Public License version 3.0 as published by the Free Software Foundation
** and appearing in the file LICENSE.GPL included in the packaging of this
** file. Please review the following information to ensure the GNU General
** Public License version 3.0 requirements will be met:
** http://www.gnu.org/copyleft/gpl.html.
**
** Other Usage
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Nokia.
**
**
**
**
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qguiapplication.h"

#include "private/qguiapplication_p.h"
#include "private/qplatformintegrationfactory_qpa_p.h"
#include "private/qevent_p.h"
#include "qfont.h"
#include "qplatformfontdatabase_qpa.h"
#include "qplatformwindow_qpa.h"
#include "qplatformnativeinterface_qpa.h"
#include "qplatformtheme_qpa.h"
#include "qplatformintegration_qpa.h"

#include <QtCore/QAbstractEventDispatcher>
#include <QtCore/QVariant>
#include <QtCore/private/qcoreapplication_p.h>
#include <QtCore/private/qabstracteventdispatcher_p.h>
#include <QtCore/qmutex.h>
#include <QtCore/private/qthread_p.h>
#include <QtCore/qdir.h>
#include <QtDebug>
#include <qpalette.h>
#include <qscreen.h>
#include <private/qscreen_p.h>
#include <private/qdrawhelper_p.h>

#include <QtGui/QPlatformIntegration>
#include <QtGui/QGenericPluginFactory>
#include <QtGui/qstylehints.h>
#include <QtGui/qinputpanel.h>
#include <QtGui/qplatformtheme_qpa.h>
#include <QtGui/qplatforminputcontext_qpa.h>
#include <private/qplatforminputcontext_qpa_p.h>

#include <QWindowSystemInterface>
#include "private/qwindowsysteminterface_qpa_p.h"
#include "private/qwindow_p.h"
#include "private/qcursor_p.h"

#include "private/qdnd_p.h"
#include <private/qplatformthemefactory_qpa_p.h>
#include "qplatformdrag_qpa.h"

#ifndef QT_NO_CURSOR
#include "qplatformcursor_qpa.h"
#endif

#include <QtGui/QPixmap>

#ifndef QT_NO_CLIPBOARD
#include <QtGui/QClipboard>
#endif

QT_BEGIN_NAMESPACE

Q_GUI_EXPORT bool qt_is_gui_used = true;

Qt::MouseButtons QGuiApplicationPrivate::mouse_buttons = Qt::NoButton;
Qt::KeyboardModifiers QGuiApplicationPrivate::modifier_buttons = Qt::NoModifier;

QPointF QGuiApplicationPrivate::lastCursorPosition(0.0, 0.0);

QPlatformIntegration *QGuiApplicationPrivate::platform_integration = 0;
QPlatformTheme *QGuiApplicationPrivate::platform_theme = 0;

QList<QObject *> QGuiApplicationPrivate::generic_plugin_list;

enum ApplicationResourceFlags
{
    ApplicationPaletteExplicitlySet = 0x1,
    ApplicationFontExplicitlySet = 0x2
};

static unsigned applicationResourceFlags = 0;

QString *QGuiApplicationPrivate::platform_name = 0;
bool QGuiApplicationPrivate::app_do_modal = false;

QPalette *QGuiApplicationPrivate::app_pal = 0;        // default application palette

Qt::MouseButtons QGuiApplicationPrivate::buttons = Qt::NoButton;
ulong QGuiApplicationPrivate::mousePressTime = 0;
Qt::MouseButton QGuiApplicationPrivate::mousePressButton = Qt::NoButton;
int QGuiApplicationPrivate::mousePressX = 0;
int QGuiApplicationPrivate::mousePressY = 0;
int QGuiApplicationPrivate::mouse_double_click_distance = 5;

static Qt::LayoutDirection layout_direction = Qt::LeftToRight;
static bool force_reverse = false;

QGuiApplicationPrivate *QGuiApplicationPrivate::self = 0;
QTouchDevice *QGuiApplicationPrivate::m_fakeTouchDevice = 0;
int QGuiApplicationPrivate::m_fakeMouseSourcePointId = 0;

#ifndef QT_NO_CLIPBOARD
QClipboard *QGuiApplicationPrivate::qt_clipboard = 0;
#endif

QList<QScreen *> QGuiApplicationPrivate::screen_list;

QWindowList QGuiApplicationPrivate::window_list;
QWindow *QGuiApplicationPrivate::focus_window = 0;

static QBasicMutex applicationFontMutex;
QFont *QGuiApplicationPrivate::app_font = 0;
bool QGuiApplicationPrivate::obey_desktop_settings = true;

static qreal fontSmoothingGamma = 1.7;

extern void qRegisterGuiVariant();
extern void qUnregisterGuiVariant();
extern void qInitDrawhelperAsm();
extern void qInitImageConversions();

static bool qt_detectRTLLanguage()
{
    return force_reverse ^
        (QCoreApplication::tr("QT_LAYOUT_DIRECTION",
                         "Translate this string to the string 'LTR' in left-to-right"
                         " languages or to 'RTL' in right-to-left languages (such as Hebrew"
                         " and Arabic) to get proper widget layout.") == QLatin1String("RTL"));
}

static void initPalette()
{
    if (!QGuiApplicationPrivate::app_pal)
        if (const QPalette *themePalette = QGuiApplicationPrivate::platformTheme()->palette())
            QGuiApplicationPrivate::app_pal = new QPalette(*themePalette);
    if (!QGuiApplicationPrivate::app_pal)
        QGuiApplicationPrivate::app_pal = new QPalette(Qt::black);
}

static inline void clearPalette()
{
    delete QGuiApplicationPrivate::app_pal;
    QGuiApplicationPrivate::app_pal = 0;
}

static void initFontUnlocked()
{
    if (!QGuiApplicationPrivate::app_font) {
        if (const QPlatformTheme *theme = QGuiApplicationPrivate::platformTheme())
            if (const QFont *font = theme->font(QPlatformTheme::SystemFont))
                QGuiApplicationPrivate::app_font = new QFont(*font);
    }
    if (!QGuiApplicationPrivate::app_font)
        QGuiApplicationPrivate::app_font =
            new QFont(QGuiApplicationPrivate::platformIntegration()->fontDatabase()->defaultFont());
}

static inline void clearFontUnlocked()
{
    delete QGuiApplicationPrivate::app_font;
    QGuiApplicationPrivate::app_font = 0;
}

/*!
    \class QGuiApplication
    \brief The QGuiApplication class manages the GUI application's control
    flow and main settings.

    \inmodule QtGui
    \since 5.0

    QGuiApplication contains the main event loop, where all events from the window
    system and other sources are processed and dispatched. It also handles the
    application's initialization and finalization. In addition, QGuiApplication handles
    most of the system-wide and application-wide settings.

    For any GUI application using Qt, there is precisely \b one QGuiApplication
    object no matter whether the application has 0, 1, 2 or more windows at
    any given time. For non-GUI Qt applications, use QCoreApplication instead,
    as it does not depend on the \l QtGui library.

    The QGuiApplication object is accessible through the instance() function, which
    returns a pointer equivalent to the global \l qApp pointer.

    QGuiApplication's main areas of responsibility are:
        \list
            \li  It initializes the application with the user's desktop settings,
                such as palette(), font() and styleHints(). It keeps
                track of these properties in case the user changes the desktop
                globally, for example, through some kind of control panel.

            \li  It performs event handling, meaning that it receives events
                from the underlying window system and dispatches them to the
                relevant widgets. You can send your own events to windows by
                using sendEvent() and postEvent().

            \li  It parses common command line arguments and sets its internal
                state accordingly. See the \l{QGuiApplication::QGuiApplication()}
                {constructor documentation} below for more details.

            \li  It provides localization of strings that are visible to the
                user via translate().

            \li  It provides some magical objects like the clipboard().

            \li  It knows about the application's windows. You can ask which
                window is at a certain position using topLevelAt(), get a list of
                topLevelWindows(), etc.

            \li  It manages the application's mouse cursor handling, see
                setOverrideCursor()
        \endlist

    Since the QGuiApplication object does so much initialization, it \e{must} be
    created before any other objects related to the user interface are created.
    QGuiApplication also deals with common command line arguments. Hence, it is
    usually a good idea to create it \e before any interpretation or
    modification of \c argv is done in the application itself.

    \table
    \header
        \li{2,1} Groups of functions

        \row
        \li  System settings
        \li  desktopSettingsAware(),
            setDesktopSettingsAware(),
            styleHints(),
            palette(),
            setPalette(),
            font(),
            setFont().

        \row
        \li  Event handling
        \li  exec(),
            processEvents(),
            exit(),
            quit().
            sendEvent(),
            postEvent(),
            sendPostedEvents(),
            removePostedEvents(),
            hasPendingEvents(),
            notify().

        \row
        \li  Windows
        \li  allWindows(),
            topLevelWindows(),
            focusWindow(),
            clipboard(),
            topLevelAt().

        \row
        \li  Advanced cursor handling
        \li  overrideCursor(),
            setOverrideCursor(),
            restoreOverrideCursor().

        \row
        \li  Miscellaneous
        \li  startingUp(),
            closingDown(),
            type().
    \endtable

    \sa QCoreApplication, QAbstractEventDispatcher, QEventLoop
*/

/*!
    Initializes the window system and constructs an application object with
    \a argc command line arguments in \a argv.

    \warning The data referred to by \a argc and \a argv must stay valid for
    the entire lifetime of the QGuiApplication object. In addition, \a argc must
    be greater than zero and \a argv must contain at least one valid character
    string.

    The global \c qApp pointer refers to this application object. Only one
    application object should be created.

    This application object must be constructed before any \l{QPaintDevice}
    {paint devices} (including pixmaps, bitmaps etc.).

    \note \a argc and \a argv might be changed as Qt removes command line
    arguments that it recognizes.

    All Qt programs automatically support the following command line options:
    \list
        \li  -reverse, sets the application's layout direction to
            Qt::RightToLeft
        \li  -qmljsdebugger=, activates the QML/JS debugger with a specified port.
            The value must be of format port:1234[,block], where block is optional
            and will make the application wait until a debugger connects to it.
    \endlist

    \sa arguments()
*/
QGuiApplication::QGuiApplication(int &argc, char **argv, int flags)
    : QCoreApplication(*new QGuiApplicationPrivate(argc, argv, flags))
{
    d_func()->init();

    QCoreApplicationPrivate::eventDispatcher->startingUp();
}

QGuiApplication::QGuiApplication(QGuiApplicationPrivate &p)
    : QCoreApplication(p)
{
    d_func()->init(); }

/*!
    Destructs the application.
*/
QGuiApplication::~QGuiApplication()
{
    Q_D(QGuiApplication);
    // flush clipboard contents
    if (QGuiApplicationPrivate::qt_clipboard) {
        QEvent event(QEvent::Clipboard);
        QGuiApplication::sendEvent(QGuiApplicationPrivate::qt_clipboard, &event);
    }

    d->eventDispatcher->closingDown();
    d->eventDispatcher = 0;

    delete QGuiApplicationPrivate::qt_clipboard;
    QGuiApplicationPrivate::qt_clipboard = 0;

    clearPalette();

    qUnregisterGuiVariant();

#ifndef QT_NO_CURSOR
    d->cursor_list.clear();
#endif

    delete QGuiApplicationPrivate::platform_name;
    QGuiApplicationPrivate::platform_name = 0;
}

QGuiApplicationPrivate::QGuiApplicationPrivate(int &argc, char **argv, int flags)
    : QCoreApplicationPrivate(argc, argv, flags),
      styleHints(0),
      inputMethod(0),
      lastTouchType(QEvent::TouchEnd)
{
    self = this;
    application_type = QCoreApplication::GuiClient;
}

/*!
    Returns the QWindow that receives events tied to focus,
    such as key events.
*/
QWindow *QGuiApplication::focusWindow()
{
    return QGuiApplicationPrivate::focus_window;
}

/*!
    \fn QGuiApplication::focusObjectChanged(QObject *focusObject)

    This signal is emitted when final receiver of events tied to focus is changed.
    \sa focusObject()
*/

/*!
    Returns the QObject in currently active window that will be final receiver of events
    tied to focus, such as key events.
 */
QObject *QGuiApplication::focusObject()
{
    if (focusWindow())
        return focusWindow()->focusObject();
    return 0;
}

/*!
    \fn QGuiApplication::allWindows()

    Returns a list of all the windows in the application.

    The list is empty if there are no windows.

    \sa topLevelWindows()
 */
QWindowList QGuiApplication::allWindows()
{
    return QGuiApplicationPrivate::window_list;
}

/*!
    \fn QGuiApplication::topLevelWindows()

    Returns a list of the top-level windows in the application.

    \sa allWindows()
 */
QWindowList QGuiApplication::topLevelWindows()
{
    const QWindowList &list = QGuiApplicationPrivate::window_list;
    QWindowList topLevelWindows;
    for (int i = 0; i < list.size(); i++) {
        if (!list.at(i)->parent())
            topLevelWindows.prepend(list.at(i));
    }
    return topLevelWindows;
}

/*!
    Returns the primary (or default) screen of the application.

    This will be the screen where QWindows are shown, unless otherwise specified.
*/
QScreen *QGuiApplication::primaryScreen()
{
    if (QGuiApplicationPrivate::screen_list.isEmpty())
        return 0;
    return QGuiApplicationPrivate::screen_list.at(0);
}

/*!
    Returns a list of all the screens associated with the
    windowing system the application is connected to.
*/
QList<QScreen *> QGuiApplication::screens()
{
    return QGuiApplicationPrivate::screen_list;
}

/*!
    Returns the top level window at the given position, if any.
*/
QWindow *QGuiApplication::topLevelAt(const QPoint &pos)
{
    QList<QScreen *> screens = QGuiApplication::screens();
    QList<QScreen *>::const_iterator screen = screens.constBegin();
    QList<QScreen *>::const_iterator end = screens.constEnd();

    while (screen != end) {
        if ((*screen)->geometry().contains(pos))
            return (*screen)->handle()->topLevelAt(pos);
        ++screen;
    }
    return 0;
}

/*!
    \property QGuiApplication::platformName
    \brief The name of the underlying platform plugin.
*/

QString QGuiApplication::platformName()
{
    return QGuiApplicationPrivate::platform_name ?
           *QGuiApplicationPrivate::platform_name : QString();
}

static void init_platform(const QString &pluginArgument, const QString &platformPluginPath)
{
    // Split into platform name and arguments
    QString name;
    QStringList arguments;
    foreach (const QString &token, pluginArgument.split(QLatin1Char(':'))) {
        if (name.isEmpty()) {
            name = token;
        } else {
            arguments.push_back(token);
        }
    }

    if (name.isEmpty()) {
        const QStringList keys = QPlatformIntegrationFactory::keys(platformPluginPath);
#if defined(Q_OS_MAC)
        const QString defaultPlatform = QLatin1String("cocoa");
#elif defined (Q_OS_WIN)
        const QString defaultPlatform = QLatin1String("windows");
#elif defined (Q_OS_QNX)
        const QString defaultPlatform = QLatin1String("qnx");
#elif !defined (QT_NO_XCB)
        const QString defaultPlatform = QLatin1String("xcb");
#elif !defined (QT_NO_WAYLAND)
        const QString defaultPlatform = QLatin1String("wayland");
#elif !defined (QT_NO_EGLFS)
        const QString defaultPlatform = QLatin1String("eglfs");
#else
        const QString defaultPlatform = QLatin1String("minimal");
#endif
        if (keys.contains(defaultPlatform)) {
            qWarning("No platform plugin argument was specified, defaulting to \"%s\".",
                     qPrintable(defaultPlatform));
            name = defaultPlatform;
        } else {
            qFatal("No platform plugin argument was specified and the default plugin \"%s\" is not available",
                   qPrintable(defaultPlatform));
        }
    }

   // Create the platform integration.
    QGuiApplicationPrivate::platform_integration = QPlatformIntegrationFactory::create(name, platformPluginPath);
    if (QGuiApplicationPrivate::platform_integration) {
        QGuiApplicationPrivate::platform_name = new QString(name);
    } else {
        QStringList keys = QPlatformIntegrationFactory::keys(platformPluginPath);
        QString fatalMessage =
            QString::fromLatin1("Failed to load platform plugin \"%1\". Available platforms are: \n").arg(name);
        foreach(const QString &key, keys) {
            fatalMessage.append(key + QLatin1Char('\n'));
        }
        qFatal("%s", fatalMessage.toLocal8Bit().constData());
        return;
    }

    // Create the platform theme:
    // 1) Ask the platform integration for a list of names.
    const QStringList themeNames = QGuiApplicationPrivate::platform_integration->themeNames();
    foreach (const QString &themeName, themeNames) {
        QGuiApplicationPrivate::platform_theme = QPlatformThemeFactory::create(themeName, platformPluginPath);
        if (QGuiApplicationPrivate::platform_theme)
            break;
    }

    // 2) If none found, look for a theme plugin. Theme plugins are located in the
    // same directory as platform plugins.
    if (!QGuiApplicationPrivate::platform_theme) {
        foreach (const QString &themeName, themeNames) {
            QGuiApplicationPrivate::platform_theme = QGuiApplicationPrivate::platform_integration->createPlatformTheme(themeName);
            if (QGuiApplicationPrivate::platform_theme)
                break;
        }
        // No error message; not having a theme plugin is allowed.
    }

    // 3) Fall back on the built-in "null" platform theme.
    if (!QGuiApplicationPrivate::platform_theme)
        QGuiApplicationPrivate::platform_theme = new QPlatformTheme;

    // Set arguments as dynamic properties on the native interface as
    // boolean 'foo' or strings: 'foo=bar'
    if (!arguments.isEmpty()) {
        QObject *nativeInterface = QGuiApplicationPrivate::platform_integration->nativeInterface();
        foreach (const QString &argument, arguments) {
            const int equalsPos = argument.indexOf(QLatin1Char('='));
            const QByteArray name =
                equalsPos != -1 ? argument.left(equalsPos).toAscii() : argument.toAscii();
            const QVariant value =
                equalsPos != -1 ? QVariant(argument.mid(equalsPos + 1)) : QVariant(true);
            nativeInterface->setProperty(name.constData(), value);
        }
    }
    fontSmoothingGamma = QGuiApplicationPrivate::platformIntegration()->styleHint(QPlatformIntegration::FontSmoothingGamma).toReal();
}

static void init_plugins(const QList<QByteArray> &pluginList)
{
    for (int i = 0; i < pluginList.count(); ++i) {
        QByteArray pluginSpec = pluginList.at(i);
        int colonPos = pluginSpec.indexOf(':');
        QObject *plugin;
        if (colonPos < 0)
            plugin = QGenericPluginFactory::create(QLatin1String(pluginSpec), QString());
        else
            plugin = QGenericPluginFactory::create(QLatin1String(pluginSpec.mid(0, colonPos)),
                                                   QLatin1String(pluginSpec.mid(colonPos+1)));
        if (plugin)
            QGuiApplicationPrivate::generic_plugin_list.append(plugin);
    }
}

void QGuiApplicationPrivate::createPlatformIntegration()
{
    Q_Q(QGuiApplication);

    // Use the Qt menus by default. Platform plugins that
    // want to enable a native menu implementation can clear
    // this flag.
    q->setAttribute(Qt::AA_DontUseNativeMenuBar, true);

    // Load the platform integration
    QString platformPluginPath = QLatin1String(qgetenv("QT_QPA_PLATFORM_PLUGIN_PATH"));

    // On Mac, look inside the application bundle for the platform plugin.
    // TODO (msorvig): Create proper cross-platform solution for loading
    // deployed platform plugins
#ifdef Q_OS_MAC
    const QString bundlePluginPath = QCoreApplication::applicationDirPath() + QLatin1String("../Plugins/");
    if (platformPluginPath.isEmpty() && QDir(bundlePluginPath).exists()) {
        platformPluginPath = bundlePluginPath;
    }
#endif

    QByteArray platformName;
#ifdef QT_QPA_DEFAULT_PLATFORM_NAME
    platformName = QT_QPA_DEFAULT_PLATFORM_NAME;
#endif
    QByteArray platformNameEnv = qgetenv("QT_QPA_PLATFORM");
    if (!platformNameEnv.isEmpty()) {
        platformName = platformNameEnv;
    }

    // Get command line params

    int j = argc ? 1 : 0;
    for (int i=1; i<argc; i++) {
        if (argv[i] && *argv[i] != '-') {
            argv[j++] = argv[i];
            continue;
        }
        QByteArray arg = argv[i];
        if (arg == "-platformpluginpath") {
            if (++i < argc)
                platformPluginPath = QLatin1String(argv[i]);
        } else if (arg == "-platform") {
            if (++i < argc)
                platformName = argv[i];
        } else {
            argv[j++] = argv[i];
        }
    }

    if (j < argc) {
        argv[j] = 0;
        argc = j;
    }

    init_platform(QLatin1String(platformName), platformPluginPath);

}

void QGuiApplicationPrivate::createEventDispatcher()
{
    if (platform_integration == 0)
        createPlatformIntegration();

    if (!eventDispatcher) {
        QAbstractEventDispatcher *eventDispatcher = platform_integration->guiThreadEventDispatcher();
        setEventDispatcher(eventDispatcher);
    }
}

void QGuiApplicationPrivate::setEventDispatcher(QAbstractEventDispatcher *eventDispatcher)
{
    Q_Q(QGuiApplication);

    if (!QCoreApplicationPrivate::eventDispatcher) {
        QCoreApplicationPrivate::eventDispatcher = eventDispatcher;
        QCoreApplicationPrivate::eventDispatcher->setParent(q);
        threadData->eventDispatcher = eventDispatcher;
    }

}

void QGuiApplicationPrivate::init()
{
    QList<QByteArray> pluginList;
    // Get command line params

    int j = argc ? 1 : 0;
    for (int i=1; i<argc; i++) {
        if (argv[i] && *argv[i] != '-') {
            argv[j++] = argv[i];
            continue;
        }
        QByteArray arg = argv[i];
        if (arg == "-plugin") {
            if (++i < argc)
                pluginList << argv[i];
        } else if (arg == "-reverse") {
            force_reverse = true;
            QGuiApplication::setLayoutDirection(Qt::RightToLeft);
        } else {
            argv[j++] = argv[i];
        }
    }

    if (j < argc) {
        argv[j] = 0;
        argc = j;
    }

    if (platform_integration == 0)
        createPlatformIntegration();

    // Set up which span functions should be used in raster engine...
    qInitDrawhelperAsm();
    // and QImage conversion functions
    qInitImageConversions();

    QFont::initialize();

#ifndef QT_NO_CURSOR
    QCursorData::initialize();
#endif

    // trigger registering of QVariant's GUI types
    qRegisterGuiVariant();

    QWindowSystemInterfacePrivate::eventTime.start();

    is_app_running = true;
    init_plugins(pluginList);
    QWindowSystemInterface::sendWindowSystemEvents(QCoreApplicationPrivate::eventDispatcher, QEventLoop::AllEvents);
}

extern void qt_cleanupFontDatabase();

QGuiApplicationPrivate::~QGuiApplicationPrivate()
{
    is_app_closing = true;
    is_app_running = false;

    for (int i = 0; i < generic_plugin_list.count(); ++i)
        delete generic_plugin_list.at(i);
    generic_plugin_list.clear();

    clearFontUnlocked();

    QFont::cleanup();

#ifndef QT_NO_CURSOR
    QCursorData::cleanup();
#endif

    layout_direction = Qt::LeftToRight;

    cleanupThreadData();

    delete styleHints;
    delete inputMethod;

    qt_cleanupFontDatabase();

    delete  platform_theme;
    delete platform_integration;
    platform_integration = 0;
    delete m_gammaTables.load();
}

#if 0
#ifndef QT_NO_CURSOR
QCursor *overrideCursor();
void setOverrideCursor(const QCursor &);
void changeOverrideCursor(const QCursor &);
void restoreOverrideCursor();
#endif

static QFont font();
static QFont font(const QWidget*);
static QFont font(const char *className);
static void setFont(const QFont &, const char* className = 0);
static QFontMetrics fontMetrics();

#ifndef QT_NO_CLIPBOARD
static QClipboard *clipboard();
#endif
#endif

/*!
    Returns the current state of the modifier keys on the keyboard. The current
    state is updated sychronously as the event queue is emptied of events that
    will spontaneously change the keyboard state (QEvent::KeyPress and
    QEvent::KeyRelease events).

    It should be noted this may not reflect the actual keys held on the input
    device at the time of calling but rather the modifiers as last reported in
    one of the above events. If no keys are being held Qt::NoModifier is
    returned.

    \sa mouseButtons(), queryKeyboardModifiers()
*/
Qt::KeyboardModifiers QGuiApplication::keyboardModifiers()
{
    return QGuiApplicationPrivate::modifier_buttons;
}

/*!
    \fn Qt::KeyboardModifiers QApplication::queryKeyboardModifiers()

    Queries and returns the state of the modifier keys on the keyboard.
    Unlike keyboardModifiers, this method returns the actual keys held
    on the input device at the time of calling the method.

    It does not rely on the keypress events having been received by this
    process, which makes it possible to check the modifiers while moving
    a window, for instance. Note that in most cases, you should use
    keyboardModifiers(), which is faster and more accurate since it contains
    the state of the modifiers as they were when the currently processed
    event was received.

    \sa keyboardModifiers()
*/
Qt::KeyboardModifiers QGuiApplication::queryKeyboardModifiers()
{
    QPlatformIntegration *pi = QGuiApplicationPrivate::platformIntegration();
    return pi->queryKeyboardModifiers();
}

/*!
    Returns the current state of the buttons on the mouse. The current state is
    updated syncronously as the event queue is emptied of events that will
    spontaneously change the mouse state (QEvent::MouseButtonPress and
    QEvent::MouseButtonRelease events).

    It should be noted this may not reflect the actual buttons held on the
    input device at the time of calling but rather the mouse buttons as last
    reported in one of the above events. If no mouse buttons are being held
    Qt::NoButton is returned.

    \sa keyboardModifiers()
*/
Qt::MouseButtons QGuiApplication::mouseButtons()
{
    return QGuiApplicationPrivate::mouse_buttons;
}

/*!
    Returns the platform's native interface, for platform specific
    functionality.
*/
QPlatformNativeInterface *QGuiApplication::platformNativeInterface()
{
    QPlatformIntegration *pi = QGuiApplicationPrivate::platformIntegration();
    return pi->nativeInterface();
}

/*!
    Enters the main event loop and waits until exit() is called, and then
    returns the value that was set to exit() (which is 0 if exit() is called
    via quit()).

    It is necessary to call this function to start event handling. The main
    event loop receives events from the window system and dispatches these to
    the application widgets.

    Generally, no user interaction can take place before calling exec().

    To make your application perform idle processing, e.g., executing a special
    function whenever there are no pending events, use a QTimer with 0 timeout.
    More advanced idle processing schemes can be achieved using processEvents().

    We recommend that you connect clean-up code to the
    \l{QCoreApplication::}{aboutToQuit()} signal, instead of putting it in your
    application's \c{main()} function. This is because, on some platforms, the
    QApplication::exec() call may not return.

    \sa quitOnLastWindowClosed, quit(), exit(), processEvents(),
        QCoreApplication::exec()
*/
int QGuiApplication::exec()
{
    return QCoreApplication::exec();
}

/*! \reimp
*/
bool QGuiApplication::notify(QObject *object, QEvent *event)
{
#ifndef QT_NO_SHORTCUT
    if (event->type() == QEvent::KeyPress) {
        // Try looking for a Shortcut before sending key events
        QWindow *w = qobject_cast<QWindow *>(object);
        QObject *focus = w ? w->focusObject() : 0;
        if (!focus)
            focus = object;
        if (QGuiApplicationPrivate::instance()->shortcutMap.tryShortcutEvent(focus, static_cast<QKeyEvent *>(event)))
            return true;
    }
#endif

    return QCoreApplication::notify(object, event);
}

/*! \reimp
*/
bool QGuiApplication::event(QEvent *e)
{
    if(e->type() == QEvent::LanguageChange) {
        setLayoutDirection(qt_detectRTLLanguage()?Qt::RightToLeft:Qt::LeftToRight);
    }
    return QCoreApplication::event(e);
}

/*!
    \internal
*/
bool QGuiApplication::compressEvent(QEvent *event, QObject *receiver, QPostEventList *postedEvents)
{
    return QCoreApplication::compressEvent(event, receiver, postedEvents);
}

bool QGuiApplicationPrivate::processNativeEvent(QWindow *window, const QByteArray &eventType, void *message, long *result)
{
    return window->nativeEvent(eventType, message, result);
}

void QGuiApplicationPrivate::processWindowSystemEvent(QWindowSystemInterfacePrivate::WindowSystemEvent *e)
{
    switch(e->type) {
    case QWindowSystemInterfacePrivate::Mouse:
        QGuiApplicationPrivate::processMouseEvent(static_cast<QWindowSystemInterfacePrivate::MouseEvent *>(e));
        break;
    case QWindowSystemInterfacePrivate::Wheel:
        QGuiApplicationPrivate::processWheelEvent(static_cast<QWindowSystemInterfacePrivate::WheelEvent *>(e));
        break;
    case QWindowSystemInterfacePrivate::Key:
        QGuiApplicationPrivate::processKeyEvent(static_cast<QWindowSystemInterfacePrivate::KeyEvent *>(e));
        break;
    case QWindowSystemInterfacePrivate::Touch:
        QGuiApplicationPrivate::processTouchEvent(static_cast<QWindowSystemInterfacePrivate::TouchEvent *>(e));
        break;
    case QWindowSystemInterfacePrivate::GeometryChange:
        QGuiApplicationPrivate::processGeometryChangeEvent(static_cast<QWindowSystemInterfacePrivate::GeometryChangeEvent*>(e));
        break;
    case QWindowSystemInterfacePrivate::Enter:
        QGuiApplicationPrivate::processEnterEvent(static_cast<QWindowSystemInterfacePrivate::EnterEvent *>(e));
        break;
    case QWindowSystemInterfacePrivate::Leave:
        QGuiApplicationPrivate::processLeaveEvent(static_cast<QWindowSystemInterfacePrivate::LeaveEvent *>(e));
        break;
    case QWindowSystemInterfacePrivate::ActivatedWindow:
        QGuiApplicationPrivate::processActivatedEvent(static_cast<QWindowSystemInterfacePrivate::ActivatedWindowEvent *>(e));
        break;
    case QWindowSystemInterfacePrivate::WindowStateChanged:
        QGuiApplicationPrivate::processWindowStateChangedEvent(static_cast<QWindowSystemInterfacePrivate::WindowStateChangedEvent *>(e));
        break;
    case QWindowSystemInterfacePrivate::Close:
        QGuiApplicationPrivate::processCloseEvent(
                static_cast<QWindowSystemInterfacePrivate::CloseEvent *>(e));
        break;
    case QWindowSystemInterfacePrivate::ScreenOrientation:
        QGuiApplicationPrivate::reportScreenOrientationChange(
                static_cast<QWindowSystemInterfacePrivate::ScreenOrientationEvent *>(e));
        break;
    case QWindowSystemInterfacePrivate::ScreenGeometry:
        QGuiApplicationPrivate::reportGeometryChange(
                static_cast<QWindowSystemInterfacePrivate::ScreenGeometryEvent *>(e));
        break;
    case QWindowSystemInterfacePrivate::ScreenAvailableGeometry:
        QGuiApplicationPrivate::reportAvailableGeometryChange(
                static_cast<QWindowSystemInterfacePrivate::ScreenAvailableGeometryEvent *>(e));
        break;
    case QWindowSystemInterfacePrivate::ScreenLogicalDotsPerInch:
        QGuiApplicationPrivate::reportLogicalDotsPerInchChange(
                static_cast<QWindowSystemInterfacePrivate::ScreenLogicalDotsPerInchEvent *>(e));
        break;
    case QWindowSystemInterfacePrivate::ThemeChange:
        QGuiApplicationPrivate::processThemeChanged(
                    static_cast<QWindowSystemInterfacePrivate::ThemeChangeEvent *>(e));
        break;
    case QWindowSystemInterfacePrivate::Map:
        QGuiApplicationPrivate::processMapEvent(static_cast<QWindowSystemInterfacePrivate::MapEvent *>(e));
        break;
    case QWindowSystemInterfacePrivate::Unmap:
        QGuiApplicationPrivate::processUnmapEvent(static_cast<QWindowSystemInterfacePrivate::UnmapEvent *>(e));
        break;
    case QWindowSystemInterfacePrivate::Expose:
        QGuiApplicationPrivate::processExposeEvent(static_cast<QWindowSystemInterfacePrivate::ExposeEvent *>(e));
        break;
    default:
        qWarning() << "Unknown user input event type:" << e->type;
        break;
    }
}

void QGuiApplicationPrivate::processMouseEvent(QWindowSystemInterfacePrivate::MouseEvent *e)
{
    QEvent::Type type;
    // move first
    Qt::MouseButtons stateChange = e->buttons ^ buttons;
    if (e->globalPos != QGuiApplicationPrivate::lastCursorPosition && (stateChange != Qt::NoButton)) {
        QWindowSystemInterfacePrivate::MouseEvent * newMouseEvent =
                new QWindowSystemInterfacePrivate::MouseEvent(e->window.data(), e->timestamp, e->localPos, e->globalPos, e->buttons, e->modifiers);
        QWindowSystemInterfacePrivate::windowSystemEventQueue.prepend(newMouseEvent); // just in case the move triggers a new event loop
        stateChange = Qt::NoButton;
    }

    QWindow *window = e->window.data();
    modifier_buttons = e->modifiers;

    if (!window)
        window = QGuiApplication::topLevelAt(e->globalPos.toPoint());

    QPointF localPoint = e->localPos;
    QPointF globalPoint = e->globalPos;

    Qt::MouseButton button = Qt::NoButton;
    bool doubleClick = false;

    if (QGuiApplicationPrivate::lastCursorPosition != globalPoint) {
        type = QEvent::MouseMove;
        QGuiApplicationPrivate::lastCursorPosition = globalPoint;
        if (qAbs(globalPoint.x() - mousePressX) > mouse_double_click_distance||
            qAbs(globalPoint.y() - mousePressY) > mouse_double_click_distance)
            mousePressButton = Qt::NoButton;
    } else { // Check to see if a new button has been pressed/released.
        for (int check = Qt::LeftButton;
            check <= int(Qt::MaxMouseButton);
             check = check << 1) {
            if (check & stateChange) {
                button = Qt::MouseButton(check);
                break;
            }
        }
        if (button == Qt::NoButton) {
            // Ignore mouse events that don't change the current state.
            return;
        }
        buttons = e->buttons;
        if (button & e->buttons) {
            ulong doubleClickInterval = static_cast<ulong>(qApp->styleHints()->mouseDoubleClickInterval());
            doubleClick = e->timestamp - mousePressTime < doubleClickInterval && button == mousePressButton;
            type = QEvent::MouseButtonPress;
            mousePressTime = e->timestamp;
            mousePressButton = button;
            const QPoint point = QGuiApplicationPrivate::lastCursorPosition.toPoint();
            mousePressX = point.x();
            mousePressY = point.y();
        } else {
            type = QEvent::MouseButtonRelease;
        }
    }

    if (window) {
        QMouseEvent ev(type, localPoint, localPoint, globalPoint, button, buttons, e->modifiers);
        ev.setTimestamp(e->timestamp);
#ifndef QT_NO_CURSOR
        if (const QScreen *screen = window->screen())
            if (QPlatformCursor *cursor = screen->handle()->cursor())
                cursor->pointerEvent(ev);
#endif
        QGuiApplication::sendSpontaneousEvent(window, &ev);
        if (!e->synthetic && !ev.isAccepted() && qApp->testAttribute(Qt::AA_SynthesizeTouchForUnhandledMouseEvents)) {
            if (!m_fakeTouchDevice) {
                m_fakeTouchDevice = new QTouchDevice;
                QWindowSystemInterface::registerTouchDevice(m_fakeTouchDevice);
            }
            QList<QWindowSystemInterface::TouchPoint> points;
            QWindowSystemInterface::TouchPoint point;
            point.id = 1;
            point.area = QRectF(globalPoint.x() - 2, globalPoint.y() - 2, 4, 4);

            // only translate left button related events to
            // avoid strange touch event sequences when several
            // buttons are pressed
            if (type == QEvent::MouseButtonPress && button == Qt::LeftButton) {
                point.state = Qt::TouchPointPressed;
            } else if (type == QEvent::MouseButtonRelease && button == Qt::LeftButton) {
                point.state = Qt::TouchPointReleased;
            } else if (type == QEvent::MouseMove && (buttons & Qt::LeftButton)) {
                point.state = Qt::TouchPointMoved;
            } else {
                return;
            }

            points << point;

            QEvent::Type type;
            QList<QTouchEvent::TouchPoint> touchPoints = QWindowSystemInterfacePrivate::convertTouchPoints(points, &type);

            QWindowSystemInterfacePrivate::TouchEvent fake(window, e->timestamp, type, m_fakeTouchDevice, touchPoints, e->modifiers);
            fake.synthetic = true;
            processTouchEvent(&fake);
        }
        if (doubleClick) {
            mousePressButton = Qt::NoButton;
            QMouseEvent dblClickEvent(QEvent::MouseButtonDblClick, localPoint, localPoint, globalPoint,
                                      button, buttons, e->modifiers);
            dblClickEvent.setTimestamp(e->timestamp);
            QGuiApplication::sendSpontaneousEvent(window, &dblClickEvent);
        }
    }
}

void QGuiApplicationPrivate::processWheelEvent(QWindowSystemInterfacePrivate::WheelEvent *e)
{
    if (!e->window)
        return;

    QPointF globalPoint = e->globalPos;
    QGuiApplicationPrivate::lastCursorPosition = globalPoint;
    modifier_buttons = e->modifiers;

    QWindow *window = e->window.data();

    if (window) {
         QWheelEvent ev(e->localPos, e->globalPos, e->pixelDelta, e->angleDelta, e->qt4Delta, e->qt4Orientation, buttons, e->modifiers);
         ev.setTimestamp(e->timestamp);
         QGuiApplication::sendSpontaneousEvent(window, &ev);
         return;
     }
}

// Remember, Qt convention is:  keyboard state is state *before*

void QGuiApplicationPrivate::processKeyEvent(QWindowSystemInterfacePrivate::KeyEvent *e)
{
    QWindow *window = e->window.data();
    modifier_buttons = e->modifiers;
    if (e->nullWindow)
        window = QGuiApplication::activeWindow();
    if (!window)
        return;

    QKeyEventEx ev(e->keyType, e->key, e->modifiers, e->unicode, e->repeat, e->repeatCount,
                   e->nativeScanCode, e->nativeVirtualKey, e->nativeModifiers);
    ev.setTimestamp(e->timestamp);
    QGuiApplication::sendSpontaneousEvent(window, &ev);
}

void QGuiApplicationPrivate::processEnterEvent(QWindowSystemInterfacePrivate::EnterEvent *e)
{
    if (!e->enter)
        return;

    QEvent event(QEvent::Enter);
    QCoreApplication::sendSpontaneousEvent(e->enter.data(), &event);
}

void QGuiApplicationPrivate::processLeaveEvent(QWindowSystemInterfacePrivate::LeaveEvent *e)
{
    if (!e->leave)
        return;

    QEvent event(QEvent::Leave);
    QCoreApplication::sendSpontaneousEvent(e->leave.data(), &event);
}

void QGuiApplicationPrivate::processActivatedEvent(QWindowSystemInterfacePrivate::ActivatedWindowEvent *e)
{
    QWindow *previous = QGuiApplicationPrivate::focus_window;
    QWindow *newFocus = e->activated.data();

    if (previous == newFocus)
        return;

    QObject *previousFocusObject = previous ? previous->focusObject() : 0;

    if (previous) {
        QFocusEvent focusAboutToChange(QEvent::FocusAboutToChange);
        QCoreApplication::sendSpontaneousEvent(previous, &focusAboutToChange);
    }

    QGuiApplicationPrivate::focus_window = newFocus;

    if (previous) {
        QFocusEvent focusOut(QEvent::FocusOut);
        QCoreApplication::sendSpontaneousEvent(previous, &focusOut);
        QObject::disconnect(previous, SIGNAL(focusObjectChanged(QObject*)),
                            qApp, SLOT(q_updateFocusObject(QObject*)));
    } else {
        QEvent appActivate(QEvent::ApplicationActivate);
        qApp->sendSpontaneousEvent(qApp, &appActivate);
    }

    if (QGuiApplicationPrivate::focus_window) {
        QFocusEvent focusIn(QEvent::FocusIn);
        QCoreApplication::sendSpontaneousEvent(QGuiApplicationPrivate::focus_window, &focusIn);
        QObject::connect(QGuiApplicationPrivate::focus_window, SIGNAL(focusObjectChanged(QObject*)),
                         qApp, SLOT(q_updateFocusObject(QObject*)));
    } else {
        QEvent appActivate(QEvent::ApplicationDeactivate);
        qApp->sendSpontaneousEvent(qApp, &appActivate);
    }

    if (self) {
        self->notifyActiveWindowChange(previous);

        if (previousFocusObject != qApp->focusObject())
            self->q_updateFocusObject(qApp->focusObject());
    }
}

void QGuiApplicationPrivate::processWindowStateChangedEvent(QWindowSystemInterfacePrivate::WindowStateChangedEvent *wse)
{
    if (QWindow *window  = wse->window.data()) {
        QWindowStateChangeEvent e(window->windowState());
        window->d_func()->windowState = wse->newState;
        QGuiApplication::sendSpontaneousEvent(window, &e);
    }
}

void QGuiApplicationPrivate::processThemeChanged(QWindowSystemInterfacePrivate::ThemeChangeEvent *tce)
{
    if (self)
        self->notifyThemeChanged();
    if (QWindow *window  = tce->window.data()) {
        QEvent e(QEvent::ThemeChange);
        QGuiApplication::sendSpontaneousEvent(window, &e);
    }
}

void QGuiApplicationPrivate::processGeometryChangeEvent(QWindowSystemInterfacePrivate::GeometryChangeEvent *e)
{
    if (e->tlw.isNull())
       return;

    QWindow *window = e->tlw.data();
    if (!window)
        return;

    QRect newRect = e->newGeometry;
    QRect cr = window->d_func()->geometry;

    bool isResize = cr.size() != newRect.size();
    bool isMove = cr.topLeft() != newRect.topLeft();

    window->d_func()->geometry = newRect;

    if (isResize || window->d_func()->resizeEventPending) {
        QResizeEvent e(newRect.size(), cr.size());
        QGuiApplication::sendSpontaneousEvent(window, &e);

        window->d_func()->resizeEventPending = false;

        if (cr.width() != newRect.width())
            window->widthChanged(newRect.width());
        if (cr.height() != newRect.height())
            window->heightChanged(newRect.height());
    }

    if (isMove) {
        //### frame geometry
        QMoveEvent e(newRect.topLeft(), cr.topLeft());
        QGuiApplication::sendSpontaneousEvent(window, &e);

        if (cr.x() != newRect.x())
            window->xChanged(newRect.x());
        if (cr.y() != newRect.y())
            window->yChanged(newRect.y());
    }
}

void QGuiApplicationPrivate::processCloseEvent(QWindowSystemInterfacePrivate::CloseEvent *e)
{
    if (e->window.isNull())
        return;

    QCloseEvent event;
    QGuiApplication::sendSpontaneousEvent(e->window.data(), &event);
}

Q_GUI_EXPORT uint qHash(const QGuiApplicationPrivate::ActiveTouchPointsKey &k)
{
    return qHash(k.device) + k.touchPointId;
}

Q_GUI_EXPORT bool operator==(const QGuiApplicationPrivate::ActiveTouchPointsKey &a,
                             const QGuiApplicationPrivate::ActiveTouchPointsKey &b)
{
    return a.device == b.device
            && a.touchPointId == b.touchPointId;
}

void QGuiApplicationPrivate::processTouchEvent(QWindowSystemInterfacePrivate::TouchEvent *e)
{
    QGuiApplicationPrivate *d = self;
    modifier_buttons = e->modifiers;

    if (e->touchType == QEvent::TouchCancel) {
        // The touch sequence has been canceled (e.g. by the compositor).
        // Send the TouchCancel to all windows with active touches and clean up.
        QTouchEvent touchEvent(QEvent::TouchCancel, e->device, e->modifiers);
        touchEvent.setTimestamp(e->timestamp);
        QHash<ActiveTouchPointsKey, ActiveTouchPointsValue>::const_iterator it
                = self->activeTouchPoints.constBegin(), ite = self->activeTouchPoints.constEnd();
        QSet<QWindow *> windowsNeedingCancel;
        while (it != ite) {
            QWindow *w = it->window.data();
            if (w)
                windowsNeedingCancel.insert(w);
            ++it;
        }
        for (QSet<QWindow *>::const_iterator winIt = windowsNeedingCancel.constBegin(),
             winItEnd = windowsNeedingCancel.constEnd(); winIt != winItEnd; ++winIt) {
            touchEvent.setWindow(*winIt);
            QGuiApplication::sendSpontaneousEvent(*winIt, &touchEvent);
        }
        if (!self->synthesizedMousePoints.isEmpty() && !e->synthetic) {
            for (QHash<QWindow *, SynthesizedMouseData>::const_iterator synthIt = self->synthesizedMousePoints.constBegin(),
                 synthItEnd = self->synthesizedMousePoints.constEnd(); synthIt != synthItEnd; ++synthIt) {
                if (!synthIt->window)
                    continue;
                QWindowSystemInterfacePrivate::MouseEvent fake(synthIt->window.data(),
                                                               e->timestamp,
                                                               synthIt->pos,
                                                               synthIt->screenPos,
                                                               Qt::NoButton,
                                                               e->modifiers);
                fake.synthetic = true;
                processMouseEvent(&fake);
            }
            self->synthesizedMousePoints.clear();
        }
        self->activeTouchPoints.clear();
        self->lastTouchType = e->touchType;
        return;
    }

    // Prevent sending ill-formed event sequences: Cancel can only be followed by a Begin.
    if (self->lastTouchType == QEvent::TouchCancel && e->touchType != QEvent::TouchBegin)
        return;

    self->lastTouchType = e->touchType;

    QWindow *window = e->window.data();
    typedef QPair<Qt::TouchPointStates, QList<QTouchEvent::TouchPoint> > StatesAndTouchPoints;
    QHash<QWindow *, StatesAndTouchPoints> windowsNeedingEvents;

    for (int i = 0; i < e->points.count(); ++i) {
        QTouchEvent::TouchPoint touchPoint = e->points.at(i);
        // explicitly detach from the original touch point that we got, so even
        // if the touchpoint structs are reused, we will make a copy that we'll
        // deliver to the user (which might want to store the struct for later use).
        touchPoint.d = touchPoint.d->detach();

        // update state
        QWeakPointer<QWindow> w;
        QTouchEvent::TouchPoint previousTouchPoint;
        ActiveTouchPointsKey touchInfoKey(e->device, touchPoint.id());
        ActiveTouchPointsValue &touchInfo = d->activeTouchPoints[touchInfoKey];
        switch (touchPoint.state()) {
        case Qt::TouchPointPressed:
            if (e->device->type() == QTouchDevice::TouchPad) {
                // on touch-pads, send all touch points to the same widget
                w = d->activeTouchPoints.isEmpty()
                    ? QWeakPointer<QWindow>()
                    : d->activeTouchPoints.constBegin().value().window;
            }

            if (!w) {
                // determine which window this event will go to
                if (!window)
                    window = QGuiApplication::topLevelAt(touchPoint.screenPos().toPoint());
                if (!window)
                    continue;
                w = window;
            }

            touchInfo.window = w;
            touchPoint.d->startScreenPos = touchPoint.screenPos();
            touchPoint.d->lastScreenPos = touchPoint.screenPos();
            touchPoint.d->startNormalizedPos = touchPoint.normalizedPos();
            touchPoint.d->lastNormalizedPos = touchPoint.normalizedPos();
            if (touchPoint.pressure() < qreal(0.))
                touchPoint.d->pressure = qreal(1.);

            touchInfo.touchPoint = touchPoint;
            break;

        case Qt::TouchPointReleased:
            w = touchInfo.window;
            if (!w)
                continue;

            previousTouchPoint = touchInfo.touchPoint;
            touchPoint.d->startScreenPos = previousTouchPoint.startScreenPos();
            touchPoint.d->lastScreenPos = previousTouchPoint.screenPos();
            touchPoint.d->startPos = previousTouchPoint.startPos();
            touchPoint.d->lastPos = previousTouchPoint.pos();
            touchPoint.d->startNormalizedPos = previousTouchPoint.startNormalizedPos();
            touchPoint.d->lastNormalizedPos = previousTouchPoint.normalizedPos();
            if (touchPoint.pressure() < qreal(0.))
                touchPoint.d->pressure = qreal(0.);

            break;

        default:
            w = touchInfo.window;
            if (!w)
                continue;

            previousTouchPoint = touchInfo.touchPoint;
            touchPoint.d->startScreenPos = previousTouchPoint.startScreenPos();
            touchPoint.d->lastScreenPos = previousTouchPoint.screenPos();
            touchPoint.d->startPos = previousTouchPoint.startPos();
            touchPoint.d->lastPos = previousTouchPoint.pos();
            touchPoint.d->startNormalizedPos = previousTouchPoint.startNormalizedPos();
            touchPoint.d->lastNormalizedPos = previousTouchPoint.normalizedPos();
            if (touchPoint.pressure() < qreal(0.))
                touchPoint.d->pressure = qreal(1.);

            // Stationary points might not be delivered down to the receiving item
            // and get their position transformed, keep the old values instead.
            if (touchPoint.state() != Qt::TouchPointStationary)
                touchInfo.touchPoint = touchPoint;
            break;
        }

        Q_ASSERT(w.data() != 0);

        // make the *scene* functions return the same as the *screen* functions
        touchPoint.d->sceneRect = touchPoint.screenRect();
        touchPoint.d->startScenePos = touchPoint.startScreenPos();
        touchPoint.d->lastScenePos = touchPoint.lastScreenPos();

        StatesAndTouchPoints &maskAndPoints = windowsNeedingEvents[w.data()];
        maskAndPoints.first |= touchPoint.state();
        maskAndPoints.second.append(touchPoint);
    }

    if (windowsNeedingEvents.isEmpty())
        return;

    QHash<QWindow *, StatesAndTouchPoints>::ConstIterator it = windowsNeedingEvents.constBegin();
    const QHash<QWindow *, StatesAndTouchPoints>::ConstIterator end = windowsNeedingEvents.constEnd();
    for (; it != end; ++it) {
        QWindow *w = it.key();

        QEvent::Type eventType;
        switch (it.value().first) {
        case Qt::TouchPointPressed:
            eventType = QEvent::TouchBegin;
            break;
        case Qt::TouchPointReleased:
            eventType = QEvent::TouchEnd;
            break;
        case Qt::TouchPointStationary:
            // don't send the event if nothing changed
            continue;
        default:
            eventType = QEvent::TouchUpdate;
            break;
        }

        QTouchEvent touchEvent(eventType,
                               e->device,
                               e->modifiers,
                               it.value().first,
                               it.value().second);
        touchEvent.setTimestamp(e->timestamp);
        touchEvent.setWindow(w);

        const int pointCount = touchEvent.touchPoints().count();
        for (int i = 0; i < pointCount; ++i) {
            QTouchEvent::TouchPoint &touchPoint = touchEvent._touchPoints[i];

            // preserve the sub-pixel resolution
            QRectF rect = touchPoint.screenRect();
            const QPointF screenPos = rect.center();
            const QPointF delta = screenPos - screenPos.toPoint();

            rect.moveCenter(w->mapFromGlobal(screenPos.toPoint()) + delta);
            touchPoint.d->rect = rect;
            if (touchPoint.state() == Qt::TouchPointPressed) {
                touchPoint.d->startPos = w->mapFromGlobal(touchPoint.startScreenPos().toPoint()) + delta;
                touchPoint.d->lastPos = w->mapFromGlobal(touchPoint.lastScreenPos().toPoint()) + delta;
            }
        }

        QGuiApplication::sendSpontaneousEvent(w, &touchEvent);
        if (!e->synthetic && !touchEvent.isAccepted() && qApp->testAttribute(Qt::AA_SynthesizeMouseForUnhandledTouchEvents)) {
            // exclude touchpads as those generate their own mouse events
            if (touchEvent.device()->type() != QTouchDevice::TouchPad) {
                Qt::MouseButtons b = eventType == QEvent::TouchEnd ? Qt::NoButton : Qt::LeftButton;
                if (b == Qt::NoButton)
                    self->synthesizedMousePoints.clear();

                QList<QTouchEvent::TouchPoint> touchPoints = touchEvent.touchPoints();
                if (eventType == QEvent::TouchBegin)
                    m_fakeMouseSourcePointId = touchPoints.first().id();

                for (int i = 0; i < touchPoints.count(); ++i) {
                    const QTouchEvent::TouchPoint &touchPoint = touchPoints.at(i);
                    if (touchPoint.id() == m_fakeMouseSourcePointId) {
                        if (b != Qt::NoButton)
                            self->synthesizedMousePoints.insert(w, SynthesizedMouseData(
                                                                    touchPoint.pos(), touchPoint.screenPos(), w));
                        QWindowSystemInterfacePrivate::MouseEvent fake(w, e->timestamp,
                                                                       touchPoint.pos(),
                                                                       touchPoint.screenPos(),
                                                                       b, e->modifiers);
                        fake.synthetic = true;
                        processMouseEvent(&fake);
                        break;
                    }
                }
            }
        }
    }

    // Remove released points from the hash table only after the event is
    // delivered. When the receiver is a widget, QApplication will access
    // activeTouchPoints during delivery and therefore nothing can be removed
    // before sending the event.
    for (int i = 0; i < e->points.count(); ++i) {
        QTouchEvent::TouchPoint touchPoint = e->points.at(i);
        if (touchPoint.state() == Qt::TouchPointReleased)
            d->activeTouchPoints.remove(ActiveTouchPointsKey(e->device, touchPoint.id()));
    }
}

void QGuiApplicationPrivate::reportScreenOrientationChange(QWindowSystemInterfacePrivate::ScreenOrientationEvent *e)
{
    // This operation only makes sense after the QGuiApplication constructor runs
    if (QCoreApplication::startingUp())
        return;

    if (!e->screen)
        return;

    QScreen *s = e->screen.data();
    s->d_func()->orientation = e->orientation;

    reportScreenOrientationChange(s);
}

void QGuiApplicationPrivate::reportScreenOrientationChange(QScreen *s)
{
    emit s->orientationChanged(s->orientation());

    QScreenOrientationChangeEvent event(s, s->orientation());
    QCoreApplication::sendEvent(QCoreApplication::instance(), &event);
}

void QGuiApplicationPrivate::reportGeometryChange(QWindowSystemInterfacePrivate::ScreenGeometryEvent *e)
{
    // This operation only makes sense after the QGuiApplication constructor runs
    if (QCoreApplication::startingUp())
        return;

    if (!e->screen)
        return;

    QScreen *s = e->screen.data();
    s->d_func()->geometry = e->geometry;

    Qt::ScreenOrientation primaryOrientation = s->primaryOrientation();
    Qt::ScreenOrientation orientation = s->orientation();
    s->d_func()->updatePrimaryOrientation();

    emit s->sizeChanged(s->size());
    emit s->geometryChanged(s->geometry());
    emit s->physicalDotsPerInchXChanged(s->physicalDotsPerInchX());
    emit s->physicalDotsPerInchYChanged(s->physicalDotsPerInchY());
    emit s->physicalDotsPerInchChanged(s->physicalDotsPerInch());
    emit s->availableSizeChanged(s->availableSize());
    emit s->availableGeometryChanged(s->availableGeometry());

    if (s->primaryOrientation() != primaryOrientation)
        emit s->primaryOrientationChanged(s->primaryOrientation());

    if (s->orientation() != orientation)
        reportScreenOrientationChange(s);
}

void QGuiApplicationPrivate::reportAvailableGeometryChange(
        QWindowSystemInterfacePrivate::ScreenAvailableGeometryEvent *e)
{
    // This operation only makes sense after the QGuiApplication constructor runs
    if (QCoreApplication::startingUp())
        return;

    if (!e->screen)
        return;

    QScreen *s = e->screen.data();
    s->d_func()->availableGeometry = e->availableGeometry;

    emit s->availableSizeChanged(s->availableSize());
    emit s->availableGeometryChanged(s->availableGeometry());
}

void QGuiApplicationPrivate::reportLogicalDotsPerInchChange(QWindowSystemInterfacePrivate::ScreenLogicalDotsPerInchEvent *e)
{
    // This operation only makes sense after the QGuiApplication constructor runs
    if (QCoreApplication::startingUp())
        return;

    if (!e->screen)
        return;

    QScreen *s = e->screen.data();
    s->d_func()->logicalDpi = QDpi(e->dpiX, e->dpiY);

    emit s->logicalDotsPerInchXChanged(s->logicalDotsPerInchX());
    emit s->logicalDotsPerInchYChanged(s->logicalDotsPerInchY());
    emit s->logicalDotsPerInchChanged(s->logicalDotsPerInch());
}

void QGuiApplicationPrivate::processMapEvent(QWindowSystemInterfacePrivate::MapEvent *e)
{
    if (!e->mapped)
        return;

    QEvent event(QEvent::Map);
    QCoreApplication::sendSpontaneousEvent(e->mapped.data(), &event);
}

void QGuiApplicationPrivate::processUnmapEvent(QWindowSystemInterfacePrivate::UnmapEvent *e)
{
    if (!e->unmapped)
        return;

    QEvent event(QEvent::Unmap);
    QCoreApplication::sendSpontaneousEvent(e->unmapped.data(), &event);
}

void QGuiApplicationPrivate::processExposeEvent(QWindowSystemInterfacePrivate::ExposeEvent *e)
{
    if (!e->exposed)
        return;

    QWindow *window = e->exposed.data();

    QExposeEvent exposeEvent(e->region);
    QCoreApplication::sendSpontaneousEvent(window, &exposeEvent);
}

QPlatformDragQtResponse QGuiApplicationPrivate::processDrag(QWindow *w, const QMimeData *dropData, const QPoint &p, Qt::DropActions supportedActions)
{
    static QPointer<QWindow> currentDragWindow;
    static Qt::DropAction lastAcceptedDropAction = Qt::IgnoreAction;
    QPlatformDrag *platformDrag = platformIntegration()->drag();
    if (!platformDrag) {
        lastAcceptedDropAction = Qt::IgnoreAction;
        return QPlatformDragQtResponse(false, lastAcceptedDropAction, QRect());
    }

    if (!dropData) {
        if (currentDragWindow.data() == w)
            currentDragWindow = 0;
        QDragLeaveEvent e;
        QGuiApplication::sendEvent(w, &e);
        lastAcceptedDropAction = Qt::IgnoreAction;
        return QPlatformDragQtResponse(false, lastAcceptedDropAction, QRect());
    }
    QDragMoveEvent me(p, supportedActions, dropData,
                      QGuiApplication::mouseButtons(), QGuiApplication::keyboardModifiers());

    if (w != currentDragWindow) {
        lastAcceptedDropAction = Qt::IgnoreAction;
        if (currentDragWindow) {
            QDragLeaveEvent e;
            QGuiApplication::sendEvent(currentDragWindow, &e);
        }
        currentDragWindow = w;
        QDragEnterEvent e(p, supportedActions, dropData,
                          QGuiApplication::mouseButtons(), QGuiApplication::keyboardModifiers());
        QGuiApplication::sendEvent(w, &e);
        if (e.isAccepted() && e.dropAction() != Qt::IgnoreAction)
            lastAcceptedDropAction = e.dropAction();
    }

    // Handling 'DragEnter' should suffice for the application.
    if (lastAcceptedDropAction != Qt::IgnoreAction
        && (supportedActions & lastAcceptedDropAction)) {
        me.setDropAction(lastAcceptedDropAction);
        me.accept();
    }
    QGuiApplication::sendEvent(w, &me);
    lastAcceptedDropAction = me.isAccepted() ?
                             me.dropAction() :  Qt::IgnoreAction;
    return QPlatformDragQtResponse(me.isAccepted(), lastAcceptedDropAction, me.answerRect());
}

QPlatformDropQtResponse QGuiApplicationPrivate::processDrop(QWindow *w, const QMimeData *dropData, const QPoint &p, Qt::DropActions supportedActions)
{
    QDropEvent de(p, supportedActions, dropData,
                  QGuiApplication::mouseButtons(), QGuiApplication::keyboardModifiers());
    QGuiApplication::sendEvent(w, &de);

    Qt::DropAction acceptedAction = de.isAccepted() ? de.dropAction() : Qt::IgnoreAction;
    QPlatformDropQtResponse response(de.isAccepted(),acceptedAction);
    return response;
}

#ifndef QT_NO_CLIPBOARD
/*!
    Returns the object for interacting with the clipboard.
*/
QClipboard * QGuiApplication::clipboard()
{
    if (QGuiApplicationPrivate::qt_clipboard == 0) {
        if (!qApp) {
            qWarning("QGuiApplication: Must construct a QGuiApplication before accessing a QClipboard");
            return 0;
        }
        QGuiApplicationPrivate::qt_clipboard = new QClipboard(0);
    }
    return QGuiApplicationPrivate::qt_clipboard;
}
#endif

/*!
    Returns the default application palette.

    \sa setPalette()
*/

QPalette QGuiApplication::palette()
{
    initPalette();
    return *QGuiApplicationPrivate::app_pal;
}

/*!
    Changes the default application palette to \a palette.

    \sa palette()
*/
void QGuiApplication::setPalette(const QPalette &pal)
{
    if (QGuiApplicationPrivate::app_pal && pal.isCopyOf(*QGuiApplicationPrivate::app_pal))
        return;
    if (!QGuiApplicationPrivate::app_pal)
        QGuiApplicationPrivate::app_pal = new QPalette(pal);
    else
        *QGuiApplicationPrivate::app_pal = pal;
    applicationResourceFlags |= ApplicationPaletteExplicitlySet;
}

/*!
    Returns the default application font.

    \sa setFont()
*/
QFont QGuiApplication::font()
{
    QMutexLocker locker(&applicationFontMutex);
    initFontUnlocked();
    return *QGuiApplicationPrivate::app_font;
}

/*!
    Changes the default application font to \a font.

    \sa font()
*/
void QGuiApplication::setFont(const QFont &font)
{
    QMutexLocker locker(&applicationFontMutex);
    if (!QGuiApplicationPrivate::app_font)
        QGuiApplicationPrivate::app_font = new QFont(font);
    else
        *QGuiApplicationPrivate::app_font = font;
    applicationResourceFlags |= ApplicationFontExplicitlySet;
}

/*!
    \fn bool QGuiApplication::isRightToLeft()

    Returns true if the application's layout direction is
    Qt::RightToLeft; otherwise returns false.

    \sa layoutDirection(), isLeftToRight()
*/

/*!
    \fn bool QGuiApplication::isLeftToRight()

    Returns true if the application's layout direction is
    Qt::LeftToRight; otherwise returns false.

    \sa layoutDirection(), isRightToLeft()
*/

void QGuiApplicationPrivate::notifyLayoutDirectionChange()
{
}

void QGuiApplicationPrivate::notifyActiveWindowChange(QWindow *)
{
}


/*!
    \property QGuiApplication::quitOnLastWindowClosed

    \brief whether the application implicitly quits when the last window is
    closed.

    The default is true.

    If this property is true, the applications quits when the last visible
    primary window (i.e. window with no parent) is closed.

    \sa quit(), QWindow::close()
 */

void QGuiApplication::setQuitOnLastWindowClosed(bool quit)
{
    QCoreApplication::setQuitLockEnabled(quit);
}



bool QGuiApplication::quitOnLastWindowClosed()
{
    return QCoreApplication::isQuitLockEnabled();
}



void QGuiApplicationPrivate::emitLastWindowClosed()
{
    if (qGuiApp && qGuiApp->d_func()->in_exec) {
        emit qGuiApp->lastWindowClosed();
    }
}

bool QGuiApplicationPrivate::shouldQuit()
{
    /* if there is no visible top-level window left, we allow the quit */
    QWindowList list = QGuiApplication::topLevelWindows();
    for (int i = 0; i < list.size(); ++i) {
        QWindow *w = list.at(i);
        if (w->isVisible())
            return false;
    }
    return true;
}

/*!
    \property QGuiApplication::layoutDirection
    \brief the default layout direction for this application

    On system start-up, the default layout direction depends on the
    application's language.

    \sa QWidget::layoutDirection, isLeftToRight(), isRightToLeft()
 */

void QGuiApplication::setLayoutDirection(Qt::LayoutDirection direction)
{
    if (layout_direction == direction || direction == Qt::LayoutDirectionAuto)
        return;

    layout_direction = direction;

    QGuiApplicationPrivate::self->notifyLayoutDirectionChange();
}

Qt::LayoutDirection QGuiApplication::layoutDirection()
{
    return layout_direction;
}

/*!
    \fn QCursor *QGuiApplication::overrideCursor()

    Returns the active application override cursor.

    This function returns 0 if no application cursor has been defined (i.e. the
    internal cursor stack is empty).

    \sa setOverrideCursor(), restoreOverrideCursor()
*/
#ifndef QT_NO_CURSOR
QCursor *QGuiApplication::overrideCursor()
{
    return qGuiApp->d_func()->cursor_list.isEmpty() ? 0 : &qGuiApp->d_func()->cursor_list.first();
}

/*!
    Changes the currently active application override cursor to \a cursor.

    This function has no effect if setOverrideCursor() was not called.

    \sa setOverrideCursor(), overrideCursor(), restoreOverrideCursor(),
    QWidget::setCursor()
 */
void QGuiApplication::changeOverrideCursor(const QCursor &cursor)
{
    if (qGuiApp->d_func()->cursor_list.isEmpty())
        return;
    qGuiApp->d_func()->cursor_list.removeFirst();
    setOverrideCursor(cursor);
}
#endif


#ifndef QT_NO_CURSOR
static inline void applyCursor(QWindow *w, QCursor c)
{
    if (const QScreen *screen = w->screen())
        if (QPlatformCursor *cursor = screen->handle()->cursor())
            cursor->changeCursor(&c, w);
}

static inline void applyCursor(const QList<QWindow *> &l, const QCursor &c)
{
    for (int i = 0; i < l.size(); ++i) {
        QWindow *w = l.at(i);
        if (w->handle() && w->windowType() != Qt::Desktop)
            applyCursor(w, c);
    }
}

/*!
    \fn void QGuiApplication::setOverrideCursor(const QCursor &cursor)

    Sets the application override cursor to \a cursor.

    Application override cursors are intended for showing the user that the
    application is in a special state, for example during an operation that
    might take some time.

    This cursor will be displayed in all the application's widgets until
    restoreOverrideCursor() or another setOverrideCursor() is called.

    Application cursors are stored on an internal stack. setOverrideCursor()
    pushes the cursor onto the stack, and restoreOverrideCursor() pops the
    active cursor off the stack. changeOverrideCursor() changes the curently
    active application override cursor.

    Every setOverrideCursor() must eventually be followed by a corresponding
    restoreOverrideCursor(), otherwise the stack will never be emptied.

    Example:
    \snippet doc/src/snippets/code/src_gui_kernel_qapplication_x11.cpp 0

    \sa overrideCursor(), restoreOverrideCursor(), changeOverrideCursor(),
    QWidget::setCursor()
*/
void QGuiApplication::setOverrideCursor(const QCursor &cursor)
{
    qGuiApp->d_func()->cursor_list.prepend(cursor);
    applyCursor(QGuiApplicationPrivate::window_list, cursor);
}

/*!
    \fn void QGuiApplication::restoreOverrideCursor()

    Undoes the last setOverrideCursor().

    If setOverrideCursor() has been called twice, calling
    restoreOverrideCursor() will activate the first cursor set. Calling this
    function a second time restores the original widgets' cursors.

    \sa setOverrideCursor(), overrideCursor()
*/
void QGuiApplication::restoreOverrideCursor()
{
    if (qGuiApp->d_func()->cursor_list.isEmpty())
        return;
    qGuiApp->d_func()->cursor_list.removeFirst();
    QCursor c(qGuiApp->d_func()->cursor_list.value(0, QCursor()));
    applyCursor(QGuiApplicationPrivate::window_list, c);
}
#endif// QT_NO_CURSOR

/*!
  Returns the application's style hints.

  The style hints encapsulate a set of platform dependent properties
  such as double click intervals, full width selection and others.

  The hints can be used to integrate tighter with the underlying platform.

  \sa QStyleHints
  */
QStyleHints *QGuiApplication::styleHints() const
{
    Q_D(const QGuiApplication);
    if (!d->styleHints)
        const_cast<QGuiApplicationPrivate *>(d)->styleHints = new QStyleHints();
    return d->styleHints;
}

/*!
    Sets whether Qt should use the system's standard colors, fonts, etc., to
    \a on. By default, this is true.

    This function must be called before creating the QGuiApplication object, like
    this:

    \snippet doc/src/snippets/code/src_gui_kernel_qapplication.cpp 6

    \sa desktopSettingsAware()
*/
void QGuiApplication::setDesktopSettingsAware(bool on)
{
    QGuiApplicationPrivate::obey_desktop_settings = on;
}

/*!
    Returns true if Qt is set to use the system's standard colors, fonts, etc.;
    otherwise returns false. The default is true.

    \sa setDesktopSettingsAware()
*/
bool QGuiApplication::desktopSettingsAware()
{
    return QGuiApplicationPrivate::obey_desktop_settings;
}

QInputMethod *QGuiApplication::inputMethod() const
{
    Q_D(const QGuiApplication);
    if (!d->inputMethod)
        const_cast<QGuiApplicationPrivate *>(d)->inputMethod = new QInputMethod();
    return d->inputMethod;
}

/*!
  returns the input panel.

  The input panel returns properties about the state and position of
  the virtual keyboard. It also provides information about the position of the
  current focused input element.

  \sa QInputPanel
  */
QInputPanel *QGuiApplication::inputPanel() const
{
    return inputMethod();
}

/*!
    \fn void QGuiApplication::fontDatabaseChanged()

    This signal is emitted when application fonts are loaded or removed.

    \sa QFontDatabase::addApplicationFont(),
    QFontDatabase::addApplicationFontFromData(),
    QFontDatabase::removeAllApplicationFonts(),
    QFontDatabase::removeApplicationFont()
*/

// These pixmaps approximate the images in the Windows User Interface Guidelines.

// XPM

static const char * const move_xpm[] = {
"11 20 3 1",
".        c None",
"a        c #FFFFFF",
"X        c #000000", // X11 cursor is traditionally black
"aa.........",
"aXa........",
"aXXa.......",
"aXXXa......",
"aXXXXa.....",
"aXXXXXa....",
"aXXXXXXa...",
"aXXXXXXXa..",
"aXXXXXXXXa.",
"aXXXXXXXXXa",
"aXXXXXXaaaa",
"aXXXaXXa...",
"aXXaaXXa...",
"aXa..aXXa..",
"aa...aXXa..",
"a.....aXXa.",
"......aXXa.",
".......aXXa",
".......aXXa",
"........aa."};


/* XPM */
static const char * const copy_xpm[] = {
"24 30 3 1",
".        c None",
"a        c #000000",
"X        c #FFFFFF",
"XX......................",
"XaX.....................",
"XaaX....................",
"XaaaX...................",
"XaaaaX..................",
"XaaaaaX.................",
"XaaaaaaX................",
"XaaaaaaaX...............",
"XaaaaaaaaX..............",
"XaaaaaaaaaX.............",
"XaaaaaaXXXX.............",
"XaaaXaaX................",
"XaaXXaaX................",
"XaX..XaaX...............",
"XX...XaaX...............",
"X.....XaaX..............",
"......XaaX..............",
".......XaaX.............",
".......XaaX.............",
"........XX...aaaaaaaaaaa",
".............aXXXXXXXXXa",
".............aXXXXXXXXXa",
".............aXXXXaXXXXa",
".............aXXXXaXXXXa",
".............aXXaaaaaXXa",
".............aXXXXaXXXXa",
".............aXXXXaXXXXa",
".............aXXXXXXXXXa",
".............aXXXXXXXXXa",
".............aaaaaaaaaaa"};

/* XPM */
static const char * const link_xpm[] = {
"24 30 3 1",
".        c None",
"a        c #000000",
"X        c #FFFFFF",
"XX......................",
"XaX.....................",
"XaaX....................",
"XaaaX...................",
"XaaaaX..................",
"XaaaaaX.................",
"XaaaaaaX................",
"XaaaaaaaX...............",
"XaaaaaaaaX..............",
"XaaaaaaaaaX.............",
"XaaaaaaXXXX.............",
"XaaaXaaX................",
"XaaXXaaX................",
"XaX..XaaX...............",
"XX...XaaX...............",
"X.....XaaX..............",
"......XaaX..............",
".......XaaX.............",
".......XaaX.............",
"........XX...aaaaaaaaaaa",
".............aXXXXXXXXXa",
".............aXXXaaaaXXa",
".............aXXXXaaaXXa",
".............aXXXaaaaXXa",
".............aXXaaaXaXXa",
".............aXXaaXXXXXa",
".............aXXaXXXXXXa",
".............aXXXaXXXXXa",
".............aXXXXXXXXXa",
".............aaaaaaaaaaa"};

QPixmap QGuiApplicationPrivate::getPixmapCursor(Qt::CursorShape cshape)
{
    Q_UNUSED(cshape);
    return QPixmap();
}

void QGuiApplicationPrivate::notifyThemeChanged()
{
    if (!(applicationResourceFlags & ApplicationPaletteExplicitlySet)) {
        clearPalette();
        initPalette();
    }
    if (!(applicationResourceFlags & ApplicationFontExplicitlySet)) {
        QMutexLocker locker(&applicationFontMutex);
        clearFontUnlocked();
        initFontUnlocked();
    }
}

const QDrawHelperGammaTables *QGuiApplicationPrivate::gammaTables()
{
    QDrawHelperGammaTables *result = m_gammaTables.load();
    if (!result){
        QDrawHelperGammaTables *tables = new QDrawHelperGammaTables(fontSmoothingGamma);
        if (!m_gammaTables.testAndSetRelease(0, tables))
            delete tables;
        result = m_gammaTables.load();
    }
    return result;
}

void QGuiApplicationPrivate::q_updateFocusObject(QObject *object)
{
    Q_Q(QGuiApplication);

    bool enabled = false;
    if (object) {
        QInputMethodQueryEvent query(Qt::ImEnabled);
        QGuiApplication::sendEvent(object, &query);
        enabled = query.value(Qt::ImEnabled).toBool();
    }

    QPlatformInputContextPrivate::setInputMethodAccepted(enabled);
    QPlatformInputContext *inputContext = platformIntegration()->inputContext();
    if (inputContext)
        inputContext->setFocusObject(object);
    emit q->focusObjectChanged(object);
}

#include "moc_qguiapplication.cpp"

QT_END_NAMESPACE
