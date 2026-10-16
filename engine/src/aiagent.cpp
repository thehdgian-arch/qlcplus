/*
  Q Light Controller Plus
  aiagent.cpp

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSettings>
#include <QDebug>
#include <QUrl>

#include "genericdmxsource.h"
#include "mastertimer.h"
#include "qlcfixturedef.h"
#include "qlcfixturemode.h"
#include "qlcchannel.h"
#include "chaserstep.h"
#include "aiagent.h"
#include "fixture.h"
#include "chaser.h"
#include "scene.h"
#include "doc.h"

#define SETTINGS_AI_URL     "aiagent/baseurl"
#define SETTINGS_AI_MODEL   "aiagent/model"
#define SETTINGS_AI_KEY     "aiagent/apikey"
#define SETTINGS_AI_PROMPT  "aiagent/systemprompt"
#define SETTINGS_AI_STEPS   "aiagent/maxsteps"

#define AI_MAX_HISTORY      60
#define AI_TIMEOUT_MS       300000

/*****************************************************************************
 * Small JSON helpers
 *****************************************************************************/

static QString toCompactString(const QJsonObject &obj)
{
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

static QJsonObject makeProperty(const QString &type, const QString &description)
{
    QJsonObject o;
    o["type"] = type;
    o["description"] = description;
    return o;
}

static QJsonObject makeTool(const QString &name, const QString &description,
                            const QJsonObject &properties = QJsonObject(),
                            const QStringList &required = QStringList())
{
    QJsonObject params;
    params["type"] = "object";
    params["properties"] = properties;

    QJsonArray req;
    foreach (const QString &r, required)
        req.append(r);
    params["required"] = req;

    QJsonObject function;
    function["name"] = name;
    function["description"] = description;
    function["parameters"] = params;

    QJsonObject tool;
    tool["type"] = "function";
    tool["function"] = function;
    return tool;
}

/*****************************************************************************
 * Initialization
 *****************************************************************************/

AIAgent::AIAgent(Doc *doc, QObject *parent)
    : QObject(parent)
    , m_maxSteps(8)
    , m_network(new QNetworkAccessManager(this))
    , m_reply(nullptr)
    , m_step(0)
    , m_busy(false)
    , m_doc(doc)
    , m_liveSource(nullptr)
{
    Q_ASSERT(m_doc != nullptr);

    QSettings settings;
    m_baseUrl = settings.value(SETTINGS_AI_URL, "http://localhost:1234/v1").toString();
    m_model = settings.value(SETTINGS_AI_MODEL, "").toString();
    m_apiKey = settings.value(SETTINGS_AI_KEY, "").toString();
    m_systemPrompt = settings.value(SETTINGS_AI_PROMPT, defaultSystemPrompt()).toString();
    m_maxSteps = settings.value(SETTINGS_AI_STEPS, 8).toInt();
}

AIAgent::~AIAgent()
{
    if (m_reply != nullptr)
    {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }

    delete m_liveSource;
    m_liveSource = nullptr;
}

/*****************************************************************************
 * Settings
 *****************************************************************************/

QString AIAgent::baseUrl() const
{
    return m_baseUrl;
}

void AIAgent::setBaseUrl(const QString &url)
{
    m_baseUrl = url.trimmed();
    QSettings().setValue(SETTINGS_AI_URL, m_baseUrl);
}

QString AIAgent::model() const
{
    return m_model;
}

void AIAgent::setModel(const QString &model)
{
    m_model = model.trimmed();
    QSettings().setValue(SETTINGS_AI_MODEL, m_model);
}

QString AIAgent::apiKey() const
{
    return m_apiKey;
}

void AIAgent::setApiKey(const QString &key)
{
    m_apiKey = key.trimmed();
    QSettings().setValue(SETTINGS_AI_KEY, m_apiKey);
}

int AIAgent::maxSteps() const
{
    return m_maxSteps;
}

void AIAgent::setMaxSteps(int steps)
{
    m_maxSteps = qBound(1, steps, 30);
    QSettings().setValue(SETTINGS_AI_STEPS, m_maxSteps);
}

QString AIAgent::defaultSystemPrompt()
{
    return QStringLiteral(
        "You are the lighting assistant built into QLC+, a lighting control "
        "application. You operate the running project through tools.\n"
        "\n"
        "Concepts:\n"
        "- A fixture is a lamp/device with an id, a universe, a DMX address and a "
        "list of channels. Channel numbers in the tools are 0-based indexes "
        "inside the fixture (as listed by list_fixtures), NOT DMX addresses. "
        "Universes and DMX addresses shown to the user are 1-based.\n"
        "- A function is a stored item: a scene (static channel values), a "
        "chaser (list of steps, each step runs a function), and so on.\n"
        "- Channel values are 0-255. For a dimmer 255 is full brightness. "
        "Read the channel names to find out which channel is dimmer, red, green, "
        "blue, pan, tilt, strobe, gobo, etc.\n"
        "\n"
        "How to work:\n"
        "1. Call list_fixtures and/or list_functions first if you do not know the "
        "ids yet. Never invent ids.\n"
        "2. Use set_channels for immediate live changes, create_scene to store a "
        "look, create_chaser to sequence existing functions.\n"
        "3. Times are in milliseconds.\n"
        "4. After acting, answer briefly in the language of the user and say "
        "what you did. If something failed, say so plainly.");
}

QString AIAgent::systemPrompt() const
{
    return m_systemPrompt;
}

void AIAgent::setSystemPrompt(const QString &prompt)
{
    m_systemPrompt = prompt.trimmed().isEmpty() ? defaultSystemPrompt() : prompt;
    QSettings().setValue(SETTINGS_AI_PROMPT, m_systemPrompt);
}

/*****************************************************************************
 * Conversation
 *****************************************************************************/

bool AIAgent::isBusy() const
{
    return m_busy;
}

void AIAgent::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged(busy);
}

