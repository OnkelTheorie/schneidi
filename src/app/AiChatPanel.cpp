// SPDX-License-Identifier: AGPL-3.0-only
#include "app/AiChatPanel.h"

#include "app/Theme.h"
#include "core/I18n.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QFormLayout>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

using Entry = AiChat::Entry;

QString tokens(qint64 n)
{
    if (n < 10000) return QLocale().toString(n);
    return QString::number(n / 1000.0, 'f', 1).replace('.', I18n::decimalPoint()) + " k";
}

QLabel* dimLabel(const QString& text = {})
{
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    return l;
}

// Enter sends, Shift+Enter is a new line; grows with the text up to a few lines
class ChatInput : public QPlainTextEdit {
public:
    std::function<void()> submit;
    ChatInput()
    {
        setTabChangesFocus(true);
        connect(this, &QPlainTextEdit::textChanged, this, [this] { fit(); });
        fit();
    }

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && !(e->modifiers() & Qt::ShiftModifier)) {
            if (submit) submit();
            return;
        }
        QPlainTextEdit::keyPressEvent(e);
    }
    void resizeEvent(QResizeEvent* e) override
    {
        QPlainTextEdit::resizeEvent(e);
        fit();
    }

private:
    void fit()
    {
        const int line = fontMetrics().lineSpacing();
        const int lines = std::clamp(int(document()->size().height()), 2, 7);
        setFixedHeight(lines * line + 2 * int(document()->documentMargin()) + frameWidth() * 2 + 4);
    }
};

// The picture bigger in a window of its own; a click or Escape closes it. Not a Qt::Popup: on X11 a popup grabs
// mouse and keyboard, and one opened from a mouse press here never let go (the whole desktop stopped answering).
class PictureWindow : public QLabel {
public:
    PictureWindow(const QImage& image, QWidget* parent) : QLabel(parent, Qt::Dialog)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(T("KI-Chat"));
        setToolTip({});
        const QScreen* s = parent->screen();
        const QRect area = s ? s->availableGeometry() : QRect(0, 0, 1280, 720);
        const QSize fit = image.size().boundedTo(area.size() * 0.8); // never larger than the picture itself
        setPixmap(QPixmap::fromImage(image.scaled(fit, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        adjustSize();
        QPoint at = QCursor::pos() - QPoint(width() / 2, height() / 2);
        at.setX(std::clamp(at.x(), area.left(), std::max(area.left(), area.right() - width())));
        at.setY(std::clamp(at.y(), area.top(), std::max(area.top(), area.bottom() - height())));
        move(at);
    }

protected:
    void mouseReleaseEvent(QMouseEvent*) override { close(); }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Escape) close();
        else QLabel::keyPressEvent(e);
    }
};

