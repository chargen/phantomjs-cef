// Copyright (c) 2015 Klaralvdalens Datakonsult AB (KDAB).
// All rights reserved. Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "handler.h"

#include <sstream>
#include <string>
#include <iostream>
#include <locale>
#include <algorithm>

#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

#include "include/base/cef_bind.h"
#include "include/cef_app.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include "print_handler.h"

std::ostream& operator<<(std::ostream& stream, const wchar_t *input)
{
    return stream << qPrintable(QString::fromWCharArray(input));
}

QDebug operator<<(QDebug stream, const CefString& string)
{
  return stream << QString::fromStdString(string.ToString());
}

namespace {
CefRefPtr<CefMessageRouterBrowserSide::Callback> takeCallback(QHash<int32, CefRefPtr<CefMessageRouterBrowserSide::Callback>>* callbacks,
                                                              const CefRefPtr<CefBrowser>& browser)
{
  auto it = callbacks->find(browser->GetIdentifier());
  if (it != callbacks->end()) {
    auto ret = it.value();
    callbacks->erase(it);
    return ret;
  }
  return {};
}
}

PhantomJSHandler::PhantomJSHandler()
    : m_messageRouter(CefMessageRouterBrowserSide::Create(messageRouterConfig()))
{
  m_messageRouter->AddHandler(this, false);
}

PhantomJSHandler::~PhantomJSHandler()
{
}

CefMessageRouterConfig PhantomJSHandler::messageRouterConfig()
{
  CefMessageRouterConfig config;
  config.js_cancel_function = "cancelPhantomJsQuery";
  config.js_query_function = "startPhantomJsQuery";
  return config;
}

CefRefPtr<CefBrowser> PhantomJSHandler::createBrowser(const CefString& url)
{
  // Information used when creating the native window.
  CefWindowInfo window_info;
#if defined(OS_WIN)
  // On Windows we need to specify certain flags that will be passed to
  // CreateWindowEx().
  window_info.SetAsPopup(NULL, "phantomjs");
#endif
  window_info.SetAsWindowless(0, true);

  // Specify CEF browser settings here.
  CefBrowserSettings browser_settings;
  // TODO: make this configurable
  browser_settings.web_security = STATE_DISABLED;
  browser_settings.universal_access_from_file_urls = STATE_ENABLED;
  browser_settings.file_access_from_file_urls = STATE_ENABLED;

  return CefBrowserHost::CreateBrowserSync(window_info, this, url, browser_settings,
                                           NULL);
}

bool PhantomJSHandler::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                                CefProcessId source_process,
                                                CefRefPtr<CefProcessMessage> message)
{
  if (m_messageRouter->OnProcessMessageReceived(browser, source_process, message)) {
    return true;
  }
  if (message->GetName() == "exit") {
    CloseAllBrowsers(true);
    return true;
  }
  return false;
}

void PhantomJSHandler::OnTitleChange(CefRefPtr<CefBrowser> browser,
                                  const CefString& title)
{
  CEF_REQUIRE_UI_THREAD();
  // TODO: send a signal via persistent callback?
//   std::string titleStr(title);

//   std::cerr << "title changed to: " << title << '\n';
}

bool PhantomJSHandler::OnConsoleMessage(CefRefPtr<CefBrowser> browser, const CefString& message, const CefString& source, int line)
{
  std::cerr << source << ':' << line << ": " << message << '\n';
  return true;
}

void PhantomJSHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
  CEF_REQUIRE_UI_THREAD();

  m_browsers[browser->GetIdentifier()] = browser;
}

bool PhantomJSHandler::DoClose(CefRefPtr<CefBrowser> browser)
{
  CEF_REQUIRE_UI_THREAD();

  // Allow the close. For windowed browsers this will result in the OS close
  // event being sent.
  return false;
}

void PhantomJSHandler::OnBeforeClose(CefRefPtr<CefBrowser> browser)
{
  CEF_REQUIRE_UI_THREAD();

  m_messageRouter->OnBeforeClose(browser);

  m_browsers.remove(browser->GetIdentifier());

  if (m_browsers.empty()) {
    // All browser windows have closed. Quit the application message loop.
    CefQuitMessageLoop();
  }
}

void PhantomJSHandler::OnLoadError(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                ErrorCode errorCode,
                                const CefString& errorText,
                                const CefString& failedUrl)
{
  CEF_REQUIRE_UI_THREAD();

  if (frame->IsMain()) {
    handleLoadEnd(browser, errorCode, false);
  }

  qWarning() << browser->GetIdentifier() << errorCode << errorText << failedUrl;
  // Don't display an error for downloaded files.
  if (errorCode == ERR_ABORTED)
    return;

  // Display a load error message.
  std::stringstream ss;
  ss << "<html><body bgcolor=\"white\">"
        "<h2>Failed to load URL " << std::string(failedUrl) <<
        " with error " << std::string(errorText) << " (" << errorCode <<
        ").</h2></body></html>";
  frame->LoadString(ss.str(), failedUrl);
}

void PhantomJSHandler::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame)
{
  CEF_REQUIRE_UI_THREAD();

  // filter out events from sub frames
  if (!frame->IsMain()) {
    return;
  }

  qDebug() << browser->GetIdentifier() << frame->GetURL();

  auto callback = m_browserSignals.value(browser->GetIdentifier());
  if (!callback) {
    return;
  }
  callback->Success("{\"signal\":\"onLoadStarted\"}");
}

void PhantomJSHandler::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode)
{
  CEF_REQUIRE_UI_THREAD();

  // filter out events from sub frames or when loading about:blank
  if (!frame->IsMain() || httpStatusCode < 200) {
    return;
  }

  qDebug() << browser->GetIdentifier() << frame->GetURL() << httpStatusCode;

  /// TODO: is this OK?
  const bool success = httpStatusCode < 400;
  handleLoadEnd(browser, httpStatusCode, success);
}