void AIAgent::reset()
{
    cancel();
    m_history = QJsonArray();
}

void AIAgent::cancel()
{
    if (m_reply != nullptr)
    {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    setBusy(false);
}

void AIAgent::ask(const QString &text)
{
    if (m_busy || text.trimmed().isEmpty())
        return;

    // keep the history bounded, never starting with an orphan tool message
    while (m_history.count() > AI_MAX_HISTORY)
    {
        m_history.removeFirst();
        while (!m_history.isEmpty() &&
               m_history.first().toObject().value("role").toString() != "user")
            m_history.removeFirst();
    }

    QJsonObject userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = text;
    m_history.append(userMsg);
    emit message("user", text);

    m_step = 0;
    setBusy(true);
    sendRequest();
}

QJsonArray AIAgent::buildMessages() const
{
    QJsonArray msgs;

    QJsonObject sys;
    sys["role"] = "system";
    sys["content"] = m_systemPrompt;
    msgs.append(sys);

    foreach (const QJsonValue &v, m_history)
        msgs.append(v);

    return msgs;
}

void AIAgent::sendRequest()
{
    QString base = m_baseUrl.trimmed();
    while (base.endsWith('/'))
        base.chop(1);

    if (base.isEmpty())
    {
        emit errorOccurred(tr("No API base URL configured"));
        finish();
        return;
    }

    QJsonObject body;
    if (!m_model.isEmpty())
        body["model"] = m_model;
    body["messages"] = buildMessages();
    body["tools"] = toolDefinitions();
    body["tool_choice"] = "auto";
    body["temperature"] = 0.3;
    body["stream"] = false;

    QNetworkRequest request{QUrl(base + "/chat/completions")};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!m_apiKey.isEmpty())
        request.setRawHeader("Authorization", QByteArray("Bearer ") + m_apiKey.toUtf8());
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(AI_TIMEOUT_MS);
#endif

    m_reply = m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::finished, this, &AIAgent::slotReplyFinished);
}

void AIAgent::finish()
{
    setBusy(false);
}