// A picture of a tool answer: a click shows it bigger
class Thumbnail : public QLabel {
public:
    explicit Thumbnail(const QImage& image) : m_image(image)
    {
        setPixmap(QPixmap::fromImage(image.scaled(132, 76, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        setCursor(Qt::PointingHandCursor);
        setToolTip(T("Klick: größer zeigen"));
        setStyleSheet(QString("border: 1px solid %1;").arg(Theme::border.name()));
    }

protected:
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || !rect().contains(e->position().toPoint())) return;
        auto* big = new PictureWindow(m_image, window());
        big->show();
        big->raise();
        big->activateWindow();
    }

private:
    QImage m_image;
};

// What the chat list holds: one widget per entry, updated when the entry changes
class EntryWidget : public QWidget {
public:
    virtual void set(const Entry& e) = 0;
};

// One entry of the chat (all kinds but plans)
class EntryView : public EntryWidget {
public:
    explicit EntryView(Entry::Kind kind)
    {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(3);
        if (kind == Entry::Kind::Tool) {
            m_head = new QPushButton;
            m_head->setFlat(true);
            m_head->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            m_head->setCursor(Qt::PointingHandCursor);
            setHeadColor(Theme::textDim);
            m_head->setFocusPolicy(Qt::NoFocus);
            lay->addWidget(m_head);
            m_details = new QLabel;
            m_details->setWordWrap(true);
            m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
            m_details->setTextFormat(Qt::PlainText);
            QFont mono("monospace");
            mono.setStyleHint(QFont::Monospace);
            mono.setPointSizeF(std::max(7.0, font().pointSizeF() - 1));
            m_details->setFont(mono);
            m_details->setStyleSheet(QString("QLabel { background: %1; color: %2; padding: 4px; }")
                                         .arg(Theme::well.name(), Theme::textDim.name()));
            m_details->hide();
            lay->addWidget(m_details);
            m_images = new QWidget;
            m_grid = new QGridLayout(m_images);
            m_grid->setContentsMargins(14, 0, 0, 0);
            m_grid->setSpacing(4);
            m_images->hide();
            lay->addWidget(m_images);
            QObject::connect(m_head, &QPushButton::clicked, m_head, [this] {
                m_open = !m_open;
                m_details->setVisible(m_open);
                updateHead();
            });
            return;
        }
        m_text = new QLabel;
        m_text->setWordWrap(true);
        m_text->setTextInteractionFlags(Qt::TextBrowserInteraction);
        m_text->setOpenExternalLinks(true);
        switch (kind) {
        case Entry::Kind::User:
            m_text->setTextFormat(Qt::PlainText);
            m_text->setStyleSheet(QString("QLabel { background: %1; color: %2; border-radius: 6px; padding: 6px 8px; }")
                                      .arg(Theme::control.name(), Theme::text.name()));
            break;
        case Entry::Kind::Assistant:
            m_text->setTextFormat(Qt::MarkdownText);
            m_text->setStyleSheet(QString("QLabel { color: %1; padding: 2px 2px; }").arg(Theme::text.name()));
            break;
        case Entry::Kind::Notice:
            m_text->setTextFormat(Qt::PlainText);
            m_text->setStyleSheet(QString("QLabel { color: %1; font-style: italic; }").arg(Theme::textDim.name()));
            break;
        case Entry::Kind::Error:
            m_text->setTextFormat(Qt::PlainText);
            m_text->setStyleSheet(QString("QLabel { color: %1; }").arg(Theme::warning.name()));
            break;
        case Entry::Kind::Tool:
        case Entry::Kind::Plan: break; // (PlanCard)
        }
        lay->addWidget(m_text);
    }

    void set(const Entry& e) override
    {
        m_entry = e;
        if (m_text) {
            m_text->setText(e.kind == Entry::Kind::Error ? "⚠ " + e.text : e.text);
            // An answer that turned out to be tool calls written as text (AiChat) is not shown; never a window of its
            // own (set() comes before the view is in the list)
            if (e.text.isEmpty()) hide();
            else if (parentWidget() && isHidden()) show();
            return;
        }
        // Long answers (a project's clip list …): the start; the model got its own cut version (AiChat::capped)
        m_details->setText(e.details.size() > 6000 ? e.details.left(6000) + "\n…" : e.details);
        updateHead();
        // New pictures only (the old ones stay)
        for (int i = m_shown; i < e.images.size(); ++i) m_grid->addWidget(new Thumbnail(e.images[i]), i / 3, i % 3);
        m_shown = int(e.images.size());
        m_images->setVisible(m_shown > 0);
    }

private:
    void updateHead()
    {
        QString state;
        switch (m_entry.state) {
        case Entry::State::Running:
            state = m_entry.percent >= 0 ? QString("… %1 %").arg(m_entry.percent) : QString("…");
            break;
        case Entry::State::Done: state = "✓"; break;
        case Entry::State::Failed: state = "✕"; break;
        case Entry::State::Stopped: state = "■"; break;
        }
        const QString arrow = m_open ? "▾" : "▸";
        const bool running = m_entry.state == Entry::State::Running;
        m_head->setText(QString("%1 %2  %3").arg(arrow, running ? QString() : state, m_entry.summary).simplified()
                        + (running ? "  " + state : QString()));
        m_head->setToolTip(m_entry.summary + "\n" + T("Klick: Aufruf und Antwort zeigen"));
        if (m_entry.state == Entry::State::Failed) setHeadColor(Theme::warning);
    }

    void setHeadColor(const QColor& c)
    {
        m_head->setStyleSheet(QString("QPushButton { text-align: left; color: %1; border: none; background: transparent; "
                                      "padding: 1px 2px; } QPushButton:hover { color: %2; }")
                                  .arg(c.name(), Theme::text.name()));
    }

