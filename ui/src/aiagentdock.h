/*
  Q Light Controller Plus
  aiagentdock.h

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

#ifndef AIAGENTDOCK_H
#define AIAGENTDOCK_H

#include <QDockWidget>

class QPlainTextEdit;
class QTextBrowser;
class QPushButton;
class QSpinBox;
class QLineEdit;
class QWidget;
class AIAgent;

/** @addtogroup ui UI
 * @{
 */

/**
 * Dock window with a chat for the AI agent: the user types a request,
 * the agent answers and the tool calls it makes are shown in the log.
 */
class AIAgentDock final : public QDockWidget
{
    Q_OBJECT
    Q_DISABLE_COPY(AIAgentDock)

public:
    AIAgentDock(AIAgent *agent, QWidget *parent = nullptr);
    ~AIAgentDock();

private slots:
    void slotSend();
    void slotStopOrClear();
    void slotToggleSettings();
    void slotSettingsEdited();
    void slotResetPrompt();

    void slotMessage(const QString &role, const QString &text);
    void slotToolCalled(const QString &name, const QString &arguments, const QString &result);
    void slotError(const QString &error);
    void slotBusyChanged(bool busy);

private:
    void appendHtml(const QString &html);
    void loadSettings();

private:
    AIAgent *m_agent;

    QTextBrowser *m_log;
    QLineEdit *m_input;
    QPushButton *m_sendButton;
    QPushButton *m_stopButton;
    QPushButton *m_settingsButton;

    QWidget *m_settingsPanel;
    QLineEdit *m_urlEdit;
    QLineEdit *m_modelEdit;
    QLineEdit *m_keyEdit;
    QSpinBox *m_stepsSpin;
    QPlainTextEdit *m_promptEdit;
};

/** @} */

#endif