void AIAgent::slotReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (reply == nullptr || reply != m_reply)
        return; // cancelled or stale

    m_reply = nullptr;
    reply->deleteLater();

    const QByteArray data = reply->readAll();

    if (reply->error() != QNetworkReply::NoError)
    {
        QString detail = reply->errorString();
        QJsonObject errObj = QJsonDocument::fromJson(data).object().value("error").toObject();
        if (!errObj.isEmpty())
            detail += " - " + errObj.value("message").toString();
        emit errorOccurred(detail);
        finish();
        return;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
    {
        emit errorOccurred(tr("Invalid response from the server: %1").arg(parseError.errorString()));
        finish();
        return;
    }

    QJsonArray choices = doc.object().value("choices").toArray();
    if (choices.isEmpty())
    {
        emit errorOccurred(tr("The server returned no answer"));
        finish();
        return;
    }

    QJsonObject msg = choices.first().toObject().value("message").toObject();
    const QString content = msg.value("content").toString();
    const QJsonArray toolCalls = msg.value("tool_calls").toArray();

    // store the assistant turn exactly as it should be replayed
    QJsonObject assistantMsg;
    assistantMsg["role"] = "assistant";
    assistantMsg["content"] = content;
    if (!toolCalls.isEmpty())
        assistantMsg["tool_calls"] = toolCalls;
    m_history.append(assistantMsg);

    if (!content.trimmed().isEmpty())
        emit message("assistant", content);

    if (toolCalls.isEmpty())
    {
        finish();
        return;
    }

    // run every requested tool and feed the results back
    foreach (const QJsonValue &tcVal, toolCalls)
    {
        QJsonObject tc = tcVal.toObject();
        QJsonObject fn = tc.value("function").toObject();
        const QString name = fn.value("name").toString();
        const QString argString = fn.value("arguments").toString();

        QJsonObject args;
        QJsonObject result;
        if (argString.trimmed().isEmpty())
        {
            result = executeTool(name, args);
        }
        else
        {
            QJsonParseError argError;
            QJsonDocument argDoc = QJsonDocument::fromJson(argString.toUtf8(), &argError);
            if (argError.error != QJsonParseError::NoError || !argDoc.isObject())
                result = error(tr("Tool arguments are not valid JSON: %1").arg(argError.errorString()));
            else
            {
                args = argDoc.object();
                result = executeTool(name, args);
            }
        }

        const QString resultString = toCompactString(result);
        emit toolCalled(name, argString, resultString);

        QJsonObject toolMsg;
        toolMsg["role"] = "tool";
        toolMsg["tool_call_id"] = tc.value("id").toString();
        toolMsg["content"] = resultString;
        m_history.append(toolMsg);
    }

    if (++m_step >= m_maxSteps)
    {
        emit errorOccurred(tr("Stopped after %1 tool rounds (step limit)").arg(m_maxSteps));
        finish();
        return;
    }

    sendRequest();
}

/*****************************************************************************
 * Tools
 *****************************************************************************/

QJsonObject AIAgent::error(const QString &text)
{
    QJsonObject o;
    o["ok"] = false;
    o["error"] = text;
    return o;
}

