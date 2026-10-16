/*
  Q Light Controller Plus
  aiagentdock.cpp

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

#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QTextBrowser>
#include <QPushButton>
#include <QScrollBar>
#include <QLineEdit>
#include <QSpinBox>
#include <QLabel>

#include "aiagentdock.h"
#include "aiagent.h"

#define AI_TOOL_TEXT_LIMIT 400

AIAgentDock::AIAgentDock(AIAgent *agent, QWidget *parent)
    : QDockWidget(tr("AI Agent"), parent)
    , m_agent(agent)
{
    Q_ASSERT(m_agent != nullptr);

    setObjectName("AIAgentDock");
    setAllowedAreas(Qt::AllDockWidgetAreas);

    QWidget *content = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(content);
    layout->setContentsMargins(4, 4, 4, 4);

    // Chat log
    m_log = new QTextBrowser(content);
    m_log->setOpenLinks(false);
    layout->addWidget(m_log, 1);

    // Settings panel (hidden by default)
    m_settingsPanel = new QWidget(content);
    QFormLayout *form = new QFormLayout(m_settingsPanel);
    form->setContentsMargins(0, 0, 0, 0);

    m_urlEdit = new QLineEdit(m_settingsPanel);
    m_urlEdit->setPlaceholderText("http://localhost:1234/v1");
    form->addRow(tr("API URL"), m_urlEdit);

    m_modelEdit = new QLineEdit(m_settingsPanel);
    m_modelEdit->setPlaceholderText(tr("empty = model loaded in the server"));
    form->addRow(tr("Model"), m_modelEdit);

    m_keyEdit = new QLineEdit(m_settingsPanel);
    m_keyEdit->setEchoMode(QLineEdit::Password);
    m_keyEdit->setPlaceholderText(tr("not needed for local servers"));
    form->addRow(tr("API key"), m_keyEdit);

    m_stepsSpin = new QSpinBox(m_settingsPanel);
    m_stepsSpin->setRange(1, 30);
    m_stepsSpin->setToolTip(tr("Maximum tool call rounds for one message"));
    form->addRow(tr("Max steps"), m_stepsSpin);

    m_promptEdit = new QPlainTextEdit(m_settingsPanel);
    m_promptEdit->setMaximumHeight(110);
    form->addRow(tr("System prompt"), m_promptEdit);

    QPushButton *resetPrompt = new QPushButton(tr("Reset prompt"), m_settingsPanel);
    form->addRow(QString(), resetPrompt);

    m_settingsPanel->hide();
    layout->addWidget(m_settingsPanel);

    // Input row
    QHBoxLayout *inputRow = new QHBoxLayout;
    m_input = new QLineEdit(content);
    m_input->setPlaceholderText(tr("Ask the AI to control your lights..."));
    inputRow->addWidget(m_input, 1);

    m_sendButton = new QPushButton(tr("Send"), content);
    inputRow->addWidget(m_sendButton);

    m_stopButton = new QPushButton(tr("Clear"), content);
    inputRow->addWidget(m_stopButton);

    m_settingsButton = new QPushButton(tr("Settings"), content);
    m_settingsButton->setCheckable(true);
    inputRow->addWidget(m_settingsButton);

    layout->addLayout(inputRow);
    setWidget(content);

    loadSettings();

    connect(m_input, SIGNAL(returnPressed()), this, SLOT(slotSend()));
    connect(m_sendButton, SIGNAL(clicked()), this, SLOT(slotSend()));
    connect(m_stopButton, SIGNAL(clicked()), this, SLOT(slotStopOrClear()));
    connect(m_settingsButton, SIGNAL(clicked()), this, SLOT(slotToggleSettings()));
    connect(resetPrompt, SIGNAL(clicked()), this, SLOT(slotResetPrompt()));

    connect(m_urlEdit, SIGNAL(editingFinished()), this, SLOT(slotSettingsEdited()));
    connect(m_modelEdit, SIGNAL(editingFinished()), this, SLOT(slotSettingsEdited()));
    connect(m_keyEdit, SIGNAL(editingFinished()), this, SLOT(slotSettingsEdited()));
    connect(m_stepsSpin, SIGNAL(editingFinished()), this, SLOT(slotSettingsEdited()));
    connect(m_promptEdit, SIGNAL(textChanged()), this, SLOT(slotSettingsEdited()));

    connect(m_agent, SIGNAL(message(QString,QString)),
            this, SLOT(slotMessage(QString,QString)));
    connect(m_agent, SIGNAL(toolCalled(QString,QString,QString)),
            this, SLOT(slotToolCalled(QString,QString,QString)));
    connect(m_agent, SIGNAL(errorOccurred(QString)),
            this, SLOT(slotError(QString)));
    connect(m_agent, SIGNAL(busyChanged(bool)),
            this, SLOT(slotBusyChanged(bool)));
}

AIAgentDock::~AIAgentDock()
{
}

void AIAgentDock::loadSettings()
{
    m_urlEdit->setText(m_agent->baseUrl());
    m_modelEdit->setText(m_agent->model());
    m_keyEdit->setText(m_agent->apiKey());
    m_stepsSpin->setValue(m_agent->maxSteps());
    m_promptEdit->setPlainText(m_agent->systemPrompt());
}

/*****************************************************************************
 * User actions
 *****************************************************************************/