    Entry m_entry;
    QLabel* m_text = nullptr;
    QPushButton* m_head = nullptr;
    QLabel* m_details = nullptr;
    QWidget* m_images = nullptr;
    QGridLayout* m_grid = nullptr;
    int m_shown = 0;
    bool m_open = false;
};

// A plan of the AI (plan mode): its steps with their reasons and what each changes, a tick per step, "Run" / "Discard".
// Nothing is changed until the user runs it (one undo step); after that the card shows what became of it.
class PlanCard : public EntryWidget {
public:
    std::function<void(const QString& id, const QVector<int>& steps)> run;
    std::function<void(const QString& id)> discard;

    PlanCard()
    {
        setObjectName("ai_plan");
        setAttribute(Qt::WA_StyledBackground);
        setStyleSheet(QString("#ai_plan { background: %1; border: 1px solid %2; border-radius: 4px; }")
                          .arg(Theme::mix(Theme::panel, Theme::primary, 0.12).name(), Theme::border.name()));
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(8, 6, 8, 6);
        lay->setSpacing(4);
        m_title = new QLabel;
        m_title->setObjectName("ai_plan_title");
        m_title->setStyleSheet("font-weight: 600;");
        m_title->setWordWrap(true);
        lay->addWidget(m_title);
        m_steps = new QVBoxLayout;
        m_steps->setSpacing(2);
        lay->addLayout(m_steps);
        m_state = new QLabel;
        m_state->setObjectName("ai_plan_state");
        m_state->setWordWrap(true);
        m_state->hide();
        lay->addWidget(m_state);
        auto* buttons = new QHBoxLayout;
        m_run = new QPushButton(T("Ausführen"));
        m_run->setObjectName("ai_plan_run");
        m_run->setToolTip(T("Die angehakten Schritte ausführen (ein Rückgängig-Schritt)"));
        m_discard = new QPushButton(T("Verwerfen"));
        m_discard->setObjectName("ai_plan_discard");
        m_discard->setToolTip(T("Den Plan verwerfen: nichts davon geschieht"));
        QObject::connect(m_run, &QPushButton::clicked, this, [this] {
            if (run) run(m_id, chosen());
        });
        QObject::connect(m_discard, &QPushButton::clicked, this, [this] {
            if (discard) discard(m_id);
        });
        buttons->addWidget(m_run);
        buttons->addWidget(m_discard);
        buttons->addStretch(1);
        lay->addLayout(buttons);
    }

    void set(const Entry& e) override
    {
        m_entry = e;
        if (m_id != e.plan["id"].toString()) build(e.plan);
        const bool open = e.state == Entry::State::Running;
        for (QCheckBox* box : std::as_const(m_boxes)) {
            box->setEnabled(open);
            if (e.state == Entry::State::Done) box->setChecked(e.ran.contains(box->property("step").toInt()));
        }
        QString state;
        QColor color = Theme::textDim;
        switch (e.state) {
        case Entry::State::Running: break;
        case Entry::State::Done: {
            QStringList ran;
            for (int n : e.ran) ran << QString::number(n);
            state = (ran.size() == 1 ? T("Ausgeführt: Schritt %1 (ein Rückgängig-Schritt).")
                                     : T("Ausgeführt: Schritte %1 (ein Rückgängig-Schritt)."))
                        .arg(ran.join(", "));
            break;
        }
        case Entry::State::Stopped: state = T("Verworfen: nichts davon wurde ausgeführt."); break;
        case Entry::State::Failed:
            state = e.text;
            color = Theme::warning;
            break;
        }
        m_state->setText(state);
        m_state->setStyleSheet(QString("QLabel { color: %1; font-style: italic; }").arg(color.name()));
        m_state->setVisible(!state.isEmpty());
        m_run->setVisible(open);
        m_discard->setVisible(open);
        updateButtons();
    }