QJsonArray AIAgent::toolDefinitions()
{
    QJsonArray tools;

    tools.append(makeTool("list_fixtures",
        "List all fixtures of the project with id, name, universe (1-based), "
        "DMX address (1-based) and the channel list (0-based index, name, group)."));

    tools.append(makeTool("list_functions",
        "List all functions (scenes, chasers, ...) with id, name, type, folder path "
        "and whether they are currently running."));

    QJsonObject idProp;
    idProp["function_id"] = makeProperty("integer", "Id of the function");

    tools.append(makeTool("start_function",
        "Start a function (scene, chaser, ...) by id.", idProp, QStringList() << "function_id"));
    tools.append(makeTool("stop_function",
        "Stop a running function by id.", idProp, QStringList() << "function_id"));
    tools.append(makeTool("stop_all_functions",
        "Stop every running function."));

    // set_channels
    QJsonObject valueItemProps;
    valueItemProps["fixture_id"] = makeProperty("integer", "Fixture id");
    valueItemProps["channel"] = makeProperty("integer", "0-based channel index inside the fixture");
    valueItemProps["value"] = makeProperty("integer", "DMX value 0-255");
    QJsonObject valueItem;
    valueItem["type"] = "object";
    valueItem["properties"] = valueItemProps;
    valueItem["required"] = QJsonArray::fromStringList(
        QStringList() << "fixture_id" << "channel" << "value");

    QJsonObject valuesProp;
    valuesProp["type"] = "array";
    valuesProp["description"] = "Channel values to set";
    valuesProp["items"] = valueItem;

    QJsonObject setProps;
    setProps["values"] = valuesProp;
    tools.append(makeTool("set_channels",
        "Set live channel values right now (like moving faders). The values stay "
        "active until release_channels is called.",
        setProps, QStringList() << "values"));

    tools.append(makeTool("release_channels",
        "Release all live channel values set with set_channels."));

    // create_scene
    QJsonObject sceneProps;
    sceneProps["name"] = makeProperty("string", "Name of the new scene");
    sceneProps["values"] = valuesProp;
    sceneProps["start"] = makeProperty("boolean", "Start the scene right after creating it");
    tools.append(makeTool("create_scene",
        "Create and store a new scene made of fixed channel values.",
        sceneProps, QStringList() << "name" << "values"));

    // create_chaser
    QJsonObject stepProps;
    stepProps["function_id"] = makeProperty("integer", "Id of the function this step runs");
    stepProps["fade_in_ms"] = makeProperty("integer", "Fade in time in milliseconds");
    stepProps["hold_ms"] = makeProperty("integer", "Hold time in milliseconds");
    stepProps["fade_out_ms"] = makeProperty("integer", "Fade out time in milliseconds");
    QJsonObject stepItem;
    stepItem["type"] = "object";
    stepItem["properties"] = stepProps;
    stepItem["required"] = QJsonArray::fromStringList(QStringList() << "function_id");

    QJsonObject stepsProp;
    stepsProp["type"] = "array";
    stepsProp["description"] = "Ordered steps of the chaser";
    stepsProp["items"] = stepItem;

    QJsonObject chaserProps;
    chaserProps["name"] = makeProperty("string", "Name of the new chaser");
    chaserProps["steps"] = stepsProp;
    chaserProps["loop"] = makeProperty("boolean", "Loop forever (default true), otherwise run once");
    chaserProps["start"] = makeProperty("boolean", "Start the chaser right after creating it");
    tools.append(makeTool("create_chaser",
        "Create and store a new chaser from existing functions.",
        chaserProps, QStringList() << "name" << "steps"));

    tools.append(makeTool("delete_function",
        "Delete a function by id. A running function is stopped first and must be deleted with a second call.",
        idProp, QStringList() << "function_id"));

    return tools;
}

QJsonObject AIAgent::executeTool(const QString &name, const QJsonObject &args)
{
    if (name == "list_fixtures")
        return toolListFixtures();
    else if (name == "list_functions")
        return toolListFunctions();
    else if (name == "start_function")
        return toolStartFunction(args, true);
    else if (name == "stop_function")
        return toolStartFunction(args, false);
    else if (name == "stop_all_functions")
        return toolStopAll();
    else if (name == "set_channels")
        return toolSetChannels(args);
    else if (name == "release_channels")
        return toolReleaseChannels();
    else if (name == "create_scene")
        return toolCreateScene(args);
    else if (name == "create_chaser")
        return toolCreateChaser(args);
    else if (name == "delete_function")
        return toolDeleteFunction(args);

    return error(tr("Unknown tool: %1").arg(name));
}

QJsonObject AIAgent::toolListFixtures()
{
    QJsonArray list;

    foreach (Fixture *fxi, m_doc->fixtures())
    {
        QJsonObject f;
        f["id"] = int(fxi->id());
        f["name"] = fxi->name();
        f["universe"] = int(fxi->universe()) + 1;
        f["address"] = int(fxi->address()) + 1;

        if (fxi->fixtureDef() != nullptr)
        {
            f["manufacturer"] = fxi->fixtureDef()->manufacturer();
            f["model"] = fxi->fixtureDef()->model();
        }
        if (fxi->fixtureMode() != nullptr)
            f["mode"] = fxi->fixtureMode()->name();

        QJsonArray channels;
        for (quint32 i = 0; i < fxi->channels(); i++)
        {
            const QLCChannel *ch = fxi->channel(i);
            if (ch == nullptr)
                continue;
            QJsonObject c;
            c["index"] = int(i);
            c["name"] = ch->name();
            c["group"] = ch->groupString();
            channels.append(c);
        }
        f["channels"] = channels;
        list.append(f);
    }

    QJsonObject res;
    res["ok"] = true;
    res["fixtures"] = list;
    return res;
}