void PhantomJSHandler::handleLoadEnd(CefRefPtr<CefBrowser> browser, int statusCode, bool success)
{
  if (auto callback = takeCallback(&m_pendingOpenBrowserRequests, browser)) {
    if (success) {
      callback->Success(std::to_string(statusCode));
    } else {
      callback->Failure(statusCode, "load error");
    }
  }

  if (auto callback = m_browserSignals.value(browser->GetIdentifier())) {
    if (success) {
      callback->Success("{\"signal\":\"onLoadFinished\",\"args\":[\"success\"]}");
    } else {
      callback->Success("{\"signal\":\"onLoadFinished\",\"args\":[\"fail\"]}");
    }
  }
}

bool PhantomJSHandler::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect)
{
  // TODO: make this configurable
  rect.Set(0, 0, 800, 600);
  return true;
}

void PhantomJSHandler::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects, const void* buffer, int width, int height)
{
  // TODO: grab screenshots?
  // do nothing
}

void PhantomJSHandler::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status)
{
  m_messageRouter->OnRenderProcessTerminated(browser);
}

bool PhantomJSHandler::OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, bool is_redirect)
{
  m_messageRouter->OnBeforeBrowse(browser, frame);
  return false;
}

void PhantomJSHandler::CloseAllBrowsers(bool force_close)
{
  m_messageRouter->CancelPending(nullptr, nullptr);

  if (!CefCurrentlyOn(TID_UI)) {
    // Execute on the UI thread.
    CefPostTask(TID_UI,
        base::Bind(&PhantomJSHandler::CloseAllBrowsers, this, force_close));
    return;
  }

  foreach (const auto& browser, m_browsers) {
    browser->GetHost()->CloseBrowser(force_close);
  }
  LOG_ASSERT(m_browsers.empty());
}

bool PhantomJSHandler::OnQuery(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                               int64 query_id, const CefString& request, bool persistent,
                               CefRefPtr<Callback> callback)
{
  CEF_REQUIRE_UI_THREAD();

  const auto data = QByteArray::fromStdString(request.ToString());

  QJsonParseError error;
  const auto json = QJsonDocument::fromJson(data, &error).object();
  if (error.error) {
    qWarning() << error.errorString();
    return false;
  }

  const auto type = json.value(QStringLiteral("type")).toString();

  if (type == QLatin1String("createBrowser")) {
    auto subBrowser = createBrowser("about:blank");
    callback->Success(std::to_string(subBrowser->GetIdentifier()));
    return true;
  } else if (type == QLatin1String("webPageSignals")) {
    const auto subBrowserId = json.value(QStringLiteral("browser")).toInt(-1);
    m_browserSignals[subBrowserId] = callback;
    return true;
  } else if (type == QLatin1String("returnEvaluateJavaScript")) {
    auto otherQueryId = json.value(QStringLiteral("queryId")).toInt(-1);
    auto it = m_pendingQueryCallbacks.find(otherQueryId);
    if (it != m_pendingQueryCallbacks.end()) {
      auto retval = json.value(QStringLiteral("retval")).toString();
      auto exception = json.value(QStringLiteral("exception")).toString();
      auto otherCallback = it.value();
      if (exception.isEmpty()) {
        otherCallback->Success(retval.toStdString());
      } else {
        otherCallback->Failure(1, exception.toStdString());
      }
      m_pendingQueryCallbacks.erase(it);
      callback->Success({});
      return true;
    }
  }

  const auto subBrowserId = json.value(QStringLiteral("browser")).toInt(-1);
  CefRefPtr<CefBrowser> subBrowser = m_browsers.value(subBrowserId);
  if (!subBrowser) {
    qWarning() << "Unknown browser with id" << subBrowserId << "for request" << json;
    return false;
  }

  // below, all queries work on a browser
  if (type == QLatin1String("openWebPage")) {
    const auto url = json.value(QStringLiteral("url")).toString().toStdString();
    subBrowser->GetMainFrame()->LoadURL(url);
    m_pendingOpenBrowserRequests[subBrowser->GetIdentifier()] = callback;
    return true;
  } else if (type == QLatin1String("stopWebPage")) {
    subBrowser->StopLoad();
    callback->Success({});
    return true;
  } else if (type == QLatin1String("closeWebPage")) {
    subBrowser->GetHost()->CloseBrowser(true);
    callback->Success({});
    return true;
  } else if (type == QLatin1String("evaluateJavaScript")) {
    auto script = json.value(QStringLiteral("script")).toString();
    m_pendingQueryCallbacks[query_id] = callback;
    script = "phantom.internal.handleEvaluateJavaScript(" + script + ", " + QString::number(query_id) + ")";
    subBrowser->GetMainFrame()->ExecuteJavaScript(script.toStdString(), "phantomjs://evaluateJavaScript", 1);
    return true;
  } else if (type == QLatin1String("renderPage")) {
    const auto path = json.value(QStringLiteral("path")).toString().toStdString();
    subBrowser->GetHost()->PrintToPDF(path, {}, makePdfPrintCallback([callback] (const CefString& path, bool success) {
      if (success) {
        callback->Success(path);
      } else {
        callback->Failure(1, std::string("failed to print to path ") + path.ToString());
      }
    }));
    return true;
  }
  return false;
}

void PhantomJSHandler::OnQueryCanceled(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                       int64 query_id)
{
  CEF_REQUIRE_UI_THREAD();

  m_pendingOpenBrowserRequests.remove(browser->GetIdentifier());
  m_pendingQueryCallbacks.remove(query_id);
  m_browserSignals.remove(browser->GetIdentifier());
}
