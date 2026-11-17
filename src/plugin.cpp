#include "liveeditor.h"

#include <KActionCollection>
#include <KLocalizedString>
#include <KPluginFactory>
#include <KTextEditor/MainWindow>
#include <KTextEditor/Message>
#include <KTextEditor/Plugin>
#include <KXMLGUIClient>
#include <KXMLGUIFactory>
#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QHash>
#include <QSet>
#include <QTimer>

using MarkdownLive::LiveEditor;

class MarkdownPluginView : public QObject, public KXMLGUIClient
{
    Q_OBJECT
public:
    explicit MarkdownPluginView(KTextEditor::MainWindow *window)
        : QObject(window), m_window(window)
    {
        setComponentName(QStringLiteral("katemarkdownlive"), i18n("Markdown RTR"));
        setXMLFile(QStringLiteral(":/markdownlive/ui.rc"));
        m_toggle = actionCollection()->addAction(QStringLiteral("markdown_live_toggle"));
        m_toggle->setText(i18n("Markdown Live Editing"));
        m_toggle->setIcon(QIcon::fromTheme(QStringLiteral("text-markdown")));
        m_toggle->setCheckable(true);
        KActionCollection::setDefaultShortcut(m_toggle, QKeySequence(QStringLiteral("Ctrl+Shift+M")));
        connect(m_toggle, &QAction::triggered, this, &MarkdownPluginView::toggle);
        m_remoteImages = actionCollection()->addAction(QStringLiteral("markdown_live_remote_images"));
        m_remoteImages->setText(i18n("Load Remote Images in Markdown"));
        m_remoteImages->setCheckable(true);
        connect(m_remoteImages, &QAction::toggled, this, [this](bool enabled) {
            for (auto editor : std::as_const(m_editors)) if (editor) editor->setRemoteImagesEnabled(enabled);
        });
        connect(window, &KTextEditor::MainWindow::viewChanged, this, [this] {
            schedule(m_window->activeView());
            updateAction();
        });
        connect(window, &KTextEditor::MainWindow::viewCreated, this, &MarkdownPluginView::hook);
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *focus) {
            const auto view = m_window->activeView();
            const auto editor = m_editors.value(view);
            if (!focus || !view || !editor || editor->isHidden() || editor == focus || editor->isAncestorOf(focus)) return;
            if (focus != view && !view->isAncestorOf(focus)) return;
            // Kate can restore focus to its internal editor after the overlay has
            // opened. Do not leave keyboard input routed to the covered source view.
            QTimer::singleShot(0, this, [this, editor, target = QPointer<QWidget>(focus)] {
                if (editor && target && !editor->isHidden() && m_window->activeView() == editor->sourceView()
                    && QApplication::focusWidget() == target) editor->setFocus();
            });
        });
        for (auto view : window->views()) hook(view);
        if (window->guiFactory()) window->guiFactory()->addClient(this);
        updateAction();
    }

    ~MarkdownPluginView() override
    {
        if (m_window->guiFactory()) m_window->guiFactory()->removeClient(this);
        const auto editors = m_editors.values();
        m_editors.clear();
        for (auto editor : editors) {
            if (!editor) continue;
            auto view = editor->sourceView();
            const bool focused = editor->hasFocus() || editor->isAncestorOf(QApplication::focusWidget());
            delete editor;
            if (focused && view) view->setFocus();
        }
    }

private:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        auto view = qobject_cast<KTextEditor::View *>(object);
        auto editor = m_editors.value(view);
        if (editor && event->type() == QEvent::Resize) editor->setGeometry(view->rect());
        return QObject::eventFilter(object, event);
    }

    void hook(KTextEditor::View *view)
    {
        if (m_hooked.contains(view)) return;
        m_hooked.insert(view);
        connect(view, &QObject::destroyed, this, [this, view] {
            m_editors.remove(view); m_choices.remove(view); m_pending.remove(view); m_hooked.remove(view);
        });
        connect(view->document(), &KTextEditor::Document::documentUrlChanged, this, [this, view] { schedule(view); });
        connect(view->document(), &KTextEditor::Document::highlightingModeChanged, this, [this, view] { schedule(view); });
        // Native search UI lives inside the original view; reveal it for these actions.
        for (const char *name : {"edit_find", "edit_replace", "go_goto_line"}) {
            if (auto action = view->actionCollection()->action(QString::fromLatin1(name)))
                connect(action, &QAction::triggered, this, [this, view, action] {
                    auto editor = m_editors.value(view);
                    if (!editor || editor->isHidden()) return;
                    disable(view);
                    QTimer::singleShot(0, action, &QAction::trigger);
                });
        }
        schedule(view);
    }

    void schedule(KTextEditor::View *view)
    {
        if (!view || m_pending.contains(view)) return;
        m_pending.insert(view);
        // Kate may still be restoring a tab or detecting its document type.
        QTimer::singleShot(0, this, [this, guard = QPointer<KTextEditor::View>(view)] {
            if (!guard) return;
            auto view = guard.data();
            m_pending.remove(view);
            if (markdown(view->document()) && m_choices.value(view, true)) enable(view, false);
            else disable(view, false);
            updateAction();
        });
    }

    static bool markdown(KTextEditor::Document *document)
    {
        const auto name = document->url().fileName().toLower();
        return document->highlightingMode().contains(QStringLiteral("markdown"), Qt::CaseInsensitive)
            || document->mimeType() == QStringLiteral("text/markdown")
            || name.endsWith(QStringLiteral(".md")) || name.endsWith(QStringLiteral(".markdown"));
    }

    void updateAction()
    {
        const auto view = m_window->activeView();
        m_toggle->setEnabled(view && markdown(view->document()));
        const auto editor = m_editors.value(view);
        m_toggle->setChecked(editor && !editor->isHidden());
    }

    void disable(KTextEditor::View *view, bool focus = true)
    {
        if (const auto editor = m_editors.value(view)) {
            const bool focused = editor->hasFocus() || editor->isAncestorOf(QApplication::focusWidget());
            editor->finishEditing();
            editor->hide();
            if ((focus || focused) && m_window->activeView() == view) view->setFocus();
        }
        updateAction();
    }

    void toggle(bool enabled)
    {
        const auto view = m_window->activeView();
        if (!view) return;
        m_choices[view] = enabled;
        if (!enabled) { disable(view); return; }
        enable(view, true);
    }

    void enable(KTextEditor::View *view, bool focus)
    {
        auto editor = m_editors.value(view);
        if (editor && !editor->isHidden()) return;
        const bool focused = view->hasFocus() || view->isAncestorOf(QApplication::focusWidget());
        if (!editor) {
            // A child overlay keeps Kate's real document tab, save actions, split
            // ownership, modified flag and close confirmation fully intact.
            editor = new LiveEditor(view, view);
            editor->setRemoteImagesEnabled(m_remoteImages->isChecked());
            m_editors.insert(view, editor);
            view->installEventFilter(this);
            connect(editor, &LiveEditor::limitReached, this, [this, view] {
                m_choices[view] = false;
                disable(view);
                auto message = new KTextEditor::Message(
                    i18n("This document exceeds the live rendering limit. Continue editing in Kate's source view."),
                    KTextEditor::Message::Information);
                message->setAutoHide(7000);
                message->setView(view);
                view->document()->postMessage(message);
            });
        }
        editor->setGeometry(view->rect());
        editor->show();
        editor->raise();
        editor->reloadResources();
        if (editor->blockCount() == 0) { m_choices[view] = false; disable(view, false); return; }
        if ((focus || focused) && m_window->activeView() == view) editor->setFocus();
        updateAction();
    }

    KTextEditor::MainWindow *m_window;
    QAction *m_toggle;
    QAction *m_remoteImages;
    QHash<KTextEditor::View *, QPointer<LiveEditor>> m_editors;
    QHash<KTextEditor::View *, bool> m_choices;
    QSet<KTextEditor::View *> m_pending, m_hooked;
};

class MarkdownPlugin : public KTextEditor::Plugin
{
    Q_OBJECT
public:
    explicit MarkdownPlugin(QObject *parent) : KTextEditor::Plugin(parent) {}
    QObject *createView(KTextEditor::MainWindow *window) override { return new MarkdownPluginView(window); }
};

K_PLUGIN_FACTORY_WITH_JSON(MarkdownPluginFactory, "plugin.json", registerPlugin<MarkdownPlugin>();)
#include "plugin.moc"