QJsonObject AIAgent::toolListFunctions()
{
    QJsonArray list;

    foreach (Function *func, m_doc->functions())
    {
        QJsonObject f;
        f["id"] = int(func->id());
        f["name"] = func->name();
        f["type"] = Function::typeToString(func->type());
        f["path"] = func->path(true);
        f["running"] = func->isRunning();

        Chaser *chaser = qobject_cast<Chaser *>(func);
        if (chaser != nullptr)
            f["steps"] = chaser->stepsCount();

        list.append(f);
    }

    QJsonObject res;
    res["ok"] = true;
    res["functions"] = list;
    return res;
}

QJsonObject AIAgent::toolStartFunction(const QJsonObject &args, bool start)
{
    if (!args.contains("function_id"))
        return error(tr("Missing function_id"));

    Function *func = m_doc->function(quint32(args.value("function_id").toInt(-1)));
    if (func == nullptr)
        return error(tr("No function with this id"));

    if (start)
    {
        if (!func->isRunning())
            func->start(m_doc->masterTimer(), FunctionParent::master());
    }
    else
    {
        if (func->isRunning())
            func->stop(FunctionParent::master());
    }

    QJsonObject res;
    res["ok"] = true;
    res["function"] = func->name();
    res["action"] = start ? "started" : "stopped";
    return res;
}

QJsonObject AIAgent::toolStopAll()
{
    m_doc->masterTimer()->stopAllFunctions();

    QJsonObject res;
    res["ok"] = true;
    return res;
}

GenericDMXSource *AIAgent::liveSource()
{
    if (m_liveSource == nullptr)
    {
        m_liveSource = new GenericDMXSource(m_doc);
        m_liveSource->setOutputEnabled(true);
    }
    return m_liveSource;
}

QJsonObject AIAgent::toolSetChannels(const QJsonObject &args)
{
    const QJsonArray values = args.value("values").toArray();
    if (values.isEmpty())
        return error(tr("Missing values"));

    int applied = 0;
    QJsonArray problems;

    foreach (const QJsonValue &v, values)
    {
        QJsonObject o = v.toObject();
        const int fid = o.value("fixture_id").toInt(-1);
        const int ch = o.value("channel").toInt(-1);
        const int val = qBound(0, o.value("value").toInt(0), 255);

        Fixture *fxi = (fid < 0) ? nullptr : m_doc->fixture(quint32(fid));
        if (fxi == nullptr)
        {
            problems.append(tr("No fixture with id %1").arg(fid));
            continue;
        }
        if (ch < 0 || quint32(ch) >= fxi->channels())
        {
            problems.append(tr("Fixture %1 has no channel %2 (it has %3 channels)")
                            .arg(fid).arg(ch).arg(fxi->channels()));
            continue;
        }

        liveSource()->set(quint32(fid), quint32(ch), uchar(val));
        applied++;
    }

    QJsonObject res;
    res["ok"] = (applied > 0);
    res["applied"] = applied;
    if (!problems.isEmpty())
        res["problems"] = problems;
    return res;
}

QJsonObject AIAgent::toolReleaseChannels()
{
    if (m_liveSource != nullptr)
        m_liveSource->unsetAll();

    QJsonObject res;
    res["ok"] = true;
    return res;
}