    void setBusy(bool busy)
    {
        m_busy = busy;
        updateButtons();
    }

private:
    void build(const QJsonObject& plan)
    {
        m_id = plan["id"].toString();
        qDeleteAll(m_rows);
        m_rows.clear();
        m_boxes.clear();
        const QJsonArray steps = plan["steps"].toArray();
        m_title->setText(steps.size() == 1 ? T("Plan %1 · 1 Schritt").arg(m_id) : T("Plan %1 · %2 Schritte").arg(m_id).arg(steps.size()));
        m_title->setToolTip(T("Nichts ist bisher geändert. Hake an, was geschehen soll."));
        for (const QJsonValue& v : steps) {
            const QJsonObject s = v.toObject();
            const int n = s["n"].toInt();
            auto* row = new QWidget;
            auto* rl = new QVBoxLayout(row);
            rl->setContentsMargins(0, 0, 0, 0);
            rl->setSpacing(0);
            const QString why = s["why"].toString().trimmed();
            const QString op = s["op"].toObject()["op"].toString();
            auto* box = new QCheckBox(QString("%1. %2").arg(n).arg(why.isEmpty() ? op : why));
            box->setObjectName(QString("ai_plan_step_%1").arg(n));
            box->setProperty("step", n);
            box->setChecked(true);
            box->setToolTip(QString::fromUtf8(QJsonDocument(s["op"].toObject()).toJson(QJsonDocument::Indented)));
            QObject::connect(box, &QCheckBox::toggled, this, [this] { updateButtons(); });
            rl->addWidget(box);
            // What it changes, in the user's words (the op's name stays: it is what the tool line shows)
            const QJsonObject c = s["changes"].toObject();
            const QString counts = T("%1: %2 neu, %3 geändert, %4 entfernt")
                                       .arg(op)
                                       .arg(c["added"].toArray().size())
                                       .arg(c["changed"].toArray().size())
                                       .arg(c["removed"].toArray().size());
            QLabel* detail = dimLabel(counts);
            detail->setContentsMargins(22, 0, 0, 0);
            rl->addWidget(detail);
            m_steps->addWidget(row);
            m_rows << row;
            m_boxes << box;
        }
    }

    QVector<int> chosen() const
    {
        QVector<int> out;
        for (const QCheckBox* box : m_boxes)
            if (box->isChecked()) out << box->property("step").toInt();
        return out;
    }

    void updateButtons()
    {
        const bool open = m_entry.state == Entry::State::Running;
        m_run->setEnabled(open && !m_busy && !chosen().isEmpty());
        m_discard->setEnabled(open && !m_busy);
    }

    Entry m_entry;
    QString m_id;
    QLabel* m_title = nullptr;
    QVBoxLayout* m_steps = nullptr;
    QLabel* m_state = nullptr;
    QPushButton* m_run = nullptr;
    QPushButton* m_discard = nullptr;
    QVector<QWidget*> m_rows;
    QVector<QCheckBox*> m_boxes;
    bool m_busy = false;
};

} // namespace

