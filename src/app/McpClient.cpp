#include "app/McpClient.h"

#include "core/I18n.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

McpClient::McpClient(QObject* parent) : QObject(parent) {}

McpClient::~McpClient()
{
    m_pending.clear(); // nobody to answer anymore
    stop();
}

void McpClient::setCommand(const QStringList& command, const QHash<QString, QString>& environment)
{
    m_command = command;
    m_environment = environment;
}

void McpClient::send(const QJsonObject& message)
{
    if (m_process) m_process->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

int McpClient::request(const QString& method, const QJsonObject& params,
                       const std::function<void(const QJsonObject&)>& reply)
{
    const int id = m_next++;
    m_pending.insert(id, {reply, {}, {}});
    send({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
    return id;
}

void McpClient::start()
{
    if (m_ready || m_starting) return;
    if (m_command.isEmpty()) {
        emit failed(T("Die Werkzeuge der KI (schneidi-cli) sind nicht eingerichtet."));
        return;
    }
    m_starting = true;
    m_errors.clear();
    m_buffer.clear();
    m_process = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (auto it = m_environment.begin(); it != m_environment.end(); ++it) env.insert(it.key(), it.value());
    m_process->setProcessEnvironment(env);
#ifdef Q_OS_WIN
    // schneidi-cli is a console program: no console window popping up next to the app
    m_process->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* a) { a->flags |= CREATE_NO_WINDOW; });
#endif
    connect(m_process, &QProcess::readyReadStandardOutput, this, &McpClient::onOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        if (!m_process) return;
        m_errors += m_process->readAllStandardError();
        if (m_errors.size() > 4096) m_errors = m_errors.right(4096);
    });
    connect(m_process, &QProcess::finished, this, &McpClient::onEnded);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) onEnded();
    });
    m_process->start(m_command.first(), m_command.mid(1));
    const QJsonObject init{{"protocolVersion", "2025-06-18"},
                           {"capabilities", QJsonObject{}},
                           {"clientInfo", QJsonObject{{"name", "schneidi app chat"},
                                                      {"version", QCoreApplication::applicationVersion()}}}};
    request("initialize", init, [this](const QJsonObject& r) {
        if (r.contains("error")) {
            m_starting = false;
            emit failed(T("Die Werkzeuge der KI antworten nicht: %1").arg(r["error"]["message"].toString()));
            return;
        }
        m_instructions = r["result"]["instructions"].toString();
        send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
        request("tools/list", {}, [this](const QJsonObject& list) {
            m_tools = list["result"]["tools"].toArray();
            m_starting = false;
            m_ready = true;
            emit ready();
        });
    });
}

void McpClient::call(const QString& tool, const QJsonObject& arguments, const Progress& progress, const Done& done)
{
    if (!m_ready) {
        done({T("Die Werkzeuge der KI laufen nicht."), {}, true});
        return;
    }
    const int id = m_next++;
    Pending p;
    p.progress = progress;
    p.done = done;
    p.reply = [done](const QJsonObject& r) {
        Result out;
        if (r.contains("error")) {
            out.text = r["error"]["message"].toString();
            out.error = true;
            done(out);
            return;
        }
        const QJsonObject result = r["result"].toObject();
        out.error = result["isError"].toBool();
        QStringList texts;
        for (const QJsonValue& v : result["content"].toArray()) {
            const QJsonObject c = v.toObject();
            if (c["type"].toString() == "text") texts << c["text"].toString();
            else if (c["type"].toString() == "image")
                out.images << Ai::Image{c["data"].toString().toLatin1(), c["mimeType"].toString("image/jpeg")};
        }
        out.text = texts.join('\n');
        done(out);
    };
    m_pending.insert(id, p);
    send({{"jsonrpc", "2.0"},
          {"id", id},
          {"method", "tools/call"},
          {"params", QJsonObject{{"name", tool}, {"arguments", arguments}, {"_meta", QJsonObject{{"progressToken", id}}}}}});
}

void McpClient::onOutput()
{
    if (!m_process) return;
    m_buffer += m_process->readAllStandardOutput();
    while (true) {
        const qsizetype nl = m_buffer.indexOf('\n');
        if (nl < 0) break;
        const QByteArray line = m_buffer.left(nl).trimmed();
        m_buffer.remove(0, nl + 1);
        if (line.isEmpty()) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject()) onMessage(doc.object());
        if (!m_process) return; // a callback stopped it
    }
}

void McpClient::onMessage(const QJsonObject& msg)
{
    const QString method = msg["method"].toString();
    if (method == "notifications/progress") {
        const QJsonObject p = msg["params"].toObject();
        const auto it = m_pending.find(p["progressToken"].toInt());
        if (it == m_pending.end() || !it->progress) return;
        const double total = p["total"].toDouble();
        const double done = p["progress"].toDouble();
        it->progress(int(total > 0 ? done * 100 / total : done));
        return;
    }
    if (!method.isEmpty()) { // a request of the server (ping …): answered, nothing else is offered
        if (!msg.contains("id")) return;
        QJsonObject answer{{"jsonrpc", "2.0"}, {"id", msg["id"]}};
        if (method == "ping") answer["result"] = QJsonObject{};
        else answer["error"] = QJsonObject{{"code", -32601}, {"message", "not supported"}};
        send(answer);
        return;
    }
    const auto it = m_pending.find(msg["id"].toInt());
    if (it == m_pending.end()) return;
    const Pending p = *it;
    m_pending.erase(it);
    if (p.reply) p.reply(msg);
}

void McpClient::finishAll(const Result& result)
{
    const QHash<int, Pending> pending = m_pending;
    m_pending.clear();
    for (const Pending& p : pending)
        if (p.done) p.done(result);
}

void McpClient::onEnded()
{
    if (!m_process) return;
    const bool starting = m_starting;
    QString why = m_process->error() == QProcess::FailedToStart
                      ? T("schneidi-cli startet nicht: %1").arg(QFileInfo(m_command.first()).fileName())
                      : T("Die Werkzeuge der KI (schneidi-cli) wurden beendet.");
    const QString last = QString::fromUtf8(m_errors).trimmed().section('\n', -1).trimmed();
    if (!last.isEmpty()) why += " " + last;
    m_process->disconnect(this);
    m_process->deleteLater();
    m_process = nullptr;
    m_ready = false;
    m_starting = false;
    finishAll({why, {}, true});
    if (starting) emit failed(why);
}

void McpClient::stop()
{
    if (!m_process) return;
    QProcess* p = m_process;
    m_process = nullptr;
    p->disconnect(this);
    p->kill();
    p->waitForFinished(2000);
    p->deleteLater();
    m_ready = false;
    m_starting = false;
    m_buffer.clear();
    finishAll({T("Gestoppt."), {}, true});
}
