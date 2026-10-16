/*
  Q Light Controller Plus
  aiagent.h

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

#ifndef AIAGENT_H
#define AIAGENT_H

#include <QJsonObject>
#include <QJsonArray>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class GenericDMXSource;
class Doc;

/** @addtogroup engine Engine
 * @{
 */

/**
 * AIAgent lets a language model operate QLC+.
 *
 * It talks to any OpenAI compatible "chat completions" endpoint (LM Studio,
 * llama.cpp server, Ollama, a cloud API, ...) and exposes a set of tools
 * (list fixtures, start functions, set channels, create scenes/chasers, ...)
 * that the model can call. The agent runs the tool-calling loop by itself
 * and reports everything through signals, so any UI can display it.
 *
 * All the work happens on the thread the object lives in (the GUI thread).
 */
class AIAgent : public QObject
{
    Q_OBJECT

public:
    AIAgent(Doc *doc, QObject *parent = nullptr);
    ~AIAgent();

    /*********************************************************************
     * Settings
     *********************************************************************/
public:
    /** Base URL of the OpenAI compatible API, e.g. http://localhost:1234/v1 */
    QString baseUrl() const;
    void setBaseUrl(const QString &url);

    /** Model identifier sent with every request. Can be empty for
     *  servers that only have one model loaded (LM Studio accepts that). */
    QString model() const;
    void setModel(const QString &model);

    /** Optional bearer token. Local servers usually don't need one. */
    QString apiKey() const;
    void setApiKey(const QString &key);

    /** Maximum number of model <-> tool round trips for one user message */
    int maxSteps() const;
    void setMaxSteps(int steps);

    /** The default system prompt, describing QLC+ and the tools */
    static QString defaultSystemPrompt();

    QString systemPrompt() const;
    void setSystemPrompt(const QString &prompt);

private:
    QString m_baseUrl;
    QString m_model;
    QString m_apiKey;
    QString m_systemPrompt;
    int m_maxSteps;

    /*********************************************************************
     * Conversation
     *********************************************************************/
public:
    /** Send a user message and run the agent loop until the model answers
     *  with plain text (or an error/step limit occurs) */
    void ask(const QString &text);

    /** Abort the running request, if any */
    void cancel();

    /** Forget the conversation history */
    void reset();

    bool isBusy() const;

signals:
    /** A message to show in the chat. role is "user", "assistant" or "system" */
    void message(const QString &role, const QString &text);

    /** The model called a tool. result is the JSON text returned to the model */
    void toolCalled(const QString &name, const QString &arguments, const QString &result);

    /** Something went wrong (network error, bad response, ...) */
    void errorOccurred(const QString &error);

    void busyChanged(bool busy);

private slots:
    void slotReplyFinished();

private:
    void sendRequest();
    void finish();
    QJsonArray buildMessages() const;
    void setBusy(bool busy);

private:
    QNetworkAccessManager *m_network;
    QNetworkReply *m_reply;
    QJsonArray m_history;
    int m_step;
    bool m_busy;

    /*********************************************************************
     * Tools
     *********************************************************************/
private:
    /** The tool definitions in OpenAI "tools" format */
    static QJsonArray toolDefinitions();

    /** Run a tool and return its result as a JSON object */
    QJsonObject executeTool(const QString &name, const QJsonObject &args);

    QJsonObject toolListFixtures();
    QJsonObject toolListFunctions();
    QJsonObject toolStartFunction(const QJsonObject &args, bool start);
    QJsonObject toolStopAll();
    QJsonObject toolSetChannels(const QJsonObject &args);
    QJsonObject toolReleaseChannels();
    QJsonObject toolCreateScene(const QJsonObject &args);
    QJsonObject toolCreateChaser(const QJsonObject &args);
    QJsonObject toolDeleteFunction(const QJsonObject &args);

    /** Lazily create the DMX source used for live channel values */
    GenericDMXSource *liveSource();

    static QJsonObject error(const QString &text);

private:
    Doc *m_doc;
    GenericDMXSource *m_liveSource;
};

/** @} */

#endif