AiChatPanel::AiChatPanel(AiChat* chat, QWidget* parent) : QWidget(parent), m_chat(chat)
{
    setObjectName("Panel"); // the panels' background
    setMinimumWidth(260);
    m_pages = new QStackedWidget;
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Header like the other panels: title, provider and model, menu
    auto* header = new QWidget;
    header->setObjectName("ai_chat_header");
    header->setAttribute(Qt::WA_StyledBackground);
    header->setStyleSheet(QString("#ai_chat_header { background: %1; }").arg(Theme::panelHeader.name()));
    auto* head = new QHBoxLayout(header);
    head->setContentsMargins(0, 0, 4, 0);
    head->setSpacing(4);
    auto* title = new QLabel(T("KI"));
    title->setObjectName("PanelTitle");
    m_provider = dimLabel();
    m_provider->setWordWrap(false);
    m_provider->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_menuButton = new QToolButton;
    m_menuButton->setText("⋯");
    m_menuButton->setAutoRaise(true);
    m_menuButton->setPopupMode(QToolButton::InstantPopup);
    m_menuButton->setToolTip(T("Mehr"));
    auto* menu = new QMenu(m_menuButton);
    menu->addAction(T("Neuer Chat"), this, [this] { m_chat->clear(); });
    menu->addAction(T("Chat kopieren"), this, [this] { QApplication::clipboard()->setText(m_chat->transcript()); });
    menu->addSeparator();
    m_left = menu->addAction(T("Links andocken"), this, [this] { emit sideRequested(true); });
    m_right = menu->addAction(T("Rechts andocken"), this, [this] { emit sideRequested(false); });
    m_left->setCheckable(true);
    m_right->setCheckable(true);
    m_right->setChecked(true);
    menu->addSeparator();
    menu->addAction(T("Einstellungen…"), this, &AiChatPanel::settingsRequested);
    menu->addAction(T("Ausblenden"), this, &AiChatPanel::hideRequested);
    m_menuButton->setMenu(menu);
    head->addWidget(title);
    head->addWidget(m_provider, 1);
    head->addWidget(m_menuButton);
    outer->addWidget(header);
    outer->addWidget(m_pages, 1);

    // ---- Chat page ----
    auto* page = new QWidget;
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(6);

    m_scroll = new QScrollArea;
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_list = new QWidget;
    m_list->setObjectName("ai_chat_list");
    m_listLayout = new QVBoxLayout(m_list);
    m_listLayout->setContentsMargins(2, 2, 6, 2);
    m_listLayout->setSpacing(8);
    m_empty = dimLabel(T("Sag der KI, was sie tun soll – zum Beispiel „Entferne alle Pausen länger als eine Sekunde“ "
                         "oder „Mach daraus ein 30-Sekunden-Video im Hochformat“. Sie arbeitet direkt in deinem "
                         "offenen Projekt; jede Änderung kannst du rückgängig machen."));
    m_listLayout->addWidget(m_empty);
    m_working = new QLabel(T("Die KI arbeitet …"));
    m_working->setObjectName("ai_working");
    m_working->setWordWrap(true);
    m_working->setStyleSheet(QString("QLabel { color: %1; font-style: italic; }").arg(Theme::textDim.name()));
    m_working->hide();
    m_listLayout->addWidget(m_working);
    m_listLayout->addStretch(1);
    m_scroll->setWidget(m_list);
    lay->addWidget(m_scroll, 1);
    // Stays at the newest message unless the user scrolled up
    QScrollBar* bar = m_scroll->verticalScrollBar();
    connect(bar, &QScrollBar::valueChanged, this, [this, bar](int v) { m_follow = v >= bar->maximum() - 4; });
    connect(bar, &QScrollBar::rangeChanged, this, [this, bar](int, int max) {
        if (m_follow) bar->setValue(max);
    });

    // Review mode: accept or discard what the request changed
    m_approval = new QWidget;
    m_approval->setObjectName("ai_approval");
    m_approval->setAttribute(Qt::WA_StyledBackground);
    m_approval->setStyleSheet(QString("#ai_approval { background: %1; border: 1px solid %2; border-radius: 4px; }")
                                  .arg(Theme::mix(Theme::panel, Theme::primary, 0.18).name(), Theme::border.name()));
    auto* al = new QVBoxLayout(m_approval);
    al->setContentsMargins(8, 6, 8, 6);
    m_approvalText = new QLabel;
    m_approvalText->setWordWrap(true);
    al->addWidget(m_approvalText);
    auto* ab = new QHBoxLayout;
    auto* accept = new QPushButton(T("Übernehmen"));
    accept->setObjectName("ai_accept");
    accept->setToolTip(T("Alle Änderungen dieser Anfrage behalten (ein Rückgängig-Schritt)"));
    auto* discard = new QPushButton(T("Verwerfen"));
    discard->setObjectName("ai_discard");
    discard->setToolTip(T("Alle Änderungen dieser Anfrage zurücknehmen"));
    connect(accept, &QPushButton::clicked, this, &AiChatPanel::approved);
    connect(discard, &QPushButton::clicked, this, &AiChatPanel::discarded);
    ab->addWidget(accept);
    ab->addWidget(discard);
    ab->addStretch(1);
    al->addLayout(ab);
    m_approval->hide();
    lay->addWidget(m_approval);

    auto* inputRow = new QHBoxLayout;
    auto* input = new ChatInput;
    input->submit = [this] { submit(); };
    m_input = input;
    m_input->setObjectName("ai_input");
    m_input->setPlaceholderText(T("Was soll die KI tun? (Enter sendet, Umschalt+Enter: neue Zeile)"));
    m_sendButton = new QToolButton;
    m_sendButton->setObjectName("ai_send");
    m_sendButton->setFocusPolicy(Qt::NoFocus);
    connect(m_sendButton, &QToolButton::clicked, this, [this] {
        if (m_chat->busy()) m_chat->stop();
        else submit();
    });
    inputRow->addWidget(m_input, 1);
    inputRow->addWidget(m_sendButton, 0, Qt::AlignBottom);
    lay->addLayout(inputRow);

    auto* foot = new QHBoxLayout;
    auto* newChat = new QToolButton;
    newChat->setText(T("Neuer Chat"));
    newChat->setAutoRaise(true);
    connect(newChat, &QToolButton::clicked, this, [this] { m_chat->clear(); });
    auto* copy = new QToolButton;
    copy->setText(T("Kopieren"));
    copy->setAutoRaise(true);
    copy->setToolTip(T("Den ganzen Chat als Text kopieren"));
    connect(copy, &QToolButton::clicked, this, [this] { QApplication::clipboard()->setText(m_chat->transcript()); });
    m_usage = dimLabel();
    m_usage->setObjectName("ai_usage");
    m_usage->setWordWrap(false);
    m_usage->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    foot->addWidget(newChat);
    foot->addWidget(copy);
    foot->addStretch(1);
    foot->addWidget(m_usage);
    lay->addLayout(foot);
    m_pages->addWidget(page);
    m_pages->addWidget(buildSetup());

    m_flush = new QTimer(this);
    m_flush->setSingleShot(true);
    m_flush->setInterval(50);
    connect(m_flush, &QTimer::timeout, this, &AiChatPanel::flush);
    m_tick = new QTimer(this);
    m_tick->setInterval(1000);
    connect(m_tick, &QTimer::timeout, this, &AiChatPanel::updateWorking);
    connect(m_chat, &AiChat::entryAdded, this, &AiChatPanel::addEntry);
    connect(m_chat, &AiChat::entryChanged, this, [this](int i) {
        if (!m_dirty.contains(i)) m_dirty << i;
        if (!m_flush->isActive()) m_flush->start();
    });
    connect(m_chat, &AiChat::cleared, this, &AiChatPanel::rebuild);
    connect(m_chat, &AiChat::busyChanged, this, &AiChatPanel::updateBusy);
    connect(m_chat, &AiChat::usageChanged, this, &AiChatPanel::updateUsage);
    rebuild();
    updateBusy(m_chat->busy());
    updateUsage();
}