void AIAgentDock::slotSend()
{
    const QString text = m_input->text().trimmed();
    if (text.isEmpty() || m_agent->isBusy())
        return;

    slotSettingsEdited();
    m_input->clear();
    m_agent->ask(text);
}

void AIAgentDock::slotStopOrClear()
{
    if (m_agent->isBusy())
    {
        m_agent->cancel();
        appendHtml("<i>" + tr("Cancelled.") + "</i>");
    }
    else
    {
        m_agent->reset();
        m_log->clear();
    }
}

void AIAgentDock::slotToggleSettings()
{
    m_settingsPanel->setVisible(m_settingsButton->isChecked());
}

void AIAgentDock::slotSettingsEdited()
{
    if (m_agent->baseUrl() != m_urlEdit->text())
        m_agent->setBaseUrl(m_urlEdit->text());
    if (m_agent->model() != m_modelEdit->text())
        m_agent->setModel(m_modelEdit->text());
    if (m_agent->apiKey() != m_keyEdit->text())
        m_agent->setApiKey(m_keyEdit->text());
    if (m_agent->maxSteps() != m_stepsSpin->value())
        m_agent->setMaxSteps(m_stepsSpin->value());
    // an empty prompt would silently fall back to the default while typing
    if (!m_promptEdit->toPlainText().trimmed().isEmpty() &&
        m_agent->systemPrompt() != m_promptEdit->toPlainText())
        m_agent->setSystemPrompt(m_promptEdit->toPlainText());
}

void AIAgentDock::slotResetPrompt()
{
    m_promptEdit->setPlainText(AIAgent::defaultSystemPrompt());
    m_agent->setSystemPrompt(AIAgent::defaultSystemPrompt());
}

/*****************************************************************************
 * Agent feedback
 *****************************************************************************/

void AIAgentDock::appendHtml(const QString &html)
{
    m_log->append(html);
    m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
}

void AIAgentDock::slotMessage(const QString &role, const QString &text)
{
    const QString escaped = text.toHtmlEscaped().replace("\n", "<br>");
    if (role == "user")
        appendHtml("<p><b>" + tr("You") + ":</b> " + escaped + "</p>");
    else
        appendHtml("<p><b>" + tr("AI") + ":</b> " + escaped + "</p>");
}

void AIAgentDock::slotToolCalled(const QString &name, const QString &arguments, const QString &result)
{
    QString args = arguments;
    if (args.length() > AI_TOOL_TEXT_LIMIT)
        args = args.left(AI_TOOL_TEXT_LIMIT) + "...";
    QString res = result;
    if (res.length() > AI_TOOL_TEXT_LIMIT)
        res = res.left(AI_TOOL_TEXT_LIMIT) + "...";

    appendHtml("<p style=\"color:gray\"><small>&#9881; <b>" + name.toHtmlEscaped() + "</b> "
               + args.toHtmlEscaped() + "<br>&#8594; " + res.toHtmlEscaped() + "</small></p>");
}

void AIAgentDock::slotError(const QString &error)
{
    appendHtml("<p style=\"color:red\"><b>" + tr("Error") + ":</b> " + error.toHtmlEscaped() + "</p>");
}

void AIAgentDock::slotBusyChanged(bool busy)
{
    m_input->setEnabled(!busy);
    m_sendButton->setEnabled(!busy);
    m_stopButton->setText(busy ? tr("Stop") : tr("Clear"));
    if (!busy)
        m_input->setFocus();
}