QJsonObject AIAgent::toolCreateScene(const QJsonObject &args)
{
    const QString name = args.value("name").toString().trimmed();
    if (name.isEmpty())
        return error(tr("Missing name"));

    const QJsonArray values = args.value("values").toArray();
    if (values.isEmpty())
        return error(tr("Missing values"));

    // validate first, so that a bad request does not leave a half scene behind
    QList<SceneValue> scValues;
    foreach (const QJsonValue &v, values)
    {
        QJsonObject o = v.toObject();
        const int fid = o.value("fixture_id").toInt(-1);
        const int ch = o.value("channel").toInt(-1);
        const int val = qBound(0, o.value("value").toInt(0), 255);

        Fixture *fxi = (fid < 0) ? nullptr : m_doc->fixture(quint32(fid));
        if (fxi == nullptr)
            return error(tr("No fixture with id %1").arg(fid));
        if (ch < 0 || quint32(ch) >= fxi->channels())
            return error(tr("Fixture %1 has no channel %2").arg(fid).arg(ch));

        scValues.append(SceneValue(quint32(fid), quint32(ch), uchar(val)));
    }

    Scene *scene = new Scene(m_doc);
    scene->setName(name);
    foreach (const SceneValue &scv, scValues)
        scene->setValue(scv);

    if (!m_doc->addFunction(scene))
    {
        delete scene;
        return error(tr("Could not add the scene to the project"));
    }

    if (args.value("start").toBool(false))
        scene->start(m_doc->masterTimer(), FunctionParent::master());

    QJsonObject res;
    res["ok"] = true;
    res["function_id"] = int(scene->id());
    res["values"] = scValues.count();
    return res;
}

QJsonObject AIAgent::toolCreateChaser(const QJsonObject &args)
{
    const QString name = args.value("name").toString().trimmed();
    if (name.isEmpty())
        return error(tr("Missing name"));

    const QJsonArray steps = args.value("steps").toArray();
    if (steps.isEmpty())
        return error(tr("A chaser needs at least one step"));

    QList<ChaserStep> chaserSteps;
    foreach (const QJsonValue &v, steps)
    {
        QJsonObject o = v.toObject();
        const int fid = o.value("function_id").toInt(-1);
        if (fid < 0 || m_doc->function(quint32(fid)) == nullptr)
            return error(tr("No function with id %1").arg(fid));

        const uint fadeIn = uint(qMax(0, o.value("fade_in_ms").toInt(0)));
        const uint hold = uint(qMax(0, o.value("hold_ms").toInt(1000)));
        const uint fadeOut = uint(qMax(0, o.value("fade_out_ms").toInt(0)));
        chaserSteps.append(ChaserStep(quint32(fid), fadeIn, hold, fadeOut));
    }

    Chaser *chaser = new Chaser(m_doc);
    chaser->setName(name);
    chaser->setFadeInMode(Chaser::PerStep);
    chaser->setFadeOutMode(Chaser::PerStep);
    chaser->setDurationMode(Chaser::PerStep);
    chaser->setDirection(Function::Forward);
    chaser->setRunOrder(args.value("loop").toBool(true) ? Function::Loop : Function::SingleShot);

    foreach (const ChaserStep &step, chaserSteps)
        chaser->addStep(step);

    if (!m_doc->addFunction(chaser))
    {
        delete chaser;
        return error(tr("Could not add the chaser to the project"));
    }

    if (args.value("start").toBool(false))
        chaser->start(m_doc->masterTimer(), FunctionParent::master());

    QJsonObject res;
    res["ok"] = true;
    res["function_id"] = int(chaser->id());
    res["steps"] = chaserSteps.count();
    return res;
}

QJsonObject AIAgent::toolDeleteFunction(const QJsonObject &args)
{
    if (!args.contains("function_id"))
        return error(tr("Missing function_id"));

    const quint32 fid = quint32(args.value("function_id").toInt(-1));
    Function *func = m_doc->function(fid);
    if (func == nullptr)
        return error(tr("No function with this id"));

    if (func->isRunning())
    {
        func->stop(FunctionParent::master());
        return error(tr("The function was running and has been stopped. Call delete_function again to delete it."));
    }

    const QString name = func->name();
    if (!m_doc->deleteFunction(fid))
        return error(tr("Could not delete the function"));

    QJsonObject res;
    res["ok"] = true;
    res["deleted"] = name;
    return res;
}