// ---------- Chat page ----------

void AiChatPanel::submit()
{
    const QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty() || m_chat->busy() || approvalShown() || !m_send) return;
    m_follow = true;
    if (m_send(text)) m_input->clear();
}

void AiChatPanel::sendText(const QString& text)
{
    m_input->setPlainText(text);
    submit();
}

void AiChatPanel::addEntry(int index)
{
    flush();
    const Entry& e = m_chat->entries().value(index);
    EntryWidget* view = nullptr;
    if (e.kind == Entry::Kind::Plan) {
        auto* card = new PlanCard;
        card->run = [this](const QString& id, const QVector<int>& steps) { emit planRun(id, steps); };
        card->discard = [this](const QString& id) { emit planDiscarded(id); };
        card->setBusy(m_chat->busy());
        view = card;
    } else {
        view = new EntryView(e.kind);
    }
    view->set(e);
    m_listLayout->insertWidget(m_listLayout->indexOf(m_working), view); // above the working line and the stretch
    m_widgets << view;
    updateEmpty();
    updateWorking();
    if (e.kind == Entry::Kind::User) scrollDown(true);
}

void AiChatPanel::updateEntry(int index)
{
    if (index < 0 || index >= m_widgets.size() || index >= m_chat->entries().size()) return;
    static_cast<EntryWidget*>(m_widgets[index])->set(m_chat->entries()[index]);
}

void AiChatPanel::flush()
{
    const QVector<int> dirty = m_dirty;
    m_dirty.clear();
    for (int i : dirty) updateEntry(i);
    if (!dirty.isEmpty()) updateWorking();
}

void AiChatPanel::updateWorking()
{
    // Thinking, waiting for the provider, starting the tools: nothing in the list moves meanwhile
    bool shown = m_chat->busy();
    if (shown && !m_chat->entries().isEmpty()) {
        const Entry& last = m_chat->entries().last();
        shown = last.state != Entry::State::Running; // a running tool or streaming text shows it itself
    }
    m_working->setVisible(shown);
    // The model is silent: for how long; a local one may first read ~15,000 tokens for minutes on a weak computer
    QString text = T("Die KI arbeitet …");
    const qint64 ms = m_chat->waitingMs();
    if (ms >= 5000) text += QString(" %1 s").arg(ms / 1000);
    if (ms >= m_slowHintMs && m_chat->localModel())
        text += "\n" + T("Ein Modell auf diesem Rechner liest zuerst die ganze Anfrage (etwa %1 Token, mit den Werkzeugen "
                         "und der Anleitung) – auf schwacher Hardware dauert das Minuten.")
                           .arg(m_chat->requestTokens());
    m_working->setText(text);
}

QString AiChatPanel::workingText() const { return m_working->isVisible() ? m_working->text() : QString(); }

void AiChatPanel::rebuild()
{
    m_dirty.clear();
    qDeleteAll(m_widgets);
    m_widgets.clear();
    for (int i = 0; i < m_chat->entries().size(); ++i) addEntry(i);
    updateEmpty();
    updateUsage();
}

void AiChatPanel::updateEmpty() { m_empty->setVisible(m_widgets.isEmpty()); }

void AiChatPanel::scrollDown(bool force)
{
    if (force) m_follow = true;
    QScrollBar* bar = m_scroll->verticalScrollBar();
    if (m_follow) bar->setValue(bar->maximum());
}

void AiChatPanel::updateBusy(bool busy)
{
    if (busy) m_tick->start();
    else m_tick->stop();
    for (QWidget* w : std::as_const(m_widgets)) // a plan runs only while the AI is not working
        if (auto* card = dynamic_cast<PlanCard*>(w)) card->setBusy(busy);
    updateWorking();
    m_sendButton->setText(busy ? T("Stopp") : T("Senden"));
    m_sendButton->setToolTip(busy ? T("Die KI anhalten") : T("Senden (Enter)"));
}

void AiChatPanel::updateUsage()
{
    const Ai::Usage u = m_chat->usage();
    m_usage->setText(u.input || u.output ? T("%1 Token").arg(tokens(u.input + u.output)) : QString());
    m_usage->setToolTip(T("Token dieses Chats (danach rechnet der Anbieter ab): %1 gesendet, davon %2 aus seinem "
                          "Zwischenspeicher (günstiger), %3 erhalten")
                            .arg(tokens(u.input), tokens(u.cached), tokens(u.output)));
}

void AiChatPanel::setProviderText(const QString& text, const QString& tip)
{
    m_providerText = text;
    m_provider->setToolTip(tip);
    elideProvider();
}

void AiChatPanel::elideProvider()
{
    m_provider->setText(
        m_provider->fontMetrics().elidedText(m_providerText, Qt::ElideMiddle, std::max(40, m_provider->width())));
}

void AiChatPanel::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    elideProvider();
}

void AiChatPanel::setDockedLeft(bool left)
{
    m_left->setChecked(left);
    m_right->setChecked(!left);
}

void AiChatPanel::showApproval(const QString& text)
{
    m_approvalText->setText(text);
    m_approval->show();
    m_input->setEnabled(false);
    m_sendButton->setEnabled(false);
}

void AiChatPanel::hideApproval()
{
    m_approval->hide();
    m_input->setEnabled(true);
    m_sendButton->setEnabled(true);
}

bool AiChatPanel::approvalShown() const { return !m_approval->isHidden(); }

// ---------- Setup page ----------

QWidget* AiChatPanel::buildSetup()
{
    auto* page = new QWidget;
    page->setObjectName("ai_setup");
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);
    auto* title = new QLabel(T("KI-Chat einrichten"));
    title->setStyleSheet("font-weight: 600;");
    lay->addWidget(title);
    lay->addWidget(dimLabel(T("Die KI bearbeitet dein offenes Projekt: schneiden, Pausen entfernen, Titel, Ton, Farbe … "
                              "Jede Änderung kannst du rückgängig machen. Wähle, mit welcher KI du arbeiten willst.")));
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapAllRows); // labels above the fields: the panel is narrow
    m_setupProvider = new QComboBox;
    m_setupProvider->setObjectName("ai_setup_provider");
    for (const Ai::Preset& p : Ai::presets()) m_setupProvider->addItem(T(p.name), p.id);
    form->addRow(T("Anbieter"), m_setupProvider);
    m_setupAddress = new QLineEdit;
    m_setupAddress->setObjectName("ai_setup_address");
    m_setupAddress->setPlaceholderText("http://server:8000/v1");
    form->addRow(T("Adresse"), m_setupAddress);
    m_setupForm = form;
    m_setupKey = new QLineEdit;
    m_setupKey->setObjectName("ai_setup_key");
    m_setupKey->setEchoMode(QLineEdit::Password);
    form->addRow(T("Schlüssel"), m_setupKey);
    m_setupKeyHint = dimLabel();
    form->addRow(m_setupKeyHint);
    m_setupModel = new QComboBox;
    m_setupModel->setObjectName("ai_setup_model");
    m_setupModel->setEditable(true);
    m_setupModel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    form->addRow(T("Modell"), m_setupModel);
    lay->addLayout(form);
    auto* start = new QPushButton(T("Chat starten"));
    start->setObjectName("ai_setup_start");
    start->setDefault(true);
    connect(start, &QPushButton::clicked, this, [this] {
        const QString provider = m_setupProvider->currentData().toString();
        const Ai::Preset* p = Ai::preset(provider);
        const QString key = m_setupKey->text().trimmed();
        const bool keyElsewhere = m_keyHint && !m_keyHint(provider).isEmpty();
        if (p && p->needsKey && key.isEmpty() && !keyElsewhere) {
            m_setupMessage->setText(T("Bitte den API-Schlüssel eingeben."));
            m_setupKey->setFocus();
            return;
        }
        if (provider == "custom" && m_setupAddress->text().trimmed().isEmpty()) {
            m_setupMessage->setText(T("Bitte die Adresse des Servers angeben."));
            m_setupAddress->setFocus();
            return;
        }
        if (m_setupModel->currentText().trimmed().isEmpty()) {
            m_setupMessage->setText(T("Bitte ein Modell angeben."));
            return;
        }
        m_setupMessage->clear();
        emit setupDone({provider, key, m_setupModel->currentText().trimmed(),
                        provider == "custom" ? m_setupAddress->text().trimmed() : QString()});
        m_setupKey->clear();
    });
    auto* row = new QHBoxLayout;
    row->addWidget(start);
    row->addStretch(1);
    lay->addLayout(row);
    m_setupMessage = new QLabel;
    m_setupMessage->setWordWrap(true);
    m_setupMessage->setStyleSheet(QString("color: %1;").arg(Theme::warning.name()));
    lay->addWidget(m_setupMessage);
    auto* more = new QLabel(QString("<a href=\"settings\">%1</a>").arg(T("Mehr in Einstellungen › KI …").toHtmlEscaped()));
    more->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    connect(more, &QLabel::linkActivated, this, &AiChatPanel::settingsRequested);
    lay->addWidget(more);
    lay->addWidget(dimLabel(T("Ollama läuft ohne Schlüssel auf deinem Rechner. Andere Adressen (eigener Server, Firma) "
                              "stellst du in den Einstellungen ein.")));
    lay->addStretch(1);
    connect(m_setupProvider, &QComboBox::currentIndexChanged, this, &AiChatPanel::setupProviderChanged);
    setupProviderChanged();
    return page;
}

void AiChatPanel::setupProviderChanged()
{
    const QString id = m_setupProvider->currentData().toString();
    const Ai::Preset* p = Ai::preset(id);
    m_setupModel->clear();
    if (p && !p->model.isEmpty()) m_setupModel->addItem(p->model);
    m_setupModel->setCurrentText(p ? p->model : QString());
    m_setupForm->setRowVisible(m_setupAddress, id == "custom");
    const QString hint = m_keyHint ? m_keyHint(id) : QString();
    m_setupKey->setEnabled(!p || p->needsKey || !hint.isEmpty() || id == "custom");
    m_setupKey->setPlaceholderText(hint.isEmpty() ? (p && !p->needsKey ? T("nicht nötig") : QString()) : T("schon vorhanden"));
    if (!hint.isEmpty()) m_setupKeyHint->setText(hint);
    else if (p && !p->needsKey) m_setupKeyHint->setText(T("Kein Schlüssel nötig."));
    else
        m_setupKeyHint->setText(T("Den API-Schlüssel bekommst du auf der Website des Anbieters. Oder setze die "
                                  "Umgebungsvariable %1.")
                                    .arg(p && !p->keyEnv.isEmpty() ? p->keyEnv : QString("SCHNEIDI_AI_KEY")));
}

void AiChatPanel::showSetup(bool on, const QString& provider, const QString& message)
{
    if (on) {
        if (const int i = m_setupProvider->findData(provider); i >= 0) m_setupProvider->setCurrentIndex(i);
        setupProviderChanged();
        m_setupMessage->setText(message);
    }
    m_pages->setCurrentIndex(on ? 1 : 0);
}

bool AiChatPanel::setupShown() const { return m_pages->currentIndex() == 1; }
